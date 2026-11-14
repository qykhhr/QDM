#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif
/** @file mainwindow.cpp @brief 任务列表、工具栏和托盘。业务状态由引擎快照驱动。 */
#include "mainwindow.h"
#include "browserbridge.h"
#include "browserprotocol.h"
#include <QDesktopServices>

/**
 * @brief 创建 GUI 线程任务表；模型只拥有快照副本，不拥有下载句柄。
 */
TaskTableModel::TaskTableModel(QObject *p) : QAbstractTableModel(p) {}
/**
 * @brief 返回根节点下的任务行数；表格为扁平模型，子节点没有行。
 */
int TaskTableModel::rowCount(const QModelIndex &p) const {
    return p.isValid() ? 0 : tasks_.size();
}
/**
 * @brief 返回固定展示列数；不能由任务进度动态增删列。
 */
int TaskTableModel::columnCount(const QModelIndex &p) const {
    return p.isValid() ? 0 : 7;
}
/**
 * @brief 按源模型行号复制任务；索引越界返回空任务，调用者须检查 id。
 */
Task TaskTableModel::taskAt(int row) const {
    return row >= 0 && row < tasks_.size() ? tasks_[row] : Task();
}
/**
 * @brief 返回中文列标题；只响应水平表头的展示角色。
 */
QVariant TaskTableModel::headerData(int i, Qt::Orientation o, int role) const {
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();
    QStringList h;
    h << QString::fromUtf8("文件名称") << QString::fromUtf8("状态") << QString::fromUtf8("大小")
      << QString::fromUtf8("已下载") << QString::fromUtf8("进度") << QString::fromUtf8("速度")
      << QString::fromUtf8("连接上限");
    return h.value(i);
}
/**
 * @brief 将领域值转换为界面内容、错误颜色和提示。
 * 速度为 byte/s 的短时采样，百分比仅在总大小已知时计算；不要把显示的 100% 直接写回任务状态。
 */
QVariant TaskTableModel::data(const QModelIndex &i, int role) const {
    if (!i.isValid() || i.row() >= tasks_.size())
        return QVariant();
    const Task &t = tasks_[i.row()];
    if (role == Qt::ToolTipRole)
        return t.path + (t.error.isEmpty() ? QString() : "\n" + t.error);
    if (role == Qt::ForegroundRole && t.state == "Failed")
        return QColor("#c43c32");
    if (role != Qt::DisplayRole)
        return QVariant();
    switch (i.column()) {
    case 0:
        return QFileInfo(t.path).fileName();
    case 1:
        return stateLabel(t.state);
    case 2:
        return byteLabel(t.total);
    case 3:
        return byteLabel(t.received);
    case 4:
        return t.total > 0 ? QString::number(100.0 * t.received / t.total, 'f', 1) + "%"
                           : (t.state == "Completed" ? "100%" : "--");
    case 5:
        return byteLabel(qint64(t.speed)) + "/s";
    case 6:
        return t.connections;
    }
    return QVariant();
}
/**
 * @brief 更新现有行或插入新行。
 * 通过 begin/endInsertRows 和 dataChanged 通知视图，避免重建整张表导致选择/排序丢失。
 */
void TaskTableModel::updateTask(const Task &t) {
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == t.id) {
            tasks_[i] = t;
            emit dataChanged(index(i, 0), index(i, 6));
            return;
        }
    int row = tasks_.size();
    beginInsertRows(QModelIndex(), row, row);
    tasks_.append(t);
    endInsertRows();
}
/**
 * @brief 按 UUID 移除一行快照，只更新视图，不直接删除文件。
 */
void TaskTableModel::removeTask(const QString &id) {
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == id) {
            beginRemoveRows(QModelIndex(), i, i);
            tasks_.remove(i);
            endRemoveRows();
            return;
        }
}

/**
 * @brief 装配主窗口、筛选模型、命令信号及托盘。
 * 所有业务命令通过 queued connection 进入引擎；浏览器确认任务后显示窗口并选中新行。
 * 窗口关闭默认隐藏，Ctrl+Q 或托盘退出才触发应用的保存退出屏障。
 */
