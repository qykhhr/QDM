#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file main.cpp
 * @brief Chrome/Edge Native Messaging 宿主。stdin/stdout 仅传小端长度前缀 JSON。
 * 浏览器按用户请求启动独立进程；阻塞 I/O 不发生在桌面 GUI 或下载线程。
 * 宿主只实现 health/create/status，不运行命令、不接收任意目录或任意 API 路径。
 */
#include "browserprotocol.h"
#include <QtNetwork>
#include <cstdio>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
namespace {
/** @brief 循环读取指定字节数，处理浏览器把一个长度帧拆成多次管道写入的情况。EOF 或读失败返回
 * false，不能把半帧当作 JSON。 */
bool readExact(char *data, size_t count) {
    size_t done = 0;
    while (done < count) {
        size_t n = fread(data + done, 1, count - done, stdin);
        if (!n)
            return false;
        done += n;
    }
    return true;
}
/** @brief 输出 UTF-8 JSON 与 4 字节小端长度。stdout 是浏览器协议通道，不能混入日志；写完立即
 * flush，避免浏览器一直等待响应。 */
bool send(const QJsonObject &o) {
    QByteArray data = QJsonDocument(o).toJson(QJsonDocument::Compact);
    quint32 size = quint32(data.size());
    unsigned char header[4] = {unsigned char(size), unsigned char(size >> 8),
                               unsigned char(size >> 16), unsigned char(size >> 24)};
    return fwrite(header, 1, 4, stdout) == 4 &&
           fwrite(data.constData(), 1, data.size(), stdout) == size && fflush(stdout) == 0;
}
/** @brief 构造有限操作集合使用的错误对象。错误文本不得包含
 * Token、完整签名地址或任何被转发的原始请求。 */
QJsonObject error(const QString &code, const QString &message) {
    QJsonObject o;
    o["ok"] = false;
    o["code"] = code;
    o["message"] = message;
    return o;
}
/** 冷启动只允许执行宿主同目录的 QDM.exe，不接收浏览器指定的命令或参数。
 * 独立宿主可以短时等待 GUI 建立服务，绝不在 GUI/下载线程里阻塞等待。
 */
bool launchDesktop(const QString &dir) {
    const QString executable = QCoreApplication::applicationDirPath() + "/QDM.exe";
    if (!QFile::exists(executable) ||
        !QProcess::startDetached(executable, QStringList() << "--data-dir" << dir))
        return false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 6000) {
        quint16 port = 0;
        QByteArray token;
        QString error;
        if (BrowserProtocol::connection(dir, &port, &token, &error)) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, port);
            if (probe.waitForConnected(100)) {
                probe.abort();
                return true;
            }
        }
        QThread::msleep(100);
    }
    return false;
}
/** @brief 将允许的 health/create/status 消息转换成本机 HTTP
 * 请求。先验证操作，再决定是否冷启动固定的
 * QDM.exe；最多重试启动一次，避免无限递归。单次正文、响应和等待时间均有上限。 */
