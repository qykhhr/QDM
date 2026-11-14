#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file downloadengine.cpp
 * @brief libcurl multi 下载、分段响应校验与文件/SQLite 检查点协议。
 * 一个工作线程拥有所有可变状态。回调同步消费单块数据，内存不随文件增长。
 * 回调中不销毁 CURL；错误由 pump 在 socket_action 返回后的安全点统一处理。
 */
#include "downloadengine.h"
#include <io.h>
#include <windows.h>

struct Transfer {
    DownloadEngine *owner;
    CURL *easy;
    bool probe, headersChecked;
    int segment;
    long status;
    qint64 start, end, total, length, rangeBegin, rangeEnd;
    QString etag, modified, encoding, error;
    QByteArray range;
    curl_slist *headers;
    bool restartAsStream;
    int retryAfterSeconds;
    char curlError[CURL_ERROR_SIZE];
    Transfer()
        : owner(0), easy(0), probe(false), headersChecked(false), segment(-1), status(0), start(0),
          end(-1), total(-1), length(-1), rangeBegin(-1), rangeEnd(-1), headers(0),
          restartAsStream(false), retryAfterSeconds(0) {
        memset(curlError, 0, sizeof(curlError));
    }
};

/** Qt 5.8 的 activated 信号只带 int，而 Windows SOCKET 是指针宽度。
 * 连接时捕获原始 curl_socket_t，不从信号的 int 参数重建 socket。
 */
struct CurlSocketWatch {
    QSocketNotifier *read;
    QSocketNotifier *write;
    quint64 serial;
    CurlSocketWatch() : read(0), write(0), serial(0) {}
};

/**
 * @brief 构造尚未启动的下载引擎。
 * 只保存配置和值状态，不在 GUI 线程打开数据库或创建网络定时器；这些资源必须等 moveToThread 后由
 * initialize 创建。
 */
DownloadEngine::DownloadEngine(const QString &dir, const QString &caFile, QObject *parent)
    : QObject(parent), dataDir_(dir), caFile_(caFile), multi_(0), timer_(0), curlTimer_(0),
      nextSocketSerial_(0), lastUi_(0), lastCheckpoint_(0), lastSpeedTime_(0), lastSpeedBytes_(0),
      initialized_(false), stopping_(false), inTick_(false) {}
/**
 * @brief 释放下载引擎。
 * 正常退出已经执行 shutdown；再次调用必须幂等，不能重复释放 curl 全局环境或仍在使用的文件。
 */
DownloadEngine::~DownloadEngine() {
    shutdown();
}
/**
 * @brief 计算本任务专属临时文件路径。
 * 临时文件与目标同目录，UUID 防止不同任务相互覆盖；完成时可同卷重命名，不再复制整个大文件。
 */
QString DownloadEngine::partPath(const Task &t) const {
    return t.path + ".qdm-" + t.id + ".part";
}
/**
 * @brief 在工作线程内查找任务。
 * 返回的是 tasks_ 容器中的借用指针；只能在当前串行操作中使用，不可传给 GUI 或跨容器修改长期保存。
 */
Task *DownloadEngine::find(const QString &id) {
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == id)
            return &tasks_[i];
    return 0;
}
/**
 * @brief 发布不可变的任务值快照。
 * Qt 队列连接负责复制数据，界面只能展示快照；实时 received 可能领先已持久化的 committed。
 */
void DownloadEngine::publish(Task &t) {
    emit taskUpdated(t);
}
/**
 * @brief 保存单个任务的原子数据库快照。
 * 调用者负责保证 committed
 * 所代表的数据已经可靠落盘；失败向应用报告错误，不把内存进度当作持久化成功。
 */
bool DownloadEngine::persist(Task &t) {
    QString error;
    if (repository_.save(t, &error))
        return true;
    emit problem(QString::fromUtf8("数据库保存失败：") + error);
    return false;
}

/**
 * @brief 在下载线程初始化全部有线程归属的资源。
 * 先打开并迁移数据库，再初始化 curl 和 socket 驱动，最后加载任务并发 ready。
 * 非终态任务先恢复为暂停，避免未经资源身份及文件摘要验证就自动拼接旧数据。
 */
