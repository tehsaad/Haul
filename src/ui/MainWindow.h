#pragma once

#include <QMainWindow>

class QButtonGroup;
class QLabel;
class QStackedWidget;

namespace haul {
class DownloadManager;
class Scheduler;
class SettingsRepository;
}

namespace haul::ui {

class DashboardPage;
class DownloadDetailsPage;
class DownloadTableModel;
class QueuePage;
class SettingsPage;
class TaskActions;
class ToastHost;

// Application shell: sidebar navigation on the left, the current page on the right.
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    enum Page { Dashboard = 0, Queue = 1, Details = 2, Settings = 3 };

    MainWindow(DownloadManager *manager, SettingsRepository *settings, Scheduler *scheduler, QWidget *parent = nullptr);

    void showPage(Page page);
    void showDetails(qint64 taskId);
    // Opens the Add Download dialog, optionally pre-filled. Returns the new task id or 0.
    qint64 openAddDialog(const QString &url = QString(), const QString &fileName = QString(),
                         const QString &referrer = QString(), const QString &variant = QString());
    void bringToFront();
    // A link from outside (the browser): opens the Add dialog; several links wait their turn.
    void askAboutDownload(const QString &url, const QString &fileName, const QString &referrer = QString(),
                          const QString &variant = QString());
    void notify(const QString &title, const QString &message, bool isError);

    DashboardPage *dashboardPage() const { return m_dashboard; }
    DownloadDetailsPage *detailsPage() const { return m_details; }
    QWidget *pageWidget(Page page) const;

private:
    void showNextPendingLink();
    struct PendingLink {
        QString url;
        QString fileName;
        QString referrer;
        QString variant;
    };
    QVector<PendingLink> m_pendingLinks;
    QWidget *buildSidebar();
    void updateSidebarStats();
    void toggleSelected();
    void removeSelected();

    DownloadManager *m_manager;
    SettingsRepository *m_settings;
    Scheduler *m_scheduler;
    DownloadTableModel *m_model;
    TaskActions *m_actions;
    ToastHost *m_toasts;

    QStackedWidget *m_stack;
    QButtonGroup *m_nav;
    DashboardPage *m_dashboard;
    QueuePage *m_queue;
    DownloadDetailsPage *m_details;
    SettingsPage *m_settingsPage;
    QLabel *m_sidebarSpeed;
    QLabel *m_sidebarStatus;
};

} // namespace haul::ui
