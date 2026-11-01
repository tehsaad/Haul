#pragma once

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTabBar;
class QTableView;
class QStackedWidget;
class QTimeEdit;

namespace haul {
class DownloadManager;
class Scheduler;
class SettingsRepository;
}

namespace haul::ui {

class DownloadFilterModel;
class DownloadTableModel;
class EmptyState;
class TaskActions;

// Queue management: start/stop the queue, order and prioritise waiting downloads,
// concurrency limits and the daily schedule.
class QueuePage : public QWidget
{
    Q_OBJECT
public:
    QueuePage(DownloadManager *manager, SettingsRepository *settings, Scheduler *scheduler,
              DownloadTableModel *model, TaskActions *actions, QWidget *parent = nullptr);

private:
    qint64 selectedId() const;
    void refreshQueueStatus();
    void refreshSelection();
    void updateEmptyState();
    void loadSettings();
    void saveLimits();
    void saveSchedule();

    DownloadManager *m_manager;
    SettingsRepository *m_settings;
    Scheduler *m_scheduler;
    TaskActions *m_actions;
    DownloadFilterModel *m_filter;

    QPushButton *m_toggleQueue;
    QLabel *m_queueStatus;
    QProgressBar *m_queueProgress;
    QTabBar *m_tabs;
    QTableView *m_table;
    QStackedWidget *m_listStack;
    EmptyState *m_empty;
    QComboBox *m_priority;
    QPushButton *m_moveUp;
    QPushButton *m_moveDown;
    QSpinBox *m_maxConcurrent;
    QSpinBox *m_maxConnections;
    QLabel *m_limitsStatus;
    QCheckBox *m_scheduleEnabled;
    QTimeEdit *m_startTime;
    QCheckBox *m_stopEnabled;
    QTimeEdit *m_stopTime;
    QLabel *m_scheduleSummary;
    QLabel *m_scheduleError;
};

} // namespace haul::ui