void DownloadEngine::initialize() {
    if (initialized_)
        return;
    QString error;
    if (!QDir().mkpath(dataDir_) || !repository_.open(dataDir_ + "/tasks.sqlite", &error) ||
        !repository_.load(&tasks_, &error)) {
        emit problem(QString::fromUtf8("初始化失败：") + error);
        return;
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        emit problem("curl_global_init failed");
        return;
    }
    multi_ = curl_multi_init();
    if (!multi_) {
        curl_global_cleanup();
        emit problem("curl_multi_init failed");
        return;
    }
    curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, 64L);
    curl_multi_setopt(multi_, CURLMOPT_MAX_HOST_CONNECTIONS, 64L);
    // 使用 socket 就绪和 libcurl 超时共同驱动，消除固定 20 ms 的接收等待。
    // curlTimer 必须在注册回调及添加 easy 之前创建。
    curlTimer_ = new QTimer(this);
    curlTimer_->setSingleShot(true);
    curlTimer_->setTimerType(Qt::PreciseTimer);
    connect(curlTimer_, SIGNAL(timeout()), this, SLOT(curlTimeout()));
    curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, &DownloadEngine::socketCallback);
    curl_multi_setopt(multi_, CURLMOPT_SOCKETDATA, this);
    curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, &DownloadEngine::timerCallback);
    curl_multi_setopt(multi_, CURLMOPT_TIMERDATA, this);
    initialized_ = true;
    clock_.start();
    for (int i = 0; i < tasks_.size(); ++i) {
        Task &t = tasks_[i];
        if (t.state == "Finalizing") {
            // 重命名成功后可能来不及提交 Completed。仅在最终文件的长度及
            // 每个已提交区间摘要都正确时恢复完成，绝不以“文件存在”代替验证。
            if (!QFile::exists(partPath(t)) && QFile::exists(t.path)) {
                QFile output(t.path);
                bool valid = output.open(QIODevice::ReadOnly) && output.size() == t.total &&
                             t.committed == t.total && !t.segments.isEmpty();
                qint64 cursor = 0;
                for (int j = 0; j < t.segments.size() && valid; ++j) {
                    const Segment &s = t.segments[j];
                    if (s.begin != cursor || s.written < s.begin ||
                        (s.end >= 0 && s.written != s.end)) {
                        valid = false;
                        break;
                    }
                    QCryptographicHash hash(QCryptographicHash::Sha256);
                    qint64 pos = s.begin;
                    valid = output.seek(pos);
                    while (valid && pos < s.written) {
                        QByteArray b = output.read(qMin<qint64>(1024 * 1024, s.written - pos));
                        if (b.isEmpty()) {
                            valid = false;
                            break;
                        }
                        hash.addData(b);
                        pos += b.size();
                    }
                    valid = valid && hash.result() == s.digest;
                    cursor = s.written;
                }
                valid = valid && cursor == t.total;
                t.state = valid ? "Completed" : "Failed";
                t.error = valid ? QString()
                                : QString::fromUtf8("完成发布恢复校验失败；最终文件保留，请核对");
                persist(t);
            }
        }
        if (t.state != "Completed" && t.state != "Failed")
            t.state = "Paused";
        t.received = t.committed;
        t.speed = 0;
        publish(t);
    }
    timer_ = new QTimer(this);
    connect(timer_, SIGNAL(timeout()), this, SLOT(tick()));
    timer_->start(100);
    emit ready();
}

/**
 * @brief 接收界面的新建任务命令。
 * 共享 enqueueTask 完成校验和持久化；错误发给 GUI，不通过返回值假装同步完成下载。
 */
void DownloadEngine::addTask(const QString &url, const QString &path, int connections) {
    QString error;
    if (enqueueTask(url, path, connections, &error).isEmpty())
        emit problem(error);
}
/**
 * @brief 接收本机桥转发的浏览器任务。
 * requestId 和 fingerprint 用于幂等；只有数据库保存成功后才发
 * browserTaskResult，扩展据此决定能否停止浏览器原下载。
 */
void DownloadEngine::addBrowserTask(const QString &requestId, const QString &fingerprint,
                                    const QString &url, const QString &path, int connections) {
    QString error;
    QString id = enqueueTask(url, path, connections, &error, requestId, fingerprint);
    emit browserTaskResult(requestId, id, error);
}
/**
 * @brief 统一创建任务，返回已持久化的任务 UUID，失败返回空串并填写 error。
 * 先检查浏览器幂等键，再检查路径：重试时原目标可能已经存在，仍应返回原任务，不能误判为文件冲突。
 * 网络请求只在任务保存、加入队列之后开始；浏览器与手工下载复用同一条业务路径。
 */
QString DownloadEngine::enqueueTask(const QString &url, const QString &path, int connections,
                                    QString *error, const QString &requestId,
                                    const QString &fingerprint) {
    if (!requestId.isEmpty())
        for (int i = 0; i < tasks_.size(); ++i)
            if (tasks_[i].requestId == requestId) {
                if (tasks_[i].requestFingerprint == fingerprint)
                    return tasks_[i].id;
                *error = QString::fromUtf8("幂等请求内容冲突");
                return QString();
            }

    if (!initialized_ || stopping_) {
        *error = (QString::fromUtf8("下载引擎尚未就绪"));
        return QString();
    }
    QUrl u(url);
    QFileInfo info(path);
    if (!u.isValid() || (u.scheme() != "http" && u.scheme() != "https") || u.host().isEmpty() ||
        !u.userInfo().isEmpty()) {
        *error = (QString::fromUtf8("请输入有效的 HTTP/HTTPS 地址；不接受 URL 内嵌账号密码"));
        return QString();
    }
    QString name = info.fileName();
    QRegExp invalid("[<>:\"/\\\\|?*\\x00-\\x1f]");
    QRegExp reserved("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(\\..*)?$", Qt::CaseInsensitive);
    if (!info.isAbsolute() || name.isEmpty() || name.contains(invalid) ||
        reserved.exactMatch(name) || name.endsWith('.') || name.endsWith(' ') ||
        QFile::exists(info.absoluteFilePath())) {
        *error = (QString::fromUtf8("输出路径必须是尚不存在的绝对文件路径"));
        return QString();
    }
    QString normalized = QDir::cleanPath(info.absoluteFilePath());
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].path.compare(normalized, Qt::CaseInsensitive) == 0) {
            *error = (QString::fromUtf8("已有任务使用此输出路径"));
            return QString();
        }
    if (!QDir().mkpath(info.absolutePath())) {
        *error = (QString::fromUtf8("无法创建输出目录"));
        return QString();
    }
    Task t;
    t.requestId = requestId;
    t.requestFingerprint = fingerprint;
    t.id = QUuid::createUuid().toString().remove('{').remove('}');
    t.url = u.toString(QUrl::FullyEncoded);
    t.path = normalized;
    t.connections = qBound(1, connections, 64);
    t.state = "Queued";
    if (!persist(t)) {
        *error = QString::fromUtf8("无法保存任务");
        return QString();
    }
    tasks_.append(t);
    publish(tasks_.last());
    startNext();
    return t.id;
}

