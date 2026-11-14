#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file browserbridge.cpp
 * @brief 有界 HTTP/1.1 单请求解析器。拒绝 chunked、重复头、管线请求和任意 Origin。
 * 只有扩展 ID、Bearer Token、Host 全部有效时才能创建或查询任务。
 */
#include "browserbridge.h"
#include "browserprotocol.h"
namespace {
const int MaxHeader = 16384, MaxBody = 65536;
QJsonObject errorJson(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"] = false;
    o["code"] = code;
    o["message"] = message;
    return o;
}
bool secureEqual(const QByteArray &a, const QByteArray &b) {
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
bool allowedOrigin(const QByteArray &origin) {
    return origin == QByteArray("chrome-extension://") + BrowserProtocol::ExtensionId;
}
} // namespace
/**
 * @brief 装配 GUI 线程中的浏览器连接服务。
 * 从本地配置加载默认目录/并发，以 queued signal 向引擎发命令；只缓存值快照，不跨线程读引擎状态。
 */
BrowserBridge::BrowserBridge(DownloadEngine *engine, const QString &dir, const QString &output,
                             quint16 port, QObject *parent)
    : QObject(parent), engine_(engine), dataDir_(dir), downloadDir_(output), requestedPort_(port),
      connections_(4) {
    QFile config(dir + "/browser-settings.json");
    if (config.open(QIODevice::ReadOnly)) {
        QJsonObject o = QJsonDocument::fromJson(config.read(16384)).object();
        if (downloadDir_.isEmpty())
            downloadDir_ = o["download_dir"].toString();
        connections_ = qBound(1, o["connections"].toInt(4), 64);
    }
    if (downloadDir_.isEmpty())
        downloadDir_ = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    downloadDir_ = QDir(downloadDir_).absolutePath();
    connect(&server_, SIGNAL(newConnection()), this, SLOT(acceptConnections()));
    connect(engine, SIGNAL(ready()), this, SLOT(start()));
    connect(engine, SIGNAL(taskUpdated(Task)), this, SLOT(updateTask(Task)));
    connect(engine, &DownloadEngine::taskRemoved, this,
            [this](const QString &id) { tasks_.remove(id); });
    connect(engine, SIGNAL(browserTaskResult(QString, QString, QString)), this,
            SLOT(taskResult(QString, QString, QString)));
    connect(this, SIGNAL(submit(QString, QString, QString, QString, int)), engine,
            SLOT(addBrowserTask(QString, QString, QString, QString, int)), Qt::QueuedConnection);
}
/**
 * @brief 关闭本机监听并清理此实例的发现文件。
 * 先断开 Socket 捕获 this 的槽，再让 QObject 清理子对象，避免成员容器已经析构后再次触发清理回调。
 */
BrowserBridge::~BrowserBridge() {
    if (server_.isListening()) {
        server_.close();
        QFile::remove(dataDir_ + "/browser-endpoint.json");
    }
    // 客户端 QObject 由本对象拥有；不在析构过程中执行捕获 this 的清理槽。
    const QList<QTcpSocket *> sockets = findChildren<QTcpSocket *>();
    for (int i = 0; i < sockets.size(); ++i)
        sockets[i]->disconnect(this);
}
/**
 * @brief 引擎 ready 后开启 loopback 服务。
 * 读取/生成 DPAPI 保护的 Token，监听指定端口，再原子保存端口与 PID；任何一步失败都不发布可用端点。
 */
void BrowserBridge::start() {
    QString error;
    if (!BrowserProtocol::loadToken(dataDir_, &token_, true, &error) ||
        !QDir().mkpath(downloadDir_)) {
        emit problem(QString::fromUtf8("浏览器连接初始化失败：") + error);
        return;
    }
    server_.setMaxPendingConnections(16);
    if (!server_.listen(QHostAddress::LocalHost, requestedPort_)) {
        emit problem(QString::fromUtf8("浏览器连接端口不可用：") + server_.errorString());
        return;
    }
    QJsonObject info;
    info["port"] = server_.serverPort();
    info["pid"] = QString::number(QCoreApplication::applicationPid());
    info["api_version"] = 1;
    if (!BrowserProtocol::writeJson(dataDir_ + "/browser-endpoint.json", info, &error)) {
        server_.close();
        emit problem(error);
    }
}
bool BrowserBridge::isListening() const {
    return server_.isListening();
}
/**
 * @brief 生成用户主动复制的便携配对 JSON。
 * 包含本机密钥，仅用于交给自己的扩展；不得写进调试日志、URL 查询参数或外部页面。
 */
QString BrowserBridge::pairingText() const {
    if (!isListening())
        return QString();
    QJsonObject o;
    o["endpoint"] = QString("http://127.0.0.1:%1").arg(server_.serverPort());
    o["token"] = QString::fromLatin1(token_);
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}
QString BrowserBridge::downloadDirectory() const {
    return downloadDir_;
}
int BrowserBridge::connections() const {
    return connections_;
}
/**
 * @brief 保存桌面控制的下载 profile。
 * 只接受可写绝对目录；浏览器请求不能调用此方法或改变输出目录，以免远程网页指定任意本地路径。
 */
bool BrowserBridge::setPreferences(const QString &directory, int connections, QString *error) {
    if (!QFileInfo(directory).isAbsolute() || !QDir().mkpath(directory) ||
        !QFileInfo(directory).isWritable()) {
        *error = QString::fromUtf8("下载目录不可写");
        return false;
    }
    QJsonObject o;
    o["download_dir"] = QDir(directory).absolutePath();
    o["connections"] = qBound(1, connections, 64);
    if (!BrowserProtocol::writeJson(dataDir_ + "/browser-settings.json", o, error))
        return false;
    downloadDir_ = o["download_dir"].toString();
    connections_ = o["connections"].toInt();
    return true;
}
/**
 * @brief 接收有数量和时限约束的本机客户端。
 * Socket 由本对象拥有；限制读缓冲、活跃连接和等待时间，断开后删除缓冲与 QObject。
 */
void BrowserBridge::acceptConnections() {
    while (server_.hasPendingConnections()) {
        QTcpSocket *socket = server_.nextPendingConnection();
        socket->setParent(this);
        if (clients_.size() >= 16) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        clients_.insert(socket);
        socket->setReadBufferSize(MaxHeader + MaxBody + 1);
        buffers_[socket] = QByteArray();
        QTimer *deadline = new QTimer(socket);
        deadline->setSingleShot(true);
        deadline->start(10000);
        connect(deadline, &QTimer::timeout, socket, [socket]() { socket->abort(); });
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { consume(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            buffers_.remove(socket);
            clients_.remove(socket);
            socket->deleteLater();
        });
    }
}
/**
 * @brief 增量解析一个有界 HTTP/1.1 请求。
 * 等待完整头与声明的正文长度后才分发；拒绝重复头、折叠头、chunked 和管线数据，防止长度歧义。
 * 不能假定一次 readyRead 就是一整个请求，也不能在未知长度下无限缓存。
 */
void BrowserBridge::consume(QTcpSocket *socket) {
    if (!buffers_.contains(socket))
        return;
    QByteArray &buffer = buffers_[socket];
    buffer += socket->readAll();
    if (buffer.size() > MaxHeader + MaxBody) {
        reject(socket, 413, "TOO_LARGE", QString::fromUtf8("请求过大"));
        return;
    }
    int split = buffer.indexOf("\r\n\r\n");
    if (split < 0) {
        if (buffer.size() > MaxHeader)
            reject(socket, 431, "HEADERS_TOO_LARGE", QString::fromUtf8("请求头过大"));
        return;
    }
    if (split > MaxHeader) {
        reject(socket, 431, "HEADERS_TOO_LARGE", "Header limit exceeded");
        return;
    }
    QList<QByteArray> lines = buffer.left(split).split('\n');
    QList<QByteArray> request = lines.takeFirst().trimmed().split(' ');
    if (request.size() != 3 || request[2] != "HTTP/1.1" || !request[1].startsWith('/')) {
        reject(socket, 400, "BAD_REQUEST", "Invalid request line");
        return;
    }
    QMap<QByteArray, QByteArray> headers;
    for (int i = 0; i < lines.size(); ++i) {
        QByteArray line = lines[i];
        if (line.endsWith('\r'))
            line.chop(1);
        int colon = line.indexOf(':');
        if (colon <= 0 || line.startsWith(' ') || line.startsWith('\t')) {
            reject(socket, 400, "BAD_HEADERS", "Invalid header");
            return;
        }
        QByteArray key = line.left(colon).toLower(), value = line.mid(colon + 1).trimmed();
        if (headers.contains(key) || key.contains(' ') || key.contains('\t') ||
            value.contains('\r')) {
            reject(socket, 400, "BAD_HEADERS", "Duplicate or invalid header");
            return;
        }
        headers[key] = value;
    }
    if (headers.contains("transfer-encoding")) {
        reject(socket, 400, "UNSUPPORTED_ENCODING", "Chunked requests are not supported");
        return;
    }
    bool ok = true;
    qlonglong length = 0;
    if (headers.contains("content-length")) {
        QByteArray value = headers["content-length"];
        ok = !value.isEmpty();
        for (int i = 0; i < value.size(); ++i)
            ok = ok && value[i] >= '0' && value[i] <= '9';
        bool converted = false;
        length = value.toLongLong(&converted);
        ok = ok && converted;
    }
    if (!ok || length < 0 || length > MaxBody) {
        reject(socket, 413, "BAD_LENGTH", "Invalid content length");
        return;
    }
    if (request[0] == "POST" && !headers.contains("content-length")) {
        reject(socket, 411, "LENGTH_REQUIRED", "Content-Length required");
        return;
    }
    if (buffer.size() < split + 4 + length)
        return;
    if (buffer.size() != split + 4 + length) {
        reject(socket, 400, "PIPELINING_DISABLED", "Only one request is allowed");
        return;
    }
    const QByteArray body = buffer.mid(split + 4);
    buffers_.remove(socket);
    socket->setProperty("qdm_origin", headers.value("origin"));
    dispatch(socket, request[0], request[1], headers, body);
}
/**
 * @brief 发送单个 JSON 响应并关闭连接。
 * 只向允许的扩展 Origin 添加 CORS 响应头；所有结果 no-store，不暴露任务 URL 或密钥。
 */
void BrowserBridge::respond(QTcpSocket *socket, int status, const QJsonObject &body,
                            const QByteArray &requestedOrigin) {
    if (!socket || socket->state() != QAbstractSocket::ConnectedState)
        return;
    buffers_.remove(socket);
    QByteArray data = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray headers = "HTTP/1.1 " + QByteArray::number(status) +
                         " QDM\r\nContent-Type: application/json; charset=utf-8\r\nConnection: "
                         "close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    QByteArray origin =
        requestedOrigin.isEmpty() ? socket->property("qdm_origin").toByteArray() : requestedOrigin;
    if (allowedOrigin(origin))
        headers += "Access-Control-Allow-Origin: " + origin +
                   "\r\nVary: Origin\r\nAccess-Control-Allow-Methods: GET, POST, "
                   "OPTIONS\r\nAccess-Control-Allow-Headers: authorization, content-type, "
                   "x-qdm-extension-id\r\n";
    headers += "Content-Length: " + QByteArray::number(data.size()) + "\r\n\r\n";
    socket->write(headers + data);
    socket->disconnectFromHost();
}
/**
 * @brief 统一生成错误响应。
 * 错误码供扩展决定提示或回退，错误文本不得包含 Bearer Token、Cookie 或完整签名 URL。
 */
void BrowserBridge::reject(QTcpSocket *s, int status, const QString &code, const QString &message) {
    respond(s, status, errorJson(code, message));
}
/**
 * @brief 生成面向扩展的最小任务快照。
 * 字节量用十进制字符串保留 64 位精度，未知总长为 null；不返回服务器 URL 和本地绝对路径。
 */
QJsonObject BrowserBridge::taskJson(const Task &t) const {
    QJsonObject o;
    o["ok"] = true;
    o["task_id"] = t.id;
    o["state"] = t.state;
    o["name"] = QFileInfo(t.path).fileName();
    o["received"] = QString::number(t.received);
    o["total"] = t.total < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::number(t.total));
    return o;
}
/**
 * @brief 验证授权并路由本机 API。
 * Host、Origin、扩展 ID、Token 均通过后才处理 JSON；再限制字段、URL、profile 和清单类型。
 * 幂等键已存在时返回原任务；新任务只预留安全文件名，持久化交由下载线程完成后再应答。
 */
