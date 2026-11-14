#ifndef QDM_REPOSITORY_H
#define QDM_REPOSITORY_H
/** @file repository.h @brief SQLite C API 适配；仅下载工作线程调用。
 * 每次保存为一个原子 SQL statement，连接不跨线程。不加载 Qt SQL 插件。
 */
#include "task.h"
struct sqlite3;
class Repository {
  public:
    Repository();
    ~Repository();
    /// 打开本地数据库，启用 WAL/FULL 并检查 schema；失败给出用户可读原因。
    bool open(const QString &path, QString *error);
    /// 加载值快照并解密 URL。DPAPI 解密失败时拒绝静默遗失任务。
    bool load(QVector<Task> *tasks, QString *error);
    /// 保存任务的原子快照；调用者必须先确保其中 committed 数据已可靠落盘。
    bool save(const Task &task, QString *error);
    /// 仅删除任务记录，不触碰任何下载文件。
    bool remove(const QString &id, QString *error);

  private:
    sqlite3 *db_; ///< 仓库独占连接；创建、SQL、备份和销毁均在同一下载线程。
    Q_DISABLE_COPY(Repository)
};
#endif