/**
 * @brief 从队列选择下一项任务。
 * 当前版本只允许一个活跃任务，每个任务内部可并发多个分段；任务状态全部在同一线程串行修改。
 */
void DownloadEngine::startNext() {
    if (stopping_ || !activeId_.isEmpty() || !initialized_)
        return;
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].state == "Queued") {
            activeId_ = tasks_[i].id;
            startProbe(tasks_[i]);
            return;
        }
}

/**
 * @brief 为探测或一个分段创建 easy handle，并交给 multi 管理。
 * URL/Range 字符串由 curl 复制，额外请求头由 Transfer 持有并统一释放；不跨线程暴露 CURL 指针。
 * 分段内部采用半开区间，发送 HTTP Range 时将 end 转成 end-1；证书链和主机名验证始终开启。
 */
Transfer *DownloadEngine::createTransfer(int segment, bool probe) {
    Task *task = find(activeId_);
    if (!task)
        return 0;
    Transfer *t = new Transfer;
    t->owner = this;
    t->segment = segment;
    t->probe = probe;
    t->easy = curl_easy_init();
    if (!t->easy) {
        delete t;
        return 0;
    }
    CURL *e = t->easy;
    curl_easy_setopt(e, CURLOPT_URL, task->url.toUtf8().constData());
    curl_easy_setopt(e, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(e, CURLOPT_REDIR_PROTOCOLS_STR,
                     task->url.startsWith("https:") ? "https" : "http,https");
    curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(e, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(e, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(e, CURLOPT_USERAGENT, "QDM/0.1");
    curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(e, CURLOPT_HTTP_CONTENT_DECODING, 0L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    // 官方 Windows 包使用 LibreSSL；显式启用 Windows 系统 CA。
    // 包内没有独立 CA 文件，不能设置不存在的 CAINFO 路径。
    curl_easy_setopt(e, CURLOPT_SSL_OPTIONS, long(CURLSSLOPT_NATIVE_CA));
    if (!caFile_.isEmpty()) {
        QByteArray ca = QDir::toNativeSeparators(caFile_).toUtf8();
        curl_easy_setopt(e, CURLOPT_CAINFO, ca.constData());
    }
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, &DownloadEngine::writeCallback);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, t);
    curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, &DownloadEngine::headerCallback);
    curl_easy_setopt(e, CURLOPT_HEADERDATA, t);
    curl_easy_setopt(e, CURLOPT_PRIVATE, t);
    curl_easy_setopt(e, CURLOPT_ERRORBUFFER, t->curlError);
    if (probe) {
        t->range = "0-0";
        curl_easy_setopt(e, CURLOPT_RANGE, t->range.constData());
    } else if (task->ranges) {
        Segment &s = task->segments[segment];
        t->start = s.written;
        t->end = s.end;
        t->range = QByteArray::number(t->start) + "-" + QByteArray::number(t->end - 1);
        curl_easy_setopt(e, CURLOPT_RANGE, t->range.constData());
        QString validator = !task->etag.isEmpty() ? task->etag : task->modified;
        if (!validator.isEmpty()) {
            QByteArray header = "If-Range: " + validator.toLatin1();
            t->headers = curl_slist_append(0, header.constData());
            curl_easy_setopt(e, CURLOPT_HTTPHEADER, t->headers);
        }
    }
    transfers_.append(t);
    if (curl_multi_add_handle(multi_, e) != CURLM_OK) {
        transfers_.removeOne(t);
        curl_easy_cleanup(e);
        curl_slist_free_all(t->headers);
        delete t;
        return 0;
    }
    return t;
}

/**
 * @brief 把任务置为探测状态并保存，然后发出 bytes=0-0 请求。
 * 不能仅相信 Accept-Ranges 或 HEAD；必须检查服务器实际响应是否允许安全分段。
 */
void DownloadEngine::startProbe(Task &t) {
    t.state = "Probing";
    t.error.clear();
    t.speed = 0;
    if (!persist(t)) {
        activeId_.clear();
        t.state = "Failed";
        t.error = QString::fromUtf8("无法持久化任务");
        publish(t);
        return;
    }
    publish(t);
    if (!createTransfer(-1, true))
        stopActive("Failed", QString::fromUtf8("无法创建探测请求"));
}

/**
 * @brief 解析当前 HTTP 响应头，回调仅在网络线程执行。
 * 代理 CONNECT、重定向和最终响应各自重新清空元信息，避免继承上一跳的长度或 ETag。
 * 只记录状态与范围，不在回调中释放当前请求；正文到达前仍须由 writeCallback 验证。
 */
size_t DownloadEngine::headerCallback(char *data, size_t size, size_t count, void *context) {
    Transfer *t = static_cast<Transfer *>(context);
    const size_t n = size * count;
    QByteArray line(data, int(n));
    line = line.trimmed();
    if (line.startsWith("HTTP/")) {
        // 每个重定向/代理 CONNECT/最终响应都是独立头集合，不继承前一跳字段。
        QList<QByteArray> p = line.split(' ');
        t->status = p.size() > 1 ? p[1].toLong() : 0;
        t->length = -1;
        t->total = -1;
        t->rangeBegin = -1;
        t->rangeEnd = -1;
        t->etag.clear();
        t->modified.clear();
        t->encoding.clear();
        t->headersChecked = false;
        t->retryAfterSeconds = 0;
    } else {
        int c = line.indexOf(':');
        if (c < 0)
            return n;
        QByteArray key = line.left(c).toLower(), value = line.mid(c + 1).trimmed();
        if (key == "content-length") {
            bool ok = false;
            qint64 v = value.toLongLong(&ok);
            if (ok && v >= 0)
                t->length = v;
        } else if (key == "etag")
            t->etag = QString::fromLatin1(value);
        else if (key == "last-modified")
            t->modified = QString::fromLatin1(value);
        else if (key == "content-encoding")
            t->encoding = QString::fromLatin1(value).toLower();
        else if (key == "retry-after") {
            bool ok = false;
            qint64 delay = value.toLongLong(&ok);
            if (!ok) {
                time_t date = curl_getdate(value.constData(), 0);
                delay = date < 0 ? 0 : qint64(date) - QDateTime::currentDateTimeUtc().toTime_t();
            }
            t->retryAfterSeconds = int(qBound<qint64>(0, delay, 3600));
        } else if (key == "content-range") {
            QRegExp r("bytes ([0-9]+)-([0-9]+)/([0-9]+)");
            if (r.exactMatch(QString::fromLatin1(value))) {
                bool a, b, c;
                t->rangeBegin = r.cap(1).toLongLong(&a);
                t->rangeEnd = r.cap(2).toLongLong(&b);
                t->total = r.cap(3).toLongLong(&c);
                if (!a || !b || !c)
                    t->total = -1;
            } else if (value == "bytes */0")
                t->total = 0;
        }
    }
    return n;
}

/**
 * @brief 接收一个有界正文块并写入指定分段。
 * 先验证状态码、Content-Range、资源身份及内容编码，再 seek/write，最后更新哈希和实时计数。
 * 返回字节数表示完整消费，返回 0 让 curl 停止请求；任何不匹配响应都不能写进已有偏移。
 * 这里的 written 尚不是断点承诺，只有 checkpoint 成功后才成为可恢复进度。
 */
size_t DownloadEngine::writeCallback(char *data, size_t size, size_t count, void *context) {
    Transfer *x = static_cast<Transfer *>(context);
    DownloadEngine *self = x->owner;
    size_t n = size * count;
    if (x->status >= 300 && x->status < 400)
        return n; // 跳转正文不进入输出文件。
    if (x->probe) {
        return x->status == 206 && n <= 1 ? n : 0;
    }
    Task *t = self->find(self->activeId_);
    if (!t)
        return 0;
    if (!x->headersChecked) {
        bool encoded = !x->encoding.isEmpty() && x->encoding != "identity";
        if (encoded) {
            x->error = QString::fromUtf8("服务器未返回 identity 表示，不能按原字节写入");
            return 0;
        }
        if (t->ranges) {
            if (x->status == 200) {
                x->restartAsStream = true;
                return 0;
            }
            if (x->status != 206 || x->rangeBegin != x->start || x->rangeEnd != x->end - 1 ||
                x->total != t->total) {
                x->error = QString::fromUtf8("分段响应不匹配，已停止写入；请重新开始任务");
                return 0;
            }
            if ((!t->etag.isEmpty() && x->etag != t->etag) ||
                (!t->modified.isEmpty() && x->modified != t->modified)) {
                x->error = QString::fromUtf8("传输期间资源身份变化，拒绝拼接");
                return 0;
            }
        } else if (x->status != 200) {
            x->error = QString("HTTP %1").arg(x->status);
            return 0;
        }
        x->headersChecked = true;
    }
    Segment &s = t->segments[x->segment];
    if (n > size_t(INT_MAX) || (s.end >= 0 && qint64(n) > s.end - s.written)) {
        x->error = QString::fromUtf8("响应正文超出预期区间");
        return 0;
    }
    if (!self->file_.seek(s.written) || self->file_.write(data, qint64(n)) != qint64(n)) {
        x->error = QString::fromUtf8("文件写入失败：") + self->file_.errorString();
        return 0;
    }
    self->hashes_[x->segment]->addData(data, int(n));
    s.written += qint64(n);
    t->received += qint64(n);
    return n;
}

/**
 * @brief 根据真实探测结果决定分段、单流或重试。
 * 只有总长度一致、Range 可用且强 ETag/Last-Modified
 * 相符时才考虑复用旧分段；本地内容还要继续校验摘要。
 * 探测句柄先统一清理，再准备文件与正式传输，避免回调上下文成为悬空指针。
 */
void DownloadEngine::finishProbe(Transfer *x, CURLcode code) {
    Task *t = find(activeId_);
    if (!t)
        return;
    const bool rangeOk = x->status == 206 && x->rangeBegin == 0 && x->rangeEnd == 0 &&
                         x->total > 0 && code == CURLE_OK;
    const bool emptyOk = x->status == 416 && x->total == 0;
    const bool streamOk =
        (x->status == 200 || emptyOk) && (code == CURLE_OK || code == CURLE_WRITE_ERROR);
    if ((!rangeOk && !streamOk) || (!x->encoding.isEmpty() && x->encoding != "identity")) {
        QString why = x->status ? QString("HTTP %1 / invalid probe response").arg(x->status)
                                : QString::fromLatin1(curl_easy_strerror(code));
        bool transient = x->status == 429 || x->status == 503 || x->status >= 500 ||
                         code == CURLE_COULDNT_CONNECT || code == CURLE_OPERATION_TIMEDOUT;
        stopActive("Failed", why, transient, x->retryAfterSeconds);
        return;
    }
    const qint64 total = (rangeOk || emptyOk) ? x->total : x->length;
    // 没有可靠验证器、长度改变或 Range 不可用时，旧文件字节不再可复用。
    const bool strong = !x->etag.isEmpty() && !x->etag.startsWith("W/");
    const bool identity = strong ? (!t->etag.isEmpty() && t->etag == x->etag)
                                 : (!x->modified.isEmpty() && t->modified == x->modified);
    bool allowRanges = rangeOk && !forceSingle_.contains(t->id);
    bool reuse =
        allowRanges && t->ranges && identity && total == t->total && !t->segments.isEmpty();
    t->total = total;
    t->ranges = allowRanges;
    t->etag = strong ? x->etag : QString();
    t->modified = x->modified;
    clearTransfers();
    if (!reuse) {
        t->segments.clear();
        t->committed = 0;
        t->received = 0;
    }
    if (!prepareFile(*t))
        return;
    t->state = "Downloading";
    lastCheckpoint_ = clock_.elapsed();
    lastSpeedTime_ = lastCheckpoint_;
    lastSpeedBytes_ = t->received;
    if (!persist(*t)) {
        stopActive("Failed", QString::fromUtf8("无法保存下载状态"));
        return;
    }
    publish(*t);
    for (int i = 0; i < t->segments.size(); ++i) {
        if (t->segments[i].end >= 0 && t->segments[i].written == t->segments[i].end)
            continue;
        if (!createTransfer(i, false)) {
            stopActive("Failed", QString::fromUtf8("无法创建传输"));
            return;
        }
    }
    if (transfers_.isEmpty())
        finishTask();
}

/**
 * @brief 打开临时文件并重建每段哈希状态。
 * 验证区间连续、不重叠、边界合法，以及已提交前缀 SHA-256；文件长度正确并不意味着数据已下载。
 * 任何检查点不可信时清空旧数据重新规划，不混合新旧资源；大文件恢复校验仍可能占用工作线程较久。
 */
bool DownloadEngine::prepareFile(Task &t) {
    file_.setFileName(partPath(t));
    if (QFile::exists(t.path)) {
        stopActive("Failed", QString::fromUtf8("目标文件已存在，拒绝覆盖"));
        return false;
    }
    if (!file_.open(QIODevice::ReadWrite)) {
        stopActive("Failed", file_.errorString());
        return false;
    }
    bool valid = !t.segments.isEmpty();
    qint64 cursor = 0;
    for (int i = 0; i < t.segments.size() && valid; ++i) {
        const Segment &s = t.segments[i];
        valid = s.begin == cursor && s.end > s.begin && s.end <= t.total && s.written >= s.begin &&
                s.written <= s.end && file_.size() >= s.written;
        cursor = s.end;
    }
    valid = valid && cursor == t.total && t.segments.size() <= 64;
    t.state = "Recovering";
    publish(t);
    if (valid) {
        for (int i = 0; i < t.segments.size(); ++i) {
            const Segment &s = t.segments[i];
            QCryptographicHash *h = new QCryptographicHash(QCryptographicHash::Sha256);
            hashes_.append(h);
            qint64 pos = s.begin;
            file_.seek(pos);
            while (pos < s.written) {
                QByteArray b = file_.read(qMin<qint64>(1024 * 1024, s.written - pos));
                if (b.isEmpty()) {
                    valid = false;
                    break;
                }
                h->addData(b);
                pos += b.size();
            }
            if (!valid || h->result() != s.digest) {
                valid = false;
                break;
            }
        }
    }
    if (!valid) {
        qDeleteAll(hashes_);
        hashes_.clear();
        t.segments.clear();
        t.committed = 0;
        t.received = 0;
        // resize(0) 去除旧资源内容。不得将预分配长度当作下载进度。
        if (!file_.resize(0)) {
            stopActive("Failed", file_.errorString());
            return false;
        }
        int count = t.ranges
                        ? qMin(t.connections,
                               int(qMax<qint64>(1, qMin<qint64>(64, t.total / (8 * 1024 * 1024)))))
                        : 1;
        for (int i = 0; i < count; ++i) {
            Segment s;
            s.begin = t.ranges ? (t.total / count) * i : 0;
            s.end = t.ranges ? (i == count - 1 ? t.total : (t.total / count) * (i + 1)) : t.total;
            s.written = s.begin;
            t.segments.append(s);
            hashes_.append(new QCryptographicHash(QCryptographicHash::Sha256));
        }
    }
    t.received = 0;
    for (int i = 0; i < t.segments.size(); ++i)
        t.received += t.segments[i].written - t.segments[i].begin;
    return true;
}

/**
 * @brief 在 curl 回调之外停止并释放所有传输。
 * 必须先从 multi 移除，再清理 easy 与请求头；移除过程可能同步通知 socket 监视器取消订阅。
 */
void DownloadEngine::clearTransfers() {
    while (!transfers_.isEmpty()) {
        Transfer *t = transfers_.takeLast();
        curl_multi_remove_handle(multi_, t->easy);
        curl_easy_cleanup(t->easy);
        curl_slist_free_all(t->headers);
        delete t;
    }
}

/**
 * @brief 建立文件数据先于数据库记录的可靠检查点。
 * 顺序为 QFile.flush → FlushFileBuffers → 保存各段摘要与终点 → SQLite 提交。
 * 任一步失败都不能抬高 committed；磁盘上多写但未提交的字节可在恢复时重新下载。
 */
bool DownloadEngine::checkpoint() {
    Task *t = find(activeId_);
    if (!t || !file_.isOpen())
        return true;
    // QFile::flush 只刷新用户态缓冲；FlushFileBuffers 才请求系统可靠落盘。
    if (!file_.flush()) {
        t->error = file_.errorString();
        return false;
    }
    intptr_t raw = _get_osfhandle(file_.handle());
    if (raw == -1 || !FlushFileBuffers(reinterpret_cast<HANDLE>(raw))) {
        t->error = QString::fromUtf8("系统刷新文件失败，错误码 %1").arg(GetLastError());
        return false;
    }
    Task durable = *t;
    durable.committed = 0;
    for (int i = 0; i < durable.segments.size(); ++i) {
        durable.segments[i].digest = hashes_[i]->result();
        durable.committed += durable.segments[i].written - durable.segments[i].begin;
    }
    if (!persist(durable)) {
        t->error = QString::fromUtf8("检查点提交失败");
        return false;
    }
    *t = durable;
    lastCheckpoint_ = clock_.elapsed();
    return true;
}

/**
 * @brief 停止活跃任务并收敛到暂停、失败或等待重试状态。
 * 先关闭网络，保存可相信的检查点，再关闭文件和哈希对象；检查点失败时从数据库恢复旧终点。
 * 重试只调整计划时间，不在错误回调中阻塞等待，也不无限重试证书/磁盘错误。
 */
void DownloadEngine::stopActive(const QString &state, const QString &reason, bool retry,
                                int serverDelaySeconds) {
    Task *t = find(activeId_);
    if (!t)
        return;
    clearTransfers();
    QString target = state;
    QString message = reason;
    if (!checkpoint()) {
        target = "Failed";
        message = t->error;
        retry = false;
    }
    file_.close();
    qDeleteAll(hashes_);
    hashes_.clear();
    if (retry && t->retries < 5) {
        ++t->retries;
        target = "RetryWait";
        int delay = qMax(serverDelaySeconds * 1000, qMin(60000, 1000 * (1 << t->retries))) +
                    int(qHash(t->id) % 251);
        retryTimes_[t->id] = clock_.elapsed() + delay;
    }
    t->state = target;
    t->error = message;
    t->speed = 0;
    // 检查点失败后不能将未确认的 written 写入数据库，因此重新载入旧快照。
    QVector<Task> durable;
    QString dbError;
    if (repository_.load(&durable, &dbError))
        for (int i = 0; i < durable.size(); ++i)
            if (durable[i].id == t->id) {
                t->segments = durable[i].segments;
                t->committed = durable[i].committed;
                break;
            }
    t->received = t->committed;
    persist(*t);
    publish(*t);
    activeId_.clear();
}

/**
 * @brief 验证长度后发布最终文件。
 * 先保存 Finalizing 意图，再关闭文件并同卷重命名，最后保存 Completed。
 * 如果在重命名和最后一次数据库提交之间退出，下次启动会按区间摘要核实最终文件。
 */
void DownloadEngine::finishTask() {
    Task *t = find(activeId_);
    if (!t)
        return;
    if (!checkpoint()) {
        stopActive("Failed", t->error);
        return;
    }
    if (t->total >= 0 && t->received != t->total) {
        stopActive("Failed", QString::fromUtf8("下载长度不完整"));
        return;
    }
    if (t->total < 0)
        t->total = t->received;
    t->state = "Finalizing";
    if (!persist(*t)) {
        stopActive("Failed", QString::fromUtf8("无法保存完成意图"));
        return;
    }
    file_.close();
    if (!QFile::rename(partPath(*t), t->path)) {
        stopActive("Failed", QString::fromUtf8("无法发布文件；临时文件已保留"));
        return;
    }
    qDeleteAll(hashes_);
    hashes_.clear();
    t->state = "Completed";
    t->error.clear();
    t->speed = 0;
    persist(*t);
    publish(*t);
    activeId_.clear();
}

/**
 * @brief 接收 curl 对 socket 读写关注方向的变更。
 * 此处只调整 Qt 监视器，不递归调用 socket_action；停止期间只接受移除通知。
 */
int DownloadEngine::socketCallback(CURL *, curl_socket_t socket, int action, void *context,
                                   void *) {
    DownloadEngine *self = static_cast<DownloadEngine *>(context);
    if (!self->stopping_ || action == CURL_POLL_REMOVE)
        self->watchSocket(socket, action);
    return 0;
}

/**
 * @brief 把 curl 的下次超时交给单次 Qt 定时器。
 * 负值取消，0 也异步投递；在回调内立即重新调用 curl 会产生递归或破坏句柄生命周期。
 */
int DownloadEngine::timerCallback(CURLM *, long timeoutMs, void *context) {
    DownloadEngine *self = static_cast<DownloadEngine *>(context);
    if (!self->curlTimer_ || self->stopping_)
        return 0;
    if (timeoutMs < 0)
        self->curlTimer_->stop();
    else
        // 0 也必须交给事件循环。这里直接调用 libcurl 会造成回调递归。
        self->curlTimer_->start(int(qMin<long>(timeoutMs, INT_MAX)));
    return 0;
}

/**
 * @brief 维护一个 socket 对应的读/写 QSocketNotifier。
 * Windows SOCKET 为指针宽度，不能从 Qt 5.8 信号的 int 参数重新构造；lambda 捕获原始句柄。
 * 移除时先禁用、断连再 deleteLater，并用 serial 防止系统复用同一数值句柄后误收旧事件。
 */
void DownloadEngine::watchSocket(curl_socket_t socket, int action) {
    const quintptr key = quintptr(socket);
    CurlSocketWatch *watch = socketWatches_.value(key, 0);
    if (action == CURL_POLL_REMOVE) {
        if (!watch)
            return;
        socketWatches_.remove(key);
        // callback 可能在 activated 槽执行中移除当前 notifier。先禁用和断连，
        // 再延迟销毁 QObject，不能在它的信号调用栈中直接 delete。
        if (watch->read) {
            watch->read->setEnabled(false);
            watch->read->disconnect(this);
            watch->read->deleteLater();
        }
        if (watch->write) {
            watch->write->setEnabled(false);
            watch->write->disconnect(this);
            watch->write->deleteLater();
        }
        delete watch;
        return;
    }
    if (!watch) {
        watch = new CurlSocketWatch;
        watch->serial = ++nextSocketSerial_;
        socketWatches_.insert(key, watch);
    }
    const quint64 serial = watch->serial;
    if (!watch->read) {
        watch->read = new QSocketNotifier(qintptr(socket), QSocketNotifier::Read, this);
        connect(watch->read, &QSocketNotifier::activated, this,
                [this, socket, serial]() { socketReady(socket, CURL_CSELECT_IN, serial); });
    }
    if (!watch->write) {
        watch->write = new QSocketNotifier(qintptr(socket), QSocketNotifier::Write, this);
        connect(watch->write, &QSocketNotifier::activated, this,
                [this, socket, serial]() { socketReady(socket, CURL_CSELECT_OUT, serial); });
    }
    watch->read->setEnabled(action == CURL_POLL_IN || action == CURL_POLL_INOUT);
    watch->write->setEnabled(action == CURL_POLL_OUT || action == CURL_POLL_INOUT);
}

/**
 * @brief 处理仍然有效的 socket 就绪事件。
 * 先核对监视器序号，再驱动 curl；此检查避免已关闭连接的迟到事件访问新连接。
 */
void DownloadEngine::socketReady(curl_socket_t socket, int events, quint64 serial) {
    CurlSocketWatch *watch = socketWatches_.value(quintptr(socket), 0);
    if (!watch || watch->serial != serial)
        return; // 被移除的 socket 数值可能已用于新连接。
    pump(socket, events, true);
}

/**
 * @brief 停止所有 Qt socket 订阅。
 * 复制键列表后逐个删除，避免遍历容器时移除元素使迭代器失效。
 */
void DownloadEngine::clearSocketWatches() {
    const QList<quintptr> keys = socketWatches_.keys();
    for (int i = 0; i < keys.size(); ++i)
        watchSocket(curl_socket_t(keys[i]), CURL_POLL_REMOVE);
}

/**
 * @brief 处理 curl 指定的超时或启动事件。
 * 使用 CURL_SOCKET_TIMEOUT 通知整个 multi，而不是猜测某个传输的读写方向。
 */
void DownloadEngine::curlTimeout() {
    pump(CURL_SOCKET_TIMEOUT, 0, true);
}

/**
 * @brief 执行与接收频率无关的维护心跳。
 * 只维护 UI、重试和检查点；不能重新用固定 20 ms 轮询网络，否则会再次形成每路吞吐限制。
 */
void DownloadEngine::tick() {
    pump(CURL_SOCKET_TIMEOUT, 0, false);
}

/**
 * @brief 网络就绪与维护事件的共同串行入口。
 * networkEvent=true 才调用 socket_action，返回后统一消费完成消息并释放句柄；回调里只记录错误意图。
 * inTick_ 防止重入；UI 采样和检查点使用单调时钟，系统时间变化不影响速度和重试。
 */
void DownloadEngine::pump(curl_socket_t socket, int events, bool networkEvent) {
    if (inTick_ || stopping_)
        return;
    inTick_ = true;
    if (!activeId_.isEmpty()) {
        int running = 0;
        CURLMcode multiCode =
            networkEvent ? curl_multi_socket_action(multi_, socket, events, &running) : CURLM_OK;
        if (multiCode != CURLM_OK) {
            stopActive("Failed", QString::fromLatin1(curl_multi_strerror(multiCode)));
            inTick_ = false;
            return;
        }
        int left = 0;
        CURLMsg *msg;
        while ((msg = curl_multi_info_read(multi_, &left)) != 0) {
            if (msg->msg != CURLMSG_DONE)
                continue;
            Transfer *x = 0;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &x);
            CURLcode code = msg->data.result;
            if (x->probe) {
                finishProbe(x, code);
                inTick_ = false;
                return;
            }
            Task *t = find(activeId_);
            if (!t)
                break;
            if (x->restartAsStream) {
                // 在响应正文进入输出前已经拒绝写入；先停止所有分段并保存，
                // 下一次探测强制丢弃旧段，整文件从零下载，禁止把 200 写到偏移处。
                QString id = t->id;
                forceSingle_.insert(id);
                stopActive("Queued", QString::fromUtf8("服务器忽略 Range，回退单连接"));
                inTick_ = false;
                return;
            }
            // 零字节响应不触发 write callback，也必须核实状态和预期区间。
            if (code != CURLE_OK || !x->error.isEmpty() ||
                (!x->headersChecked && !(x->status == 200 && !t->ranges && t->total == 0)) ||
                (t->segments[x->segment].end >= 0 &&
                 t->segments[x->segment].written != t->segments[x->segment].end)) {
                QString why = !x->error.isEmpty()
                                  ? x->error
                                  : QString("HTTP %1 / %2")
                                        .arg(x->status)
                                        .arg(QString::fromLatin1(curl_easy_strerror(code)));
                bool retry = x->error.isEmpty() &&
                             (code == CURLE_OPERATION_TIMEDOUT || code == CURLE_RECV_ERROR ||
                              code == CURLE_COULDNT_CONNECT || code == CURLE_COULDNT_RESOLVE_HOST ||
                              code == CURLE_SEND_ERROR || code == CURLE_PARTIAL_FILE ||
                              x->status == 429 || x->status >= 500);
                stopActive("Failed", why, retry, x->retryAfterSeconds);
                inTick_ = false;
                return;
            }
            curl_multi_remove_handle(multi_, x->easy);
            curl_easy_cleanup(x->easy);
            curl_slist_free_all(x->headers);
            transfers_.removeOne(x);
            delete x;
        }
        if (!activeId_.isEmpty()) {
            Task *t = find(activeId_);
            if (transfers_.isEmpty())
                finishTask();
            else if (t->state == "Downloading") {
                qint64 now = clock_.elapsed();
                if (now - lastCheckpoint_ >= 2000 && !checkpoint()) {
                    stopActive("Failed", t->error);
                    inTick_ = false;
                    return;
                }
                if (now - lastUi_ >= 200) {
                    t->speed = 1000.0 * (t->received - lastSpeedBytes_) /
                               qMax<qint64>(1, now - lastSpeedTime_);
                    lastSpeedTime_ = now;
                    lastSpeedBytes_ = t->received;
                    lastUi_ = now;
                    publish(*t);
                }
            }
        }
    }
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].state == "RetryWait" && clock_.elapsed() >= retryTimes_.value(tasks_[i].id)) {
            tasks_[i].state = "Queued";
            publish(tasks_[i]);
        }
    startNext();
    inTick_ = false;
}

