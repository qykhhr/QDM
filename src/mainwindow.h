#ifndef QDM_MAINWINDOW_H
#define QDM_MAINWINDOW_H
/** @file mainwindow.h @brief 主窗口与只读任务表；仅 GUI 线程使用。
 * 界面通过信号发命令，不跨线程访问引擎字段或执行文件下载。
 */
#include <QtWidgets>
#include "downloadengine.h"
class BrowserBridge;
class TaskTableModel : public QAbstractTableModel {
    Q_OBJECT
  public:
    explicit TaskTableModel(QObject *parent = 0);
    int rowCount(const QModelIndex &parent = QModelIndex()) const;
    int columnCount(const QModelIndex &parent = QModelIndex()) const;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const;
    Task taskAt(int row) const;
  public slots:
    void updateTask(const Task &task);
    void removeTask(const QString &id);

  private:
    QVector<Task> tasks_;
};
class MainWindow : public QMainWindow {
    Q_OBJECT
  public:
    explicit MainWindow(DownloadEngine *engine, BrowserBridge *bridge, QWidget *parent = 0);
  signals:
    void addRequested(const QString &url, const QString &path, int connections);
    void pauseRequested(const QString &id);
    void resumeRequested(const QString &id);
    void removeRequested(const QString &id);

  protected:
    void closeEvent(QCloseEvent *event);
  private slots:
    void newTask();
    void pauseSelected();
    void resumeSelected();
    void removeSelected();
    void showDetails();
    void openFolder();
    void browserConnection();

  private:
    Task selectedTask() const;
    TaskTableModel *model_;        ///< GUI 所有的任务快照，不暴露业务可变对象。
    QSortFilterProxyModel *proxy_; ///< 排序/筛选后的行号必须映射回 model_。
    QTableView *table_;
    QLabel *details_;
    QSystemTrayIcon *tray_;
    BrowserBridge *bridge_; ///< 借用应用级对象，生命周期长于主窗口。
};
#endif