void BrowserBridge::dispatch(QTcpSocket *s, const QByteArray &method, const QByteArray &target,
                             const QMap<QByteArray, QByteArray> &h, const QByteArray &body) {
    if (h.value("host") != QByteArray("127.0.0.1:") + QByteArray::number(server_.serverPort())) {
        reject(s, 403, "BAD_HOST", "Loopback Host required");
        return;
    }
    const QByteArray origin = h.value("origin");
    if (!origin.isEmpty() && !allowedOrigin(origin)) {
        reject(s, 403, "BAD_ORIGIN", QString::fromUtf8("仅允许 QDM 扩展调用"));
        return;
    }
    if (method == "OPTIONS") {
        if (!allowedOrigin(origin)) {
            reject(s, 403, "BAD_ORIGIN", "Extension origin required");
            return;
        }
        QJsonObject ok;
        ok["ok"] = true;
        respond(s, 200, ok, origin);
        return;
    }
    if (!BrowserProtocol::validExtension(h.value("x-qdm-extension-id")) ||
        !secureEqual(h.value("authorization"), QByteArray("Bearer ") + token_)) {
        reject(s, 401, "UNAUTHORIZED", QString::fromUtf8("配对信息无效，请重新连接桌面程序"));
        return;
    }
    if (method == "GET" && !body.isEmpty()) {
        reject(s, 400, "BAD_BODY", "GET body is not supported");
        return;
    }
    if (method == "GET" && target == "/api/v1/health") {
        QJsonObject o;
        o["ok"] = true;
        o["api_version"] = 1;
        o["version"] = QCoreApplication::applicationVersion();
        o["profile_id"] = "default";
        o["connections"] = connections_;
        QJsonArray capabilities;
        capabilities.append("http_download");
        o["capabilities"] = capabilities;
        respond(s, 200, o);
        return;
    }
    if (method == "GET" && target.startsWith("/api/v1/tasks/")) {
        QString id = QString::fromLatin1(target.mid(14));
        if (!tasks_.contains(id)) {
            reject(s, 404, "NOT_FOUND", "Task not found");
            return;
        }
        respond(s, 200, taskJson(tasks_[id]));
        return;
    }
    if (method != "POST" || target != "/api/v1/tasks") {
        reject(s, 404, "NOT_FOUND", "Endpoint not found");
        return;
    }
    if (h.value("content-type").split(';').first().trimmed().toLower() != "application/json") {
        reject(s, 415, "JSON_REQUIRED", "Use application/json");
        return;
    }
    QJsonParseError parse;
    QJsonDocument document = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        reject(s, 400, "BAD_JSON", "Invalid JSON object");
        return;
    }
    const QJsonObject o = document.object();
    const QStringList fields = QStringList() << "request_id"
                                             << "url"
                                             << "suggested_name"
                                             << "profile_id"
                                             << "source_page";
    const QStringList keys = o.keys();
    for (int i = 0; i < keys.size(); ++i)
        if (!fields.contains(keys[i]) || !o[keys[i]].isString()) {
            reject(s, 400, "BAD_FIELDS", QString::fromUtf8("不支持的参数，浏览器不能指定本地目录"));
            return;
        }
    const QString id = o["request_id"].toString(), raw = o["url"].toString();
    QUrl url(raw);
    QRegExp requestId("^[A-Za-z0-9_-]{8,80}$");
    if (!requestId.exactMatch(id) || raw.size() > 8192 || !url.isValid() ||
        (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty() ||
        !url.userInfo().isEmpty()) {
        reject(s, 400, "BAD_URL", QString::fromUtf8("地址或请求编号无效"));
        return;
    }
    if (o["suggested_name"].toString().size() > 512 || o["source_page"].toString().size() > 8192 ||
        (!o["profile_id"].toString().isEmpty() && o["profile_id"].toString() != "default")) {
        reject(s, 400, "BAD_PROFILE", "Invalid profile or filename");
        return;
    }
    if (QRegExp(".*\\.(m3u8|mpd)$", Qt::CaseInsensitive).exactMatch(url.path())) {
        reject(s, 422, "MEDIA_NOT_SUPPORTED",
               QString::fromUtf8("已识别流媒体清单，桌面版尚未支持合并下载"));
        return;
    }
    QString name = o["suggested_name"].toString();
    if (name.isEmpty())
        name = url.fileName();
    name = BrowserProtocol::cleanFileName(name);
    QJsonObject identity;
    identity["url"] = url.toString(QUrl::FullyEncoded);
    identity["name"] = name;
    identity["profile"] = "default";
    QString fingerprint = QString::fromLatin1(
        QCryptographicHash::hash(QJsonDocument(identity).toJson(QJsonDocument::Compact),
                                 QCryptographicHash::Sha256)
            .toHex());
    for (QHash<QString, Task>::const_iterator it = tasks_.constBegin(); it != tasks_.constEnd();
         ++it)
        if (it.value().requestId == id) {
            if (it.value().requestFingerprint != fingerprint)
                reject(s, 409, "REQUEST_CONFLICT", "Request ID reused with different data");
            else
                respond(s, 200, taskJson(it.value()));
            return;
        }
    if (pending_.contains(id)) {
        if (pending_[id].fingerprint != fingerprint)
            reject(s, 409, "REQUEST_CONFLICT", "Request ID reused with different data");
        else
            pending_[id].clients.append(QPointer<QTcpSocket>(s));
        return;
    }
    if (pending_.size() >= 16) {
        reject(s, 429, "BUSY", "Too many pending tasks");
        return;
    }
    const QString path = uniquePath(name);
    if (path.isEmpty()) {
        reject(s, 409, "NAME_CONFLICT", QString::fromUtf8("无法分配输出文件名"));
        return;
    }
    Pending p;
    p.fingerprint = fingerprint;
    p.path = path;
    p.clients.append(QPointer<QTcpSocket>(s));
    pending_[id] = p;
    reservations_.insert(path.toLower());
    emit submit(id, fingerprint, url.toString(QUrl::FullyEncoded), path, connections_);
}
/**
 * @brief 在受控 profile 中分配不会覆盖已有任务/文件的目标名。
 * 同时检查磁盘、已知任务与尚在等待引擎确认的预留路径；数字后缀不参与请求摘要，避免重试冲突。
 */