/**
 * @brief 暂停指定任务。
 * 活跃任务先执行网络停止与检查点屏障；排队任务只修改状态。显示已暂停前必须完成可靠进度保存。
 */
void DownloadEngine::pauseTask(const QString &id) {
    Task *t = find(id);
    if (!t)
        return;
    if (id == activeId_)
        stopActive("Paused", QString());
    else if (t->state == "Queued" || t->state == "RetryWait") {
        t->state = "Paused";
        persist(*t);
        publish(*t);
    }
}
/**
 * @brief 重新排队一个暂停或失败任务。
 * 已完成任务只返回快照；恢复仍须重新探测资源和检查本地摘要，不能直接沿用旧连接。
 */
void DownloadEngine::resumeTask(const QString &id) {
    Task *t = find(id);
    if (!t) {
        emit problem(QString::fromUtf8("任务 ID 不存在"));
        return;
    }
    if (id == activeId_ || t->state == "Completed") {
        publish(*t);
        return;
    }
    t->state = "Queued";
    t->error.clear();
    t->retries = 0;
    if (persist(*t)) {
        publish(*t);
        startNext();
    }
}
/**
 * @brief 移除任务记录与本任务拥有的临时文件。
 * 活跃任务先停止并关闭句柄；不会删除已经发布的用户最终文件。删除失败时保留记录便于诊断。
 */
