#include "MainWindow.h"

#include "AddDownloadDialog.h"
#include "DashboardPage.h"
#include "DownloadDetailsPage.h"
#include "DownloadTableModel.h"
#include "QueuePage.h"
#include "SettingsPage.h"
#include "TaskActions.h"
#include "Theme.h"
#include "Widgets.h"
#include "app/DownloadManager.h"
#include "core/Formatters.h"
#include "scheduler/Scheduler.h"
#include "storage/SettingsRepository.h"

#include <QApplication>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QStackedWidget>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

namespace haul::ui {

MainWindow::MainWindow(DownloadManager *manager, SettingsRepository *settings, Scheduler *scheduler, QWidget *parent)
    : QMainWindow(parent)
    , m_manager(manager)
    , m_settings(settings)
    , m_scheduler(scheduler)
{
    setWindowTitle(QStringLiteral("Haul"));
    setWindowIcon(QIcon(QStringLiteral(":/haul.svg")));
    resize(1120, 680);
    setMinimumSize(820, 500);

    m_model = new DownloadTableModel(m_manager, this);
    m_actions = new TaskActions(m_manager, m_settings, this);

    auto *central = new QWidget(this);
    auto *layout = new QHBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(buildSidebar());

    m_stack = new QStackedWidget(central);
    m_dashboard = new DashboardPage(m_manager, m_model, m_actions, m_stack);
    m_queue = new QueuePage(m_manager, m_settings, m_scheduler, m_model, m_actions, m_stack);
    m_details = new DownloadDetailsPage(m_manager, m_actions, m_stack);
    m_settingsPage = new SettingsPage(m_settings, m_stack);
    m_stack->addWidget(m_dashboard);
    m_stack->addWidget(m_queue);
    m_stack->addWidget(m_details);
    m_stack->addWidget(m_settingsPage);
    layout->addWidget(m_stack, 1);
    setCentralWidget(central);

    m_toasts = new ToastHost(central);

    connect(m_dashboard, &DashboardPage::addRequested, this, [this] { openAddDialog(); });
    connect(m_actions, &TaskActions::detailsRequested, this, &MainWindow::showDetails);
    connect(m_actions, &TaskActions::message, this, &MainWindow::notify);
    connect(m_manager, &DownloadManager::notification, this, &MainWindow::notify);
    connect(m_scheduler, &Scheduler::acted, this, [this](const QString &message) {
        notify(QStringLiteral("Scheduler"), message, false);
    });
    connect(m_manager, &DownloadManager::statsChanged, this, &MainWindow::updateSidebarStats);
    connect(m_manager, &DownloadManager::queueStateChanged, this, &MainWindow::updateSidebarStats);
    connect(m_settings, &SettingsRepository::changed, this, [](const AppSettings &s) {
        if ((s.theme == QLatin1String("dark")) != Theme::isDark())
            Theme::apply(*qApp, s.theme);
    });

    // Keyboard shortcuts
    auto shortcut = [this](const QKeySequence &keys, auto slot) {
        auto *s = new QShortcut(keys, this);
        connect(s, &QShortcut::activated, this, slot);
    };
    shortcut(QKeySequence::New, [this] { openAddDialog(); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+V")), [this] {
        if (QApplication::focusWidget() && QApplication::focusWidget()->inherits("QLineEdit"))
            return; // let text fields paste normally
        openAddDialog();
    });
    shortcut(QKeySequence::Find, [this] {
        showPage(Dashboard);
        m_dashboard->focusSearch();
    });
    shortcut(QKeySequence(QStringLiteral("Ctrl+1")), [this] { showPage(Dashboard); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+2")), [this] { showPage(Queue); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+3")), [this] { showPage(Details); });
    shortcut(QKeySequence(QStringLiteral("Ctrl+,")), [this] { showPage(Settings); });
    auto *space = new QShortcut(QKeySequence(Qt::Key_Space), m_dashboard->table());
    space->setContext(Qt::WidgetShortcut);
    connect(space, &QShortcut::activated, this, &MainWindow::toggleSelected);
    auto *del = new QShortcut(QKeySequence::Delete, m_dashboard->table());
    del->setContext(Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, &MainWindow::removeSelected);
    auto *enter = new QShortcut(QKeySequence(Qt::Key_Return), m_dashboard->table());
    enter->setContext(Qt::WidgetShortcut);
    connect(enter, &QShortcut::activated, this, [this] {
        if (const qint64 id = m_dashboard->selectedTaskId())
            showDetails(id);
    });

    showPage(Dashboard);
    updateSidebarStats();
}

