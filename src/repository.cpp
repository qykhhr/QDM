#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file repository.cpp @brief 单线程数据库、绑定参数、版本门禁及错误传播。 */
#include "repository.h"
#include <sqlite3.h>
#include <windows.h>
#include <wincrypt.h>
namespace {
void bindText(sqlite3_stmt *s, int i, const QString &v) {
    QByteArray b = v.toUtf8();
    sqlite3_bind_text(s, i, b.constData(), b.size(), SQLITE_TRANSIENT);
}
QString text(sqlite3_stmt *s, int i) {
    const unsigned char *v = sqlite3_column_text(s, i);
    return v ? QString::fromUtf8(reinterpret_cast<const char *>(v)) : QString();
}
// 签名下载地址也可能携带凭据。使用当前 Windows 用户的 DPAPI，
// 不把 URL 明文存入 SQLite；密文不承诺跨用户或跨机器可恢复。
bool protectUrl(const QString &url, QString *encoded) {
    QByteArray bytes = url.toUtf8();
    DATA_BLOB input = {DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())},
              output = {0, 0};
    if (!CryptProtectData(&input, L"QDM URL", 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;
    *encoded =
        "dpapi:" + QString::fromLatin1(
                       QByteArray(reinterpret_cast<const char *>(output.pbData), int(output.cbData))
                           .toBase64());
    LocalFree(output.pbData);
    return true;
}
bool unprotectUrl(const QString &encoded, QString *url) {
    if (!encoded.startsWith("dpapi:")) {
        *url = encoded;
        return true;
    } // 兼容早期开发数据库，下一次保存升级。
    QByteArray bytes = QByteArray::fromBase64(encoded.mid(6).toLatin1());
    DATA_BLOB input = {DWORD(bytes.size()), reinterpret_cast<BYTE *>(bytes.data())},
              output = {0, 0};
    if (!CryptUnprotectData(&input, 0, 0, 0, 0, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return false;
    *url = QString::fromUtf8(reinterpret_cast<const char *>(output.pbData), int(output.cbData));
    LocalFree(output.pbData);
    return true;
}
} // namespace
/**
 * @brief 创建未打开的数据库仓库；连接稍后在所属下载线程中打开。
 */
Repository::Repository() : db_(0) {}
/**
 * @brief 由持有线程关闭 SQLite 连接；不得与仍执行的 SQL 或备份并发析构。
 */
Repository::~Repository() {
    if (db_)
        sqlite3_close(db_);
}
/**
 * @brief 打开数据库并按 schema 版本升级。
 * 拒绝较新版本；v1 升级前用 SQLite backup API 保存一致快照，再在事务中添加浏览器幂等字段。
 * WAL/FULL 保证提交语义，但不能代替下载文件自身的系统级刷盘。
 */
bool Repository::open(const QString &path, QString *error) {
    if (sqlite3_open_v2(path.toUtf8().constData(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        0) != SQLITE_OK) {
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    sqlite3_stmt *v = 0;
    if (sqlite3_prepare_v2(db_, "PRAGMA user_version", -1, &v, 0) != SQLITE_OK) {
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    int version = sqlite3_step(v) == SQLITE_ROW ? sqlite3_column_int(v, 0) : -1;
    sqlite3_finalize(v);
    if (version < 0 || version > 2) {
        *error = QString::fromUtf8("数据库版本不兼容");
        return false;
    }
    // 升级现有 v1 前保留一致的 SQLite 备份（包括 WAL 中已提交内容），
    // 不直接复制正在使用的主文件。备份失败则不开始修改 schema。
    if (version == 1) {
        const QString backupPath = path + ".pre-v2.bak";
        if (!QFile::exists(backupPath)) {
            sqlite3 *backupDb = 0;
            int result = sqlite3_open(backupPath.toUtf8().constData(), &backupDb);
            if (result == SQLITE_OK) {
                sqlite3_backup *copy = sqlite3_backup_init(backupDb, "main", db_, "main");
                if (copy) {
                    result = sqlite3_backup_step(copy, -1);
                    int finish = sqlite3_backup_finish(copy);
                    if (result == SQLITE_DONE)
                        result = finish;
                } else
                    result = SQLITE_ERROR;
            }
            if (backupDb)
                sqlite3_close(backupDb);
            if (result != SQLITE_OK) {
                QFile::remove(backupPath);
                *error = QString::fromUtf8("数据库升级备份失败，未修改原有任务");
                return false;
            }
        }
    }
    const char *sql =
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
        "BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS tasks("
        "id TEXT PRIMARY KEY,url TEXT NOT NULL,path TEXT NOT NULL UNIQUE,state TEXT NOT NULL,"
        "error TEXT,total INTEGER,committed INTEGER,connections INTEGER,ranges INTEGER,"
        "etag TEXT,modified TEXT,segments TEXT NOT NULL); COMMIT;";
    char *msg = 0;
    int rc = sqlite3_exec(db_, sql, 0, 0, &msg);
    if (rc != SQLITE_OK) {
        *error = QString::fromUtf8(msg ? msg : sqlite3_errmsg(db_));
        sqlite3_free(msg);
        return false;
    }
    // v2 为浏览器幂等请求增加持久化字段。迁移原子执行，旧版不能降级写入。
    if (version < 2) {
        const char *migration = "BEGIN IMMEDIATE; ALTER TABLE tasks ADD COLUMN request_id TEXT;"
                                "ALTER TABLE tasks ADD COLUMN request_fingerprint TEXT;"
                                "CREATE UNIQUE INDEX tasks_request_id ON tasks(request_id) WHERE "
                                "request_id IS NOT NULL;"
                                "PRAGMA user_version=2; COMMIT;";
        if (sqlite3_exec(db_, migration, 0, 0, &msg) != SQLITE_OK) {
            *error = QString::fromUtf8(msg ? msg : sqlite3_errmsg(db_));
            sqlite3_free(msg);
            sqlite3_exec(db_, "ROLLBACK", 0, 0, 0);
            return false;
        }
    }
    return true;
}
/**
 * @brief 读取任务和已提交的分段快照。
 * URL 由当前 Windows 用户的 DPAPI 解密；失败要明确报错，不把解密失败当作任务消失或空地址。
 */
bool Repository::load(QVector<Task> *tasks, QString *error) {
    sqlite3_stmt *s = 0;
    if (sqlite3_prepare_v2(
            db_,
            "SELECT "
            "id,url,path,state,error,total,committed,connections,ranges,etag,"
            "modified,segments,request_id,request_fingerprint FROM tasks ORDER BY rowid",
            -1, &s, 0) != SQLITE_OK) {
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    int rc;
    while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
        Task t;
        t.id = text(s, 0);
        if (!unprotectUrl(text(s, 1), &t.url)) {
            *error = QString::fromUtf8("无法解密任务地址，请使用创建任务的 Windows 用户");
            sqlite3_finalize(s);
            return false;
        }
        t.path = text(s, 2);
        t.state = text(s, 3);
        t.error = text(s, 4);
        t.total = sqlite3_column_int64(s, 5);
        t.committed = sqlite3_column_int64(s, 6);
        t.received = t.committed;
        t.connections = qBound(1, sqlite3_column_int(s, 7), 64);
        t.ranges = sqlite3_column_int(s, 8) != 0;
        t.etag = text(s, 9);
        t.modified = text(s, 10);
        t.segments = decodeSegments(text(s, 11).toUtf8());
        t.requestId = text(s, 12);
        t.requestFingerprint = text(s, 13);
        tasks->append(t);
    }
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    return true;
}
/**
 * @brief 用绑定参数原子保存一个任务快照。
 * URL 先经 DPAPI 加密；普通任务的 request_id 绑定 NULL，避免唯一索引把多个普通任务误当成重复。
 * 此方法不负责文件刷盘，调用者必须先遵守检查点提交顺序。
 */
bool Repository::save(const Task &t, QString *error) {
    QString encoded;
    if (!protectUrl(t.url, &encoded)) {
        *error = QString::fromUtf8("无法保护任务地址");
        return false;
    }
    sqlite3_stmt *s = 0;
    const char *sql =
        "INSERT OR REPLACE INTO "
        "tasks(id,url,path,state,error,total,committed,connections,ranges,etag,"
        "modified,segments,request_id,request_fingerprint) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &s, 0) != SQLITE_OK) {
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
        return false;
    }
    bindText(s, 1, t.id);
    bindText(s, 2, encoded);
    bindText(s, 3, t.path);
    bindText(s, 4, t.state);
    bindText(s, 5, t.error);
    sqlite3_bind_int64(s, 6, t.total);
    sqlite3_bind_int64(s, 7, t.committed);
    sqlite3_bind_int(s, 8, t.connections);
    sqlite3_bind_int(s, 9, t.ranges ? 1 : 0);
    bindText(s, 10, t.etag);
    bindText(s, 11, t.modified);
    bindText(s, 12, QString::fromUtf8(encodeSegments(t.segments)));
    if (t.requestId.isEmpty())
        sqlite3_bind_null(s, 13);
    else
        bindText(s, 13, t.requestId);
    bindText(s, 14, t.requestFingerprint);
    int rc = sqlite3_step(s);
    if (rc != SQLITE_DONE)
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
    sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}
/**
 * @brief 只删除一个任务的数据库记录。
 * 文件归属及停止屏障由引擎处理；SQL 仍绑定 UUID，不拼接外部输入。
 */
bool Repository::remove(const QString &id, QString *error) {
    sqlite3_stmt *s = 0;
    int rc = sqlite3_prepare_v2(db_, "DELETE FROM tasks WHERE id=?", -1, &s, 0);
    if (rc == SQLITE_OK) {
        bindText(s, 1, id);
        rc = sqlite3_step(s);
    }
    if (rc != SQLITE_DONE)
        *error = QString::fromUtf8(sqlite3_errmsg(db_));
    if (s)
        sqlite3_finalize(s);
    return rc == SQLITE_DONE;
}