MainWindow::MainWindow(DownloadEngine *engine, BrowserBridge *bridge, QWidget *p)
    : QMainWindow(p), tray_(0), bridge_(bridge) {
    setWindowTitle(QString::fromUtf8("QDM · 下载管理器 ") + QCoreApplication::applicationVersion());
    resize(1120, 680);
    QIcon icon = style()->standardIcon(QStyle::SP_ArrowDown);
    setWindowIcon(icon);
    QAction *exitAction = new QAction(QString::fromUtf8("退出并保存"), this);
    exitAction->setShortcut(QKeySequence("Ctrl+Q"));
    addAction(exitAction);
    connect(exitAction, SIGNAL(triggered()), qApp, SLOT(quit()));
    QToolBar *bar = addToolBar(QString::fromUtf8("下载操作"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    QAction *add = bar->addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder),
                                  QString::fromUtf8("新建下载"));
    QAction *resume =
        bar->addAction(style()->standardIcon(QStyle::SP_MediaPlay), QString::fromUtf8("恢复"));
    QAction *pause =
        bar->addAction(style()->standardIcon(QStyle::SP_MediaPause), QString::fromUtf8("暂停"));
    QAction *remove =
        bar->addAction(style()->standardIcon(QStyle::SP_TrashIcon), QString::fromUtf8("移除"));
    bar->addSeparator();
    QAction *folder = bar->addAction(QString::fromUtf8("打开目录"));
    QAction *browser = bar->addAction(QString::fromUtf8("浏览器扩展"));
    connect(browser, SIGNAL(triggered()), this, SLOT(browserConnection()));
    QLineEdit *search = new QLineEdit;
    search->setPlaceholderText(QString::fromUtf8("搜索文件名或状态"));
    search->setMaximumWidth(280);
    bar->addWidget(search);
    QWidget *central = new QWidget;
    QHBoxLayout *layout = new QHBoxLayout(central);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(16);
    QListWidget *nav = new QListWidget;
    nav->setMaximumWidth(150);
    nav->addItems(QStringList() << QString::fromUtf8("全部任务") << QString::fromUtf8("下载中")
                                << QString::fromUtf8("已暂停") << QString::fromUtf8("已完成")
                                << QString::fromUtf8("失败"));
    nav->setCurrentRow(0);
    layout->addWidget(nav);
    QWidget *right = new QWidget;
    QVBoxLayout *v = new QVBoxLayout(right);
    v->setContentsMargins(0, 0, 0, 0);
    QLabel *title = new QLabel(QString::fromUtf8("下载任务"));
    QFont f = title->font();
    f.setPointSize(18);
    f.setBold(true);
    title->setFont(f);
    v->addWidget(title);
    model_ = new TaskTableModel(this);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(-1);
    table_ = new QTableView;
    table_->setModel(proxy_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->hide();
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setMinimumSectionSize(90);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->setSortingEnabled(true);
    table_->setAlternatingRowColors(true);
    v->addWidget(table_);
    details_ =
        new QLabel(QString::fromUtf8("新建任务开始下载。移除任务会清理临时文件，保留已完成文件。"));
    details_->setWordWrap(true);
    details_->setTextFormat(Qt::PlainText);
    details_->setMinimumHeight(60);
    v->addWidget(details_);
    layout->addWidget(right, 1);
    setCentralWidget(central);
    // 浏览器交接确认后直接唤回软件，并定位到新任务，无需再打开扩展列表。
    connect(bridge_, &BrowserBridge::taskAccepted, this, [this, search, nav](const QString &id) {
        // 先显示全部任务，避免原来的分类/搜索条件把刚接管的任务隐藏。
        nav->setCurrentRow(0);
        search->clear();
        showNormal();
        raise();
        activateWindow();
        for (int row = 0; row < model_->rowCount(); ++row) {
            if (model_->taskAt(row).id == id) {
                const QModelIndex index = proxy_->mapFromSource(model_->index(row, 0));
                table_->setCurrentIndex(index);
                table_->scrollTo(index);
                break;
            }
        }
    });
    statusBar()->showMessage("QDM " + QCoreApplication::applicationVersion() +
                             QString::fromUtf8(" · 事件驱动下载 · Qt 5.8 · 最多64路分段"));
    connect(add, SIGNAL(triggered()), this, SLOT(newTask()));
    connect(resume, SIGNAL(triggered()), this, SLOT(resumeSelected()));
    connect(pause, SIGNAL(triggered()), this, SLOT(pauseSelected()));
    connect(remove, SIGNAL(triggered()), this, SLOT(removeSelected()));
    connect(folder, SIGNAL(triggered()), this, SLOT(openFolder()));
    connect(search, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(nav, &QListWidget::currentRowChanged, this, [this, search](int row) {
        QStringList s;
        s << "" << QString::fromUtf8("下载中") << QString::fromUtf8("已暂停")
          << QString::fromUtf8("已完成") << QString::fromUtf8("失败");
        search->setText(s.value(row));
    });
    connect(table_->selectionModel(), SIGNAL(selectionChanged(QItemSelection, QItemSelection)),
            this, SLOT(showDetails()));
    connect(engine, SIGNAL(taskUpdated(Task)), model_, SLOT(updateTask(Task)));
    connect(engine, SIGNAL(taskRemoved(QString)), model_, SLOT(removeTask(QString)));
    connect(engine, &DownloadEngine::taskUpdated, this, [this](const Task &) { showDetails(); });
    connect(engine, &DownloadEngine::problem, this, [this](const QString &s) {
        QMessageBox::warning(this, QString::fromUtf8("QDM 提示"), s);
    });
    connect(this, SIGNAL(addRequested(QString, QString, int)), engine,
            SLOT(addTask(QString, QString, int)), Qt::QueuedConnection);
    connect(this, SIGNAL(pauseRequested(QString)), engine, SLOT(pauseTask(QString)),
            Qt::QueuedConnection);
    connect(this, SIGNAL(resumeRequested(QString)), engine, SLOT(resumeTask(QString)),
            Qt::QueuedConnection);
    connect(this, SIGNAL(removeRequested(QString)), engine, SLOT(removeTask(QString)),
            Qt::QueuedConnection);
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        tray_ = new QSystemTrayIcon(icon, this);
        tray_->setToolTip("QDM");
        QMenu *m = new QMenu(this);
        QAction *show = m->addAction(QString::fromUtf8("显示主窗口"));
        QAction *quit = m->addAction(QString::fromUtf8("退出并保存任务"));
        tray_->setContextMenu(m);
        connect(show, &QAction::triggered, this, [this]() {
            showNormal();
            raise();
            activateWindow();
        });
        connect(quit, &QAction::triggered, qApp, &QApplication::quit);
        connect(tray_, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason == QSystemTrayIcon::DoubleClick) {
                        showNormal();
                        raise();
                        activateWindow();
                    }
                });
        tray_->show();
    }
}
/**
 * @brief 显示桌面控制的浏览器 profile 和配对入口。
 * 只有用户按复制按钮才把密钥写到本机剪贴板；保存目录与并发属于桌面配置，不接受网页直接修改。
 */
