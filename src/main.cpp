#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file main.cpp
 * @brief 进程入口：目录选择、单实例、依赖装配、线程启动和退出屏障。
 * --headless 使用同一下载引擎执行自动化集成测试，不启动 Widgets。
 */
#include "mainwindow.h"
#include "browserbridge.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <cstdio>

namespace {
/** 只保存启动元信息，不保存 URL 或任务凭据。
 * 与 instance.lock 的 PID 一起验证，避免崩溃遗留文件冒充当前运行实例。
 */
void saveRuntimeInfo(const QString &path) {
    QJsonObject info;
    info["pid"] = QString::number(QCoreApplication::applicationPid());
    info["version"] = QCoreApplication::applicationVersion();
    info["executable"] = QCoreApplication::applicationFilePath();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    const QByteArray data = QJsonDocument(info).toJson(QJsonDocument::Compact);
    if (file.write(data) != data.size() || !file.commit())
        qWarning("Cannot save QDM instance metadata");
}

/** 相同程序正常唤回；另一份程序/旧版不再悄悄接管新版的启动动作。
 * 这里只提示并退出，不强制终止持有锁的进程，也不修改其任务或数据库。
 */
void reportExistingInstance(QLockFile &lock, const QString &runtimePath, bool headless) {
    qint64 pid = -1;
    QString host, name;
    lock.getLockInfo(&pid, &host, &name);
    QFile file(runtimePath);
    QJsonObject info;
    if (file.open(QIODevice::ReadOnly))
        info = QJsonDocument::fromJson(file.read(16384)).object();
    if (info.value("pid").toString() != QString::number(pid))
        info = QJsonObject();
    const QString version = info.value("version").toString();
    const QString executable = info.value("executable").toString();
    const bool sameProgram =
        version == QCoreApplication::applicationVersion() &&
        QDir::cleanPath(executable)
                .compare(QDir::cleanPath(QCoreApplication::applicationFilePath()),
                         Qt::CaseInsensitive) == 0;
    const QString message =
        QString::fromUtf8("已有 QDM 在运行（PID：%1，版本：%2）。\n\n"
                          "本次启动的版本为 %3，尚未切换到这份程序。\n"
                          "请在现有 QDM 中按 Ctrl+Q 或选择托盘菜单的退出，再重新打开本程序。\n\n"
                          "本次程序：%4\n当前程序：%5")
            .arg(pid)
            .arg(version.isEmpty() ? QString::fromUtf8("未知，可能为旧版") : version)
            .arg(QCoreApplication::applicationVersion())
            .arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()))
            .arg(executable.isEmpty() ? QString::fromUtf8("未提供版本信息")
                                      : QDir::toNativeSeparators(executable));
    if (headless) {
        const QByteArray bytes = message.toUtf8();
        fprintf(stderr, "%s\n", bytes.constData());
    } else if (!sameProgram) {
        QMessageBox::information(0, QString::fromUtf8("QDM 版本未切换"), message);
    }
}
} // namespace

/** @brief 应用装配入口：选择
 * GUI/headless、解析参数、持有数据目录实例锁，再启动工作线程及本机桥。退出时先等待引擎保存屏障，再释放线程与实例标记，不能直接结束仍写文件的线程。
 */