QString BrowserBridge::uniquePath(const QString &name) const {
    QFileInfo info(name);
    QString base = info.completeBaseName(), suffix = info.suffix();
    for (int i = 0; i < 1000; ++i) {
        QString n =
            i ? base + QString(" (%1)").arg(i) + (suffix.isEmpty() ? QString() : "." + suffix)
              : name;
        QString path = QDir(downloadDir_).absoluteFilePath(n);
        bool occupied = reservations_.contains(path.toLower()) || QFile::exists(path);
        for (QHash<QString, Task>::const_iterator it = tasks_.constBegin();
             it != tasks_.constEnd() && !occupied; ++it)
            occupied = it.value().path.compare(path, Qt::CaseInsensitive) == 0;
        if (!occupied)
            return path;
    }
    return QString();
}
/**
 * @brief 接收引擎已复制的任务快照。
 * GUI 线程维护查询缓存，并将预留路径转换为已知任务占用；这里不修改数据库或执行下载。
 */
void BrowserBridge::updateTask(const Task &task) {
    tasks_[task.id] = task;
    reservations_.remove(task.path.toLower());
}
/**
 * @brief 完成浏览器任务交接。
 * 只有引擎返回成功 ID 后才唤起软件并向客户端发送确认；Socket 用 QPointer
 * 保存，客户端断开也不会访问悬空指针。
 */
void BrowserBridge::taskResult(const QString &id, const QString &taskId, const QString &error) {
    if (!pending_.contains(id))
        return;
    const Pending p = pending_.take(id);
    reservations_.remove(p.path.toLower());
    if (!taskId.isEmpty())
        emit taskAccepted(taskId);
    for (int i = 0; i < p.clients.size(); ++i)
        if (p.clients[i]) {
            if (taskId.isEmpty())
                reject(p.clients[i], 400, "CREATE_FAILED", error);
            else if (tasks_.contains(taskId))
                respond(p.clients[i], 201, taskJson(tasks_[taskId]));
            else {
                QJsonObject o;
                o["ok"] = true;
                o["task_id"] = taskId;
                o["state"] = "Queued";
                respond(p.clients[i], 201, o);
            }
        }
}
