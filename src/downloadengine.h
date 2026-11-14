#ifndef QDM_DOWNLOADENGINE_H
#define QDM_DOWNLOADENGINE_H
/** @file downloadengine.h
 * @brief 第一阶段单活跃任务调度器，最多 64 个分段并发。
 * 所有对象/回调都在一个工作线程，GUI 仅收快照，不持有 CURL 或文件对象。
 * MVP 将有界同步写入与数据库串行放在工作线程，后续拆分独立写线程。
 */
#include "repository.h"
#include <curl/curl.h>
struct Transfer;
struct CurlSocketWatch;
class DownloadEngine : public QObject {
    Q_OBJECT
  public:
    /// 在 GUI 线程构造后 moveToThread；此时尚不创建定时器或打开数据库。
    explicit DownloadEngine(const QString &dataDir, const QString &caFile = QString(),
                            QObject *parent = 0);
    ~DownloadEngine();
    static size_t writeCallback(char *data, size_t size, size_t count, void *context);
    static size_t headerCallback(char *data, size_t size, size_t count, void *context);
    /// libcurl 只在回调中更新监视器/定时器，不递归进入传输处理。
    static int socketCallback(CURL *easy, curl_socket_t socket, int action, void *context,
                              void *socketData);
    static int timerCallback(CURLM *multi, long timeoutMs, void *context);
  public slots:
    /// 工作线程入口；初始化 SQLite/libcurl、加载历史，成功后发 ready。
    void initialize();
    /** 添加并持久化任务。url 只接受 HTTP(S)，path 必须为不存在的绝对文件路径。
     * connections 限制为 1..64；实际段数受最小分段大小约束。异步结果通过快照通知。
     */
    void addTask(const QString &url, const QString &path, int connections);
    /// 浏览器异步提交；数据库保存后返回 task ID，重试相同幂等键不重复建任务。
    void addBrowserTask(const QString &requestId, const QString &fingerprint, const QString &url,
                        const QString &path, int connections);
    /// 幂等暂停；返回前已停止请求并完成文件刷新和数据库检查点。
    void pauseTask(const QString &id);
    /// 重新排队并验证源身份和本地摘要；无验证器时从零开始，Completed 保持完成。
    void resumeTask(const QString &id);
    /// 停止任务后清理专属临时文件及记录；保留用户最终文件，失败时发 problem。
    void removeTask(const QString &id);
    /// 退出屏障：停止调度、保存活跃任务、清理所有 easy/multi，再全局清理 curl。
    void shutdown();
  private slots:
    void tick(); ///< 仅做 UI/检查点/重试维护，不以固定周期驱动网络接收。
    void curlTimeout(); ///< libcurl 指定的单次超时，包含零延迟的异步启动请求。
  signals:
    /// 可复制快照，不含 CURL/文件句柄。实时 received 可以大于可靠 committed。
    void taskUpdated(const Task &task);
    void taskRemoved(const QString &id);
    void problem(const QString &message);
    void ready();
    void browserTaskResult(const QString &requestId, const QString &taskId, const QString &error);

  private:
    QString enqueueTask(const QString &url, const QString &path, int connections, QString *error,
                        const QString &requestId = QString(),
                        const QString &fingerprint = QString());
    void pump(curl_socket_t socket, int events, bool networkEvent);
    void watchSocket(curl_socket_t socket, int action);
    void socketReady(curl_socket_t socket, int events, quint64 serial);
    void clearSocketWatches();
    void publish(Task &task);
    bool persist(Task &task);
    void startNext();
    void startProbe(Task &task);
    void finishProbe(Transfer *transfer, CURLcode code);
    /// 校验持久化段覆盖及 SHA-256，摘要不一致时整体重下；不信任文件长度。
    bool prepareFile(Task &task);
    Transfer *createTransfer(int segment, bool probe);
    void clearTransfers(); ///< 只能在 curl 回调外执行，调用后无残留网络回调。
    bool checkpoint();     ///< 文件 flush + FlushFileBuffers 成功后才提交 SQLite。
    void stopActive(const QString &state, const QString &reason, bool retry = false,
                    int serverDelaySeconds = 0);
    void finishTask(); ///< 完整长度验证、检查点、Finalizing 意图、同卷重命名、完成。
    QString partPath(const Task &task) const;
    Task *find(const QString &id);
    QString dataDir_;
    QString caFile_; ///< 可选私有 CA 文件，仍执行证书及主机名验证；不修改系统信任。
    Repository repository_;
    QVector<Task> tasks_;
    QString activeId_;
    CURLM *multi_;
    QList<Transfer *> transfers_;
    QFile file_; ///< 当前活跃任务的唯一写入句柄，只能由下载线程 seek/write。
    QVector<QCryptographicHash *> hashes_; ///< 与 segments 同索引；清理任务时逐个释放。
    QTimer *timer_;     ///< 维护心跳：重试、UI 与检查点，不驱动接收频率。
    QTimer *curlTimer_; ///< curl 指定的单次超时，包含异步 0 ms 启动事件。
    QHash<quintptr, CurlSocketWatch *> socketWatches_; ///< 本线程拥有的 socket 读写订阅。
    quint64 nextSocketSerial_; ///< socket 可被系统复用，以序号过滤旧 notifier 激活。
    QElapsedTimer clock_;
    qint64 lastUi_, lastCheckpoint_, lastSpeedTime_,
        lastSpeedBytes_;                ///< 前三项为单调毫秒，最后一项为 byte。
    QHash<QString, qint64> retryTimes_; ///< 每任务单调时钟重试期限，避免相互覆盖。
    QSet<QString> forceSingle_; ///< 探测后服务器忽略 Range 时，安全回退整文件。
    bool initialized_; ///< 数据库与 curl 已就绪，不等于已有网络任务运行。
    bool stopping_;    ///< 退出闩锁；阻止回调注册新资源或调度下一个任务。
    bool inTick_;      ///< 防止事件泵重入，不是跨线程互斥锁。
    Q_DISABLE_COPY(DownloadEngine)
};
#endif
