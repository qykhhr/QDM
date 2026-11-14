#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file task.cpp @brief 快照序列化和纯展示函数，无文件/网络操作。 */
#include "task.h"
/** @brief 把内部英文状态映射为中文显示。未知状态保留原值，便于发现新增状态未补翻译的问题。 */
QString stateLabel(const QString &s) {
    static const char *keys[] = {"Queued", "Probing",   "Downloading", "Paused",    "Completed",
                                 "Failed", "RetryWait", "Finalizing",  "Recovering"};
    static const char *names[] = {"排队中", "探测资源", "下载中",   "已暂停",  "已完成",
                                  "失败",   "等待重试", "完成发布", "恢复检查"};
    for (int i = 0; i < 9; ++i)
        if (s == QLatin1String(keys[i]))
            return QString::fromUtf8(names[i]);
    return s;
}
/** @brief 按 1024 进制格式化字节数；负值表示未知，0
 * 是真实空文件。此函数只用于展示，不参与进度计算。 */
QString byteLabel(qint64 n) {
    if (n < 0)
        return QString::fromUtf8("未知");
    double v = double(n);
    QStringList u;
    u << "B"
      << "KiB"
      << "MiB"
      << "GiB"
      << "TiB";
    int i = 0;
    while (v >= 1024 && i < 4) {
        v /= 1024;
        ++i;
    }
    return QString::number(v, 'f', i ? 1 : 0) + " " + u[i];
}
/** @brief 保存分段检查点。偏移用十进制字符串编码，避免 JSON double 超过 2^53 后丢失 64
 * 位精度；摘要以十六进制保存。 */
QByteArray encodeSegments(const QVector<Segment> &segments) {
    QJsonArray a;
    for (int i = 0; i < segments.size(); ++i) {
        const Segment &s = segments[i];
        QJsonObject o;
        // JSON double 无法精确表示所有 int64：偏移按十进制字符串存储。
        o["begin"] = QString::number(s.begin);
        o["end"] = QString::number(s.end);
        o["written"] = QString::number(s.written);
        o["sha256"] = QString::fromLatin1(s.digest.toHex());
        a.append(o);
    }
    return QJsonDocument(a).toJson(QJsonDocument::Compact);
}
/** @brief
 * 恢复分段值对象。遇到非法数字或越界终点返回空集合，让引擎重新下载；不能信任损坏记录继续偏移写入。
 */
QVector<Segment> decodeSegments(const QByteArray &data) {
    QVector<Segment> out;
    QJsonDocument d = QJsonDocument::fromJson(data);
    if (!d.isArray())
        return out;
    QJsonArray a = d.array();
    for (int i = 0; i < a.size(); ++i) {
        QJsonObject o = a[i].toObject();
        Segment s;
        bool b = false, e = false, w = false;
        s.begin = o["begin"].toString().toLongLong(&b);
        s.end = o["end"].toString().toLongLong(&e);
        s.written = o["written"].toString().toLongLong(&w);
        s.digest = QByteArray::fromHex(o["sha256"].toString().toLatin1());
        if (!b || !e || !w || s.begin < 0 || s.written < s.begin ||
            (s.end >= 0 && s.written > s.end))
            return QVector<Segment>();
        out.append(s);
    }
    return out;
}