int main(int argc, char **argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--headless")
            headless = true;
    QScopedPointer<QCoreApplication> application(headless ? new QCoreApplication(argc, argv)
                                                          : new QApplication(argc, argv));
    if (!headless)
        QApplication::setFont(QFont("Microsoft YaHei UI", 9));
    QCoreApplication::setApplicationName("QDM");
    QCoreApplication::setOrganizationName("QDM");
    QCoreApplication::setApplicationVersion("0.2.1");
    qRegisterMetaType<Task>("Task");
    QCommandLineParser p;
    p.addHelpOption();
    p.addVersionOption();
    p.addOption(QCommandLineOption("headless", "Run without UI; emit JSON events to stdout."));
    p.addOption(QCommandLineOption("url", "HTTP/HTTPS URL.", "url"));
    p.addOption(QCommandLineOption("output", "Absolute output file.", "path"));
    p.addOption(
        QCommandLineOption("connections", "Maximum parallel segments (1-64).", "number", "4"));
    p.addOption(QCommandLineOption("data-dir", "Override data directory.", "path"));
    p.addOption(QCommandLineOption("ca-file",
                                   "Additional PEM CA file (verification stays enabled).", "path"));
    p.addOption(QCommandLineOption("resume", "Resume an existing task UUID.", "id"));
    p.addOption(QCommandLineOption("serve", "Enable browser bridge in headless mode."));
    p.addOption(QCommandLineOption(
        "bridge-port", "Loopback port (0 selects an ephemeral test port).", "port", "17878"));
    p.addOption(QCommandLineOption("download-dir", "Default browser download profile directory.",
                                   "directory"));
    p.addOption(QCommandLineOption("pause-after",
                                   "Pause after milliseconds and exit (diagnostics).", "ms"));
    p.process(*application);
    QString data = p.value("data-dir");
    if (data.isEmpty())
        data = QFile::exists(QCoreApplication::applicationDirPath() + "/portable.flag")
                   ? QCoreApplication::applicationDirPath() + "/data"
                   : QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    data = QDir(data).absolutePath();
    if (!QDir().mkpath(data)) {
        fprintf(stderr, "Cannot create data directory\n");
        return 2;
    }
    QLockFile lock(data + "/instance.lock");
    lock.setStaleLockTime(30000);
    const QString runtimePath = data + "/runtime.json";
    QString serverName =
        "QDM-" +
        QString::fromLatin1(
            QCryptographicHash::hash(data.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    if (!lock.tryLock(0)) {
        reportExistingInstance(lock, runtimePath, headless);
        QLocalSocket s;
        s.connectToServer(serverName);
        if (s.waitForConnected(1000)) {
            s.write("show");
            s.flush();
            s.waitForBytesWritten(1000);
        }
        fprintf(stderr, "QDM is already running for this data directory\n");
        return 3;
    }
    saveRuntimeInfo(runtimePath);
    QLocalServer::removeServer(serverName);
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    server.listen(serverName);
    QThread thread;
    DownloadEngine *engine = new DownloadEngine(data, p.value("ca-file"));
    engine->moveToThread(&thread);
    QObject::connect(&thread, SIGNAL(started()), engine, SLOT(initialize()));
    QObject::connect(&thread, SIGNAL(finished()), engine, SLOT(deleteLater()));
    int exitCode = 0;
    bool commandsStarted = false;
    BrowserBridge *bridge =
        (!headless || p.isSet("serve"))
            ? new BrowserBridge(engine, data, p.value("download-dir"),
                                quint16(p.value("bridge-port").toUInt()), application.data())
            : 0;
    QString target = p.value("resume");
    QScopedPointer<MainWindow> window;
    if (!headless) {
        window.reset(new MainWindow(engine, bridge));
        window->show();
        QObject::connect(&server, &QLocalServer::newConnection, application.data(), [&]() {
            QLocalSocket *s = server.nextPendingConnection();
            if (s) {
                s->close();
                s->deleteLater();
            }
            window->showNormal();
            window->raise();
            window->activateWindow();
        });
    } else {
        if (!p.isSet("serve") && target.isEmpty() &&
            (p.value("url").isEmpty() || p.value("output").isEmpty())) {
            fprintf(stderr, "Use --url and --output, or --resume ID\n");
            return 2;
        }
        QObject::connect(engine, &DownloadEngine::problem, application.data(),
                         [&](const QString &message) {
                             QByteArray b = message.toUtf8();
                             fprintf(stderr, "%s\n", b.constData());
                             exitCode = 2;
                             application->quit();
                         });
        QObject::connect(
            engine, &DownloadEngine::taskUpdated, application.data(), [&](const Task &t) {
                // initialize 发布的是历史快照，不能误当作本次 CLI 请求的结果。
                if (!commandsStarted)
                    return;
                if (target.isEmpty())
                    target = t.id;
                if (t.id != target)
                    return;
                QJsonObject o;
                o["id"] = t.id;
                o["state"] = t.state;
                o["received"] = QString::number(t.received);
                o["committed"] = QString::number(t.committed);
                o["error"] = t.error;
                o["segments"] = t.segments.size();
                QByteArray b = QJsonDocument(o).toJson(QJsonDocument::Compact);
                fprintf(stdout, "%s\n", b.constData());
                fflush(stdout);
                if (!p.isSet("serve") && (t.state == "Completed" || t.state == "Failed")) {
                    exitCode = t.state == "Completed" ? 0 : 1;
                    application->quit();
                }
            });
    }
    if (bridge)
        QObject::connect(
            bridge, &BrowserBridge::problem, application.data(), [&](const QString &message) {
                if (headless) {
                    QByteArray bytes = message.toUtf8();
                    fprintf(stderr, "%s\n", bytes.constData());
                    exitCode = 2;
                    application->quit();
                } else
                    QMessageBox::warning(window.data(), QString::fromUtf8("浏览器连接"), message);
            });
    QObject::connect(engine, &DownloadEngine::ready, application.data(), [&]() {
        commandsStarted = true;
        if (!p.value("resume").isEmpty())
            QMetaObject::invokeMethod(engine, "resumeTask", Qt::QueuedConnection,
                                      Q_ARG(QString, p.value("resume")));
        else if (!p.value("url").isEmpty())
            QMetaObject::invokeMethod(
                engine, "addTask", Qt::QueuedConnection, Q_ARG(QString, p.value("url")),
                Q_ARG(QString, QFileInfo(p.value("output")).absoluteFilePath()),
                Q_ARG(int, p.value("connections").toInt()));
        if (p.isSet("pause-after"))
            QTimer::singleShot(qMax(1, p.value("pause-after").toInt()), application.data(),
                               SLOT(quit()));
    });
    // BlockingQueuedConnection 仅用于退出屏障；工作线程仍运行事件循环，
    // shutdown 完成文件刷新/DB 提交之后才退出线程，避免释放活跃文件句柄。
    QObject::connect(application.data(), &QCoreApplication::aboutToQuit, application.data(), [&]() {
        if (thread.isRunning()) {
            QMetaObject::invokeMethod(engine, "shutdown", Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
        }
    });
    thread.start();
    application->exec();
    QFile::remove(runtimePath); // 仍持有实例锁，只删除此进程创建的元信息。
    return exitCode;
}