QWidget *MainWindow::buildSidebar()
{
    auto *sidebar = new QWidget(this);
    sidebar->setObjectName(QStringLiteral("Sidebar"));
    sidebar->setAttribute(Qt::WA_StyledBackground);
    sidebar->setFixedWidth(148);
    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(0, 10, 0, 8);
    layout->setSpacing(0);

    auto *brand = new QHBoxLayout;
    brand->setContentsMargins(12, 0, 12, 10);
    brand->setSpacing(7);
    auto *logo = new QLabel(sidebar);
    logo->setPixmap(QIcon(QStringLiteral(":/haul.svg")).pixmap(20, 20));
    auto *name = new QLabel(QStringLiteral("Haul"), sidebar);
    name->setObjectName(QStringLiteral("BrandName"));
    brand->addWidget(logo);
    brand->addWidget(name, 1);
    layout->addLayout(brand);

    m_nav = new QButtonGroup(this);
    m_nav->setExclusive(true);
    const struct {
        Page page;
        const char *text;
        const char *tip;
    } entries[] = {
        { Dashboard, "Downloads", "Ctrl+1" },
        { Queue, "Queue", "Ctrl+2" },
        { Details, "Details", "Ctrl+3" },
        { Settings, "Settings", "Ctrl+," },
    };
    for (const auto &e : entries) {
        auto *button = new QPushButton(sidebar);
        button->setObjectName(QStringLiteral("NavButton"));
        button->setText(QString::fromLatin1(e.text));
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setToolTip(QString::fromLatin1(e.tip));
        m_nav->addButton(button, e.page);
        layout->addWidget(button);
    }
    connect(m_nav, &QButtonGroup::idClicked, this, [this](int id) { showPage(static_cast<Page>(id)); });
    layout->addStretch();

    m_sidebarStatus = new QLabel(this);
    m_sidebarSpeed = new QLabel(this);
    m_sidebarSpeed->setObjectName(QStringLiteral("StatusSpeed"));
    m_sidebarStatus->setContentsMargins(8, 0, 8, 0);
    m_sidebarSpeed->setContentsMargins(8, 0, 8, 0);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addWidget(m_sidebarStatus, 1);
    statusBar()->addPermanentWidget(m_sidebarSpeed);
    return sidebar;
}

void MainWindow::updateSidebarStats()
{
    const DownloadStats s = m_manager->stats();
    m_sidebarSpeed->setText(s.speed > 0 ? formatSpeed(s.speed) : QString());
    QString text = m_manager->isQueueRunning() ? QStringLiteral("Queue running") : QStringLiteral("Queue stopped");
    if (m_settings->current().speedLimitKiB > 0)
        text += QStringLiteral("  ·  limited to %1/s").arg(formatBytes(qint64(m_settings->current().speedLimitKiB) * 1024));
    m_sidebarStatus->setText(text);
}

QWidget *MainWindow::pageWidget(Page page) const
{
    return m_stack->widget(page);
}

void MainWindow::showPage(Page page)
{
    if (page == Details)
        m_details->showTask(m_details->taskId());
    m_stack->setCurrentIndex(page);
    if (QAbstractButton *button = m_nav->button(page))
        button->setChecked(true);
}

void MainWindow::showDetails(qint64 taskId)
{
    m_details->showTask(taskId);
    showPage(Details);
}

void MainWindow::bringToFront()
{
    if (isMinimized())
        showNormal();
    show();
    raise();
    activateWindow();
}

void MainWindow::askAboutDownload(const QString &url, const QString &fileName)
{
    m_pendingLinks.append({ url, fileName });
    if (m_pendingLinks.size() == 1) // otherwise the loop below is already showing a dialog
        QTimer::singleShot(0, this, &MainWindow::showNextPendingLink);
}

void MainWindow::showNextPendingLink()
{
    if (m_pendingLinks.isEmpty())
        return;
    if (QApplication::activeModalWidget()) { // the user is busy in another dialog: try again shortly
        QTimer::singleShot(300, this, &MainWindow::showNextPendingLink);
        return;
    }
    bringToFront();
    const QPair<QString, QString> link = m_pendingLinks.first();
    openAddDialog(link.first, link.second);
    m_pendingLinks.removeFirst();
    showNextPendingLink();
}

qint64 MainWindow::openAddDialog(const QString &url, const QString &fileName)
{
    AddDownloadDialog dialog(m_manager, m_settings, this);
    if (!url.isEmpty())
        dialog.setUrl(url);
    if (!fileName.isEmpty())
        dialog.setFileName(fileName);
    if (dialog.exec() != QDialog::Accepted)
        return 0;
    if (const DownloadTask *t = m_manager->task(dialog.createdTaskId())) {
        notify(t->state == TaskState::Queued && !m_manager->isQueueRunning() && !t->startRequested
                   ? QStringLiteral("Added to queue") : QStringLiteral("Download added"),
               t->fileName, false);
    }
    return dialog.createdTaskId();
}

void MainWindow::notify(const QString &title, const QString &message, bool isError)
{
    m_toasts->show(title, message, isError);
}

void MainWindow::toggleSelected()
{
    const qint64 id = m_dashboard->selectedTaskId();
    const DownloadTask *t = m_manager->task(id);
    if (!t)
        return;
    if (t->state == TaskState::Downloading || t->state == TaskState::Queued)
        m_manager->pause(id);
    else if (t->state == TaskState::Paused || t->state == TaskState::Failed)
        m_manager->resume(id);
}

void MainWindow::removeSelected()
{
    if (const qint64 id = m_dashboard->selectedTaskId())
        m_actions->removeWithConfirmation(id);
}

} // namespace haul::ui
