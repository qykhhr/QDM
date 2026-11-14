#ifndef QDM_BROWSERBRIDGE_H
#define QDM_BROWSERBRIDGE_H
/** @file browserbridge.h
 * @brief GUI 线程中的本机 HTTP 桥。只监听 IPv4 loopback；授权后异步投递任务。
 * 网络输入不能指定磁盘目录/命令。引擎的持久化结果才是创建成功的依据。
 */
#include <QtNetwork>
#include "downloadengine.h"
class BrowserBridge : public QObject {
    Q_OBJECT
  public:
    /// engine 由应用拥有；此对象归 GUI 线程，port=0 只用于隔离测试。
    BrowserBridge(DownloadEngine *engine, const QString &dataDir, const QString &downloadDir,
                  quint16 port, QObject *parent = 0);
    ~BrowserBridge();
    QString pairingText() const; ///< 含配对密钥，仅供用户复制到自己的扩展，不写日志。
    QString downloadDirectory() const;
    int connections() const;
    /// 保存受控 default profile；不能由浏览器请求更改，失败时返回错误原因。
    bool setPreferences(const QString &directory, int connections, QString *error);
    bool isListening() const;
  public slots:
    /// 引擎 ready 后启动。失败只禁用浏览器连接，不破坏已有手工下载。
    void start();
  private slots:
    void acceptConnections();
    void updateTask(const Task &task);
    void taskResult(const QString &requestId, const QString &taskId, const QString &error);
  signals:
    void submit(const QString &requestId, const QString &fingerprint, const QString &url,
                const QString &path, int connections);
    void problem(const QString &message);
    /// 任务已持久化且桌面确认成功；GUI 可据此唤起窗口并选中新任务。
    void taskAccepted(const QString &taskId);

  private:
    struct Pending {
        QString fingerprint, path;
        QList<QPointer<QTcpSocket>> clients;
    };
    void consume(QTcpSocket *socket);
    void dispatch(QTcpSocket *socket, const QByteArray &method, const QByteArray &target,
                  const QMap<QByteArray, QByteArray> &headers, const QByteArray &body);
    void respond(QTcpSocket *socket, int status, const QJsonObject &body,
                 const QByteArray &origin = QByteArray());
    void reject(QTcpSocket *socket, int status, const QString &code, const QString &message);
    QJsonObject taskJson(const Task &task) const;
    QString uniquePath(const QString &name) const;
    DownloadEngine *engine_;
    QString dataDir_, downloadDir_;
    quint16 requestedPort_;
    int connections_;
    QByteArray token_;
    QTcpServer server_;
    QMap<QTcpSocket *, QByteArray> buffers_;
    QSet<QTcpSocket *> clients_;
    QHash<QString, Task> tasks_;
    QHash<QString, Pending> pending_;
    QSet<QString> reservations_;
};
#endif