void MainWindow::browserConnection() {
    if (!bridge_)
        return;
    QDialog dialog(this);
    dialog.setWindowTitle(QString::fromUtf8("连接浏览器扩展"));
    dialog.resize(620, 280);
    QFormLayout *form = new QFormLayout(&dialog);
    QLabel *state = new QLabel(bridge_->isListening()
                                   ? QString::fromUtf8("桌面连接服务已启动 · 支持 Chrome / Edge")
                                   : QString::fromUtf8("连接服务未启动，请检查端口占用"));
    form->addRow(state);
    QLineEdit *directory = new QLineEdit(bridge_->downloadDirectory());
    form->addRow(QString::fromUtf8("扩展下载目录"), directory);
    QPushButton *browse = new QPushButton(QString::fromUtf8("选择目录"));
    form->addRow(browse);
    connect(browse, &QPushButton::clicked, &dialog, [this, directory]() {
        QString dir = QFileDialog::getExistingDirectory(this, QString::fromUtf8("扩展下载目录"),
                                                        directory->text());
        if (!dir.isEmpty())
            directory->setText(dir);
    });
    QSpinBox *count = new QSpinBox;
    count->setRange(1, 64);
    count->setValue(bridge_->connections());
    form->addRow(QString::fromUtf8("默认并发上限"), count);
    QLabel *help = new QLabel(
        QString::fromUtf8("安装本机宿主后，扩展可自动连接。便携模式可点击下方按钮复制配对信息，再粘"
                          "贴到扩展设置。配对信息只应交给你自己的 QDM 扩展。"));
    help->setWordWrap(true);
    form->addRow(help);
    QPushButton *copy = new QPushButton(QString::fromUtf8("复制便携配对信息"));
    copy->setEnabled(bridge_->isListening());
    form->addRow(copy);
    connect(copy, &QPushButton::clicked, &dialog, [this, copy]() {
        QApplication::clipboard()->setText(bridge_->pairingText());
        copy->setText(QString::fromUtf8("已复制"));
    });
    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [this, &dialog, directory, count]() {
        QString error;
        if (bridge_->setPreferences(directory->text(), count->value(), &error))
            dialog.accept();
        else
            QMessageBox::warning(&dialog, QString::fromUtf8("保存失败"), error);
    });
    connect(buttons, SIGNAL(rejected()), &dialog, SLOT(reject()));
    dialog.exec();
}
/**
 * @brief 将代理模型当前行转换回源模型，再获取值快照；排序后不能直接使用代理行号。
 */
