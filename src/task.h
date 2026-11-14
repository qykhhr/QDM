#ifndef QDM_TASK_H
#define QDM_TASK_H
/** @file task.h
 * @brief 下载领域值类型。跨线程仅复制快照，不共享可变任务对象。
 * 所有偏移均为 64 位 byte；分段区间采用 [begin,end)。
 */
#include <QtCore>

struct Segment {
    qint64 begin;      ///< 包含的起始偏移，单位 byte。
    qint64 end;        ///< 不包含；非分段未知长度下载为 -1。
    qint64 written;    ///< 本次运行已写入的绝对终点，尚不等于可靠检查点。
    QByteArray digest; ///< 最近检查点前缀 SHA-256；仅持久化副本中与 written 同步。
    Segment() : begin(0), end(-1), written(0) {}
};

/** 任务快照；只有下载工作线程可修改其权威副本。URL 不输出到日志。 */
struct Task {
    QString id;                 ///< UUID，不使用 URL 或目标路径作为任务身份。
    QString requestId;          ///< 浏览器提交幂等键；普通桌面任务为空。
    QString requestFingerprint; ///< 幂等请求内容摘要，拒绝相同键对应不同资源。
    QString url;                ///< HTTP(S) 来源；存入数据库前由 DPAPI 保护。
    QString path;     ///< 用户选择的最终绝对路径；临时文件位于同一目录。
    QString state;    ///< 引擎内部英文状态，展示时使用 stateLabel 转换。
    QString error;    ///< 脱敏错误原因；成功/重新排队时清空。
    QString etag;     ///< 仅保留强 ETag；弱验证器不能用于安全续传。
    QString modified; ///< Last-Modified，强 ETag 缺失时的保守恢复依据。
    qint64 total;     ///< 总字节数，-1 表示未知，0 表示真正的空文件。
    qint64 received;  ///< 本次运行累计写入的字节数，不保证已经持久化。
    qint64 committed; ///< 文件已系统刷新且 SQLite 已提交的字节总数。
    double speed;     ///< 最近采样区间的 byte/s，仅用于展示，不持久化。
    int connections;  ///< 用户配置的并发上限，1..64；不表示当前连接数。
    int retries;      ///< 当前进程内连续重试次数，手工恢复时重置。
    bool ranges;      ///< 已通过实际 0-0 请求验证分段能力且未强制回退。
    QVector<Segment> segments; ///< 有序且不重叠的完整覆盖区间。
    Task()
        : total(-1), received(0), committed(0), speed(0), connections(4), retries(0),
          ranges(false) {}
};
Q_DECLARE_METATYPE(Task)
QString stateLabel(const QString &state);
QString byteLabel(qint64 value);
QByteArray encodeSegments(const QVector<Segment> &segments);
QVector<Segment> decodeSegments(const QByteArray &data);
#endif
