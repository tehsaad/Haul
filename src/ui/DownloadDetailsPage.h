#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;

namespace haul {
class DownloadManager;
}

namespace haul::ui {

class SegmentBar;
class StatusBadge;
class TaskActions;

// Everything about one download, with its controls. Values refresh live from the manager.
class DownloadDetailsPage : public QWidget
{
    Q_OBJECT
public:
    DownloadDetailsPage(DownloadManager *manager, TaskActions *actions, QWidget *parent = nullptr);

    void showTask(qint64 id);
    qint64 taskId() const { return m_id; }

private:
    void refresh();

    DownloadManager *m_manager;
    TaskActions *m_actions;
    qint64 m_id = 0;

    QStackedWidget *m_stack;
    QLabel *m_name;
    StatusBadge *m_badge;
    QProgressBar *m_progress;
    QLabel *m_progressText;
    QLabel *m_url;
    QLabel *m_destination;
    QLabel *m_downloaded;
    QLabel *m_total;
    QLabel *m_speed;
    QLabel *m_eta;
    QLabel *m_started;
    QLabel *m_completed;
    QLabel *m_retries;
    QLabel *m_connections;
    QLabel *m_checksum;
    QLabel *m_error;
    QWidget *m_errorCard;
    QWidget *m_segmentCard;
    SegmentBar *m_segmentBar;
    QLabel *m_segmentText;

    QPushButton *m_pause;
    QPushButton *m_resume;
    QPushButton *m_cancel;
    QPushButton *m_retry;
    QPushButton *m_openFolder;
    QPushButton *m_copyUrl;
    QPushButton *m_remove;
};

} // namespace haul::ui