Task MainWindow::selectedTask() const {
    return model_->taskAt(proxy_->mapToSource(table_->currentIndex()).row());
}
/**
 * @brief 收集手工下载的 URL、绝对目标路径及并发上限。
 * 对话框只产生命令，最终 URL/路径校验由共享引擎入口再次执行，避免 UI 校验成为唯一边界。
 */
void MainWindow::newTask() {
    QDialog d(this);
    d.setWindowTitle(QString::fromUtf8("新建下载"));
    d.resize(600, 200);
    QFormLayout *f = new QFormLayout(&d);
    QLineEdit *url = new QLineEdit;
    url->setPlaceholderText("https://example.com/file.zip");
    QLineEdit *path = new QLineEdit;
    QPushButton *browse = new QPushButton(QString::fromUtf8("浏览…"));
    QWidget *paths = new QWidget;
    QHBoxLayout *h = new QHBoxLayout(paths);
    h->setContentsMargins(0, 0, 0, 0);
    h->addWidget(path);
    h->addWidget(browse);
    QSpinBox *connections = new QSpinBox;
    connections->setRange(1, 64);
    connections->setValue(4);
    f->addRow(QString::fromUtf8("下载地址"), url);
    f->addRow(QString::fromUtf8("保存文件"), paths);
    f->addRow(QString::fromUtf8("分段并发上限"), connections);
    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    f->addRow(buttons);
    connect(buttons, SIGNAL(accepted()), &d, SLOT(accept()));
    connect(buttons, SIGNAL(rejected()), &d, SLOT(reject()));
    buttons->button(QDialogButtonBox::Ok)->setText(QString::fromUtf8("开始下载"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QString::fromUtf8("取消"));
    connect(url, &QLineEdit::editingFinished, &d, [url, path]() {
        if (path->text().isEmpty()) {
            QString name = QFileInfo(QUrl(url->text()).path()).fileName();
            name.replace(QRegExp("[<>:\"/\\\\|?*]"), "_");
            if (name.isEmpty())
                name = "download.bin";
            path->setText(QDir(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                              .filePath(name));
        }
    });
    connect(browse, &QPushButton::clicked, &d, [this, path]() {
        QString s = QFileDialog::getSaveFileName(this, QString::fromUtf8("保存到"), path->text());
        if (!s.isEmpty())
            path->setText(s);
    });
    if (d.exec() == QDialog::Accepted)
        emit addRequested(url->text().trimmed(), path->text().trimmed(), connections->value());
}
/**
 * @brief 对当前任务发暂停命令；实际状态在引擎检查点保存成功后通过快照更新。
 */
void MainWindow::pauseSelected() {
    Task t = selectedTask();
    if (!t.id.isEmpty())
        emit pauseRequested(t.id);
}
/**
 * @brief 对当前任务发恢复命令；不在 GUI 线程进行网络探测或摘要校验。
 */
void MainWindow::resumeSelected() {
    Task t = selectedTask();
    if (!t.id.isEmpty())
        emit resumeRequested(t.id);
}
/**
 * @brief 确认后移除任务及专属临时数据；已完成文件保留。
 * 弹窗文字与引擎实际删除范围必须一致，不能把移除记录扩展成删除下载目录。
 */
void MainWindow::removeSelected() {
    Task t = selectedTask();
    if (!t.id.isEmpty() &&
        QMessageBox::question(
            this, QString::fromUtf8("移除任务"),
            QString::fromUtf8("移除此任务并清理临时文件？已完成的文件会保留。")) ==
            QMessageBox::Yes)
        emit removeRequested(t.id);
}
/**
 * @brief 由用户操作打开选中任务所在目录，不将下载 URL 当作本地命令执行。
 */
void MainWindow::openFolder() {
    Task t = selectedTask();
    if (!t.id.isEmpty())
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(t.path).absolutePath()));
}
/**
 * @brief 展示当前行的可靠进度、分段能力和错误；全部使用纯文本，文件名不解释成 HTML。
 */
void MainWindow::showDetails() {
    Task t = selectedTask();
    if (t.id.isEmpty())
        return;
    details_->setText(
        t.path + "\n" + QString::fromUtf8("可恢复进度：") + byteLabel(t.committed) +
        QString::fromUtf8("  ·  分段支持：") +
        (t.ranges ? QString::fromUtf8("是") : QString::fromUtf8("否，恢复将重新下载")) +
        (t.error.isEmpty() ? QString() : "\n" + t.error));
}
/**
 * @brief 将普通关闭转成隐藏到托盘；没有托盘时采用正常关闭行为，避免出现无法找回的后台窗口。
 */
void MainWindow::closeEvent(QCloseEvent *e) {
    if (tray_ && tray_->isVisible()) {
        hide();
        e->ignore();
    } else
        QMainWindow::closeEvent(e);
}