void DownloadEngine::removeTask(const QString &id) {
    if (id == activeId_)
        stopActive("Paused", QString());
    Task *t = find(id);
    if (!t)
        return;
    QString error;
    // “移除记录”只删除明确由此任务拥有的临时文件，绝不删除已完成的输出。
    if (QFile::exists(partPath(*t)) && !QFile::remove(partPath(*t))) {
        emit problem(QString::fromUtf8("无法删除临时文件，记录已保留"));
        return;
    }
    if (!repository_.remove(id, &error)) {
        emit problem(error);
        return;
    }
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == id) {
            tasks_.remove(i);
            break;
        }
    emit taskRemoved(id);
}
/**
 * @brief 按依赖顺序关闭下载线程资源。
 * 先停止定时器和调度，再保存活跃任务、释放传输和 Qt 监视器，最后关闭 multi 与 curl 全局环境。
 * GUI 退出通过阻塞队列等待此屏障，不能先终止线程再尝试保存文件。
 */
void DownloadEngine::shutdown() {
    if (!initialized_)
        return;
    stopping_ = true;
    if (timer_)
        timer_->stop();
    if (curlTimer_)
        curlTimer_->stop();
    if (!activeId_.isEmpty())
        stopActive("Paused", QString());
    clearTransfers();
    clearSocketWatches();
    if (multi_) {
        // cleanup 可能关闭连接缓存。停止向即将销毁的 Qt 对象发注册回调。
        curl_multi_setopt(multi_, CURLMOPT_SOCKETFUNCTION, 0);
        curl_multi_setopt(multi_, CURLMOPT_TIMERFUNCTION, 0);
        curl_multi_cleanup(multi_);
        multi_ = 0;
    }
    curl_global_cleanup();
    initialized_ = false;
}