QJsonObject forward(const QJsonObject &request, const QString &dir, bool allowLaunch = true) {
    quint16 port;
    QByteArray token;
    QString why;
    QByteArray method = "GET", target = "/api/v1/health", body;
    const QString op = request["op"].toString();
    if (op == "create") {
        if (!request["request"].isObject())
            return error("BAD_MESSAGE", "Missing request");
        method = "POST";
        target = "/api/v1/tasks";
        body = QJsonDocument(request["request"].toObject()).toJson(QJsonDocument::Compact);
    } else if (op == "status") {
        QString id = request["task_id"].toString();
        if (!QRegExp("^[a-fA-F0-9-]{36}$").exactMatch(id))
            return error("BAD_MESSAGE", "Invalid task ID");
        target = "/api/v1/tasks/" + id.toLatin1();
    } else if (op != "health")
        return error("BAD_MESSAGE", "Unsupported operation");
    if (!BrowserProtocol::connection(dir, &port, &token, &why)) {
        if (op == "create" && allowLaunch && launchDesktop(dir))
            return forward(request, dir, false);
        return error("DESKTOP_NOT_RUNNING", why);
    }
    QTcpSocket socket;
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(3000)) {
        if (op == "create" && allowLaunch && launchDesktop(dir))
            return forward(request, dir, false);
        return error("DESKTOP_NOT_RUNNING",
                     QString::fromUtf8("请启动支持浏览器扩展的 QDM 桌面程序"));
    }
    if (op == "create") {
        // Native Host 由用户当前浏览器启动；尽力把前台激活许可交给指定 QDM。
        // 不修改 Windows 前台锁定设置，也不向任意进程授予 ASFW_ANY 权限。
        QFile endpoint(QDir(dir).filePath("browser-endpoint.json"));
        if (endpoint.open(QIODevice::ReadOnly)) {
            const quint32 pid =
                QJsonDocument::fromJson(endpoint.read(16384)).object()["pid"].toString().toUInt();
            if (pid)
                AllowSetForegroundWindow(DWORD(pid));
        }
    }
    QByteArray wire =
        method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) +
        "\r\nAuthorization: Bearer " + token +
        "\r\nX-QDM-Extension-Id: " + BrowserProtocol::ExtensionId +
        "\r\nOrigin: chrome-extension://" + BrowserProtocol::ExtensionId +
        "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    socket.write(wire);
    QElapsedTimer deadline;
    deadline.start();
    QByteArray response;
    while (deadline.elapsed() < 10000) {
        if (socket.bytesAvailable() || socket.waitForReadyRead(250))
            response += socket.readAll();
        if (response.size() > 131072)
            return error("BAD_RESPONSE", "Response exceeds limit");
        int split = response.indexOf("\r\n\r\n");
        if (split >= 0) {
            QRegExp length("(?:^|\\r\\n)Content-Length: ([0-9]+)", Qt::CaseInsensitive);
            QString headers = QString::fromLatin1(response.left(split));
            if (length.indexIn(headers) >= 0 &&
                response.size() >= split + 4 + length.cap(1).toInt()) {
                QJsonDocument result =
                    QJsonDocument::fromJson(response.mid(split + 4, length.cap(1).toInt()));
                if (result.isObject())
                    return result.object();
                return error("BAD_RESPONSE", "Invalid desktop response");
            }
        }
        if (socket.state() == QAbstractSocket::UnconnectedState)
            break;
    }
    return error("DESKTOP_TIMEOUT", QString::fromUtf8("桌面程序响应超时，请稍后重试"));
}
} // namespace
/** @brief 设置 QDM 数据目录和二进制管道，验证 Chrome/Edge 传入的扩展来源，再逐帧处理消息。完整 EOF
 * 正常结束，截断帧或超限输入失败退出。 */
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("QDM");
    app.setOrganizationName("QDM");
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    QString origin = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    QRegExp match("^chrome-extension://([a-p]{32})/?$");
    if (!match.exactMatch(origin) || !BrowserProtocol::validExtension(match.cap(1).toLatin1())) {
        send(error("BAD_ORIGIN", "Only the QDM extension may invoke this host"));
        return 1;
    }
    QString dir = QFile::exists(app.applicationDirPath() + "/portable.flag")
                      ? app.applicationDirPath() + "/data"
                      : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QFile config(app.applicationDirPath() + "/native-host-settings.json");
    if (config.open(QIODevice::ReadOnly)) {
        QString custom =
            QJsonDocument::fromJson(config.read(16384)).object()["data_dir"].toString();
        if (!custom.isEmpty())
            dir = custom;
    }
    while (true) {
        unsigned char bytes[4];
        size_t initial = fread(bytes, 1, 4, stdin);
        if (initial == 0 && feof(stdin))
            return 0;
        if (initial < 4 && !readExact(reinterpret_cast<char *>(bytes) + initial, 4 - initial))
            return 1;
        quint32 length = quint32(bytes[0]) | (quint32(bytes[1]) << 8) | (quint32(bytes[2]) << 16) |
                         (quint32(bytes[3]) << 24);
        if (!length || length > 65536) {
            send(error("BAD_MESSAGE", "Message length must be 1..65536"));
            return 1;
        }
        QByteArray data(int(length), '\0');
        if (!readExact(data.data(), length))
            return 1;
        QJsonParseError parse;
        QJsonDocument message = QJsonDocument::fromJson(data, &parse);
        QJsonObject result = parse.error == QJsonParseError::NoError && message.isObject()
                                 ? forward(message.object(), dir)
                                 : error("BAD_MESSAGE", "Expected JSON object");
        if (!send(result))
            return 1;
    }
}
