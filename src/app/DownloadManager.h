#pragma once

#include "core/DownloadTask.h"
#include "core/RetryPolicy.h"
#include "core/SpeedMeter.h"
#include "network/DownloadWorker.h"
#include "storage/SettingsRepository.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QTimer>

#include <map>
#include <memory>

class QThread;

namespace haul {

class DownloadRepository;

struct DownloadStats {
    int total = 0;
    int active = 0;
    int queued = 0;
    int paused = 0;
    int completed = 0;
    int failed = 0;
    double speed = 0.0;      // bytes/s across all active downloads
    qint64 downloadedBytes = 0; // across all tasks
};

struct AddRequest {
    QString url;
    QString directory;
    QString fileName;       // optional; sanitized and made unique
    bool startNow = true;
    Priority priority = Priority::Normal;
    QString expectedSha256; // optional trusted checksum
};

// Owns every DownloadTask and coordinates their life cycle. Lives on the GUI thread;
// each running transfer has its own worker thread and talks to the manager only through
// queued signals, so no data is shared between threads except the atomic TransferControl.
class DownloadManager : public QObject
{
    Q_OBJECT
public:
    DownloadManager(DownloadRepository *repository, SettingsRepository *settings, QObject *parent = nullptr);
    ~DownloadManager() override;

    // Loads tasks from the database and applies the recovery policy. Call once at startup.
    void restore();

    // Returns the new task id, or 0 and an explanation in *error.
    qint64 addDownload(const AddRequest &request, QString *error = nullptr);

    bool pause(qint64 id);
    bool resume(qint64 id);
    bool cancel(qint64 id);
    bool retry(qint64 id);
    bool remove(qint64 id, bool deleteFile);
    void pauseAll();
    void resumeAll();

    void startQueue();
    void stopQueue();
    bool isQueueRunning() const { return m_queueRunning; }

    bool moveUp(qint64 id);
    bool moveDown(qint64 id);
    bool setPriority(qint64 id, Priority priority);

    const DownloadTask *task(qint64 id) const;
    QVector<DownloadTask> tasks() const;
    QVector<DownloadTask> queueOrder() const;
    DownloadStats stats() const;
    int activeCount() const { return m_active.size(); }

    // True if the same URL is already waiting or downloading into the same folder.
    bool isDuplicate(const QString &url, const QString &directory) const;

    // Stops all transfers and waits for their threads (used on exit).
    void shutdown();

signals:
    void taskAdded(qint64 id);
    void taskUpdated(qint64 id);
    void taskRemoved(qint64 id);
    void queueStateChanged(bool running);
    void statsChanged();
    void notification(const QString &title, const QString &message, bool isError);

private slots:
    void onMetadata(qint64 taskId, const QString &fileName, qint64 totalBytes, bool acceptRanges, int connections);
    void onProgress(qint64 taskId, qint64 downloaded, qint64 total, QVector<haul::SegmentInfo> segments);
    void onFinished(haul::WorkerOutcome outcome);
    void onSettingsChanged(const haul::AppSettings &settings);
    void tick();

private:
    enum class StopReason { None, Pause, Cancel, Requeue, Remove, Shutdown };

    struct ActiveTransfer {
        QThread *thread = nullptr;
        DownloadWorker *worker = nullptr;
        std::shared_ptr<TransferControl> control;
        StopReason reason = StopReason::None;
        bool deleteFilesOnRemove = false;
        SpeedMeter meter;
    };

    DownloadTask *find(qint64 id);
    bool changeState(DownloadTask &task, TaskState to);
    void schedule();
    bool startTask(DownloadTask &task);
    void stopTransfer(qint64 id, StopReason reason);
    void rebalanceBandwidth();
    void persist(const DownloadTask &task);
    void deletePartialFiles(const DownloadTask &task);
    QSet<QString> reservedNames(const QString &directory) const;
    RetryPolicy retryPolicy() const;

    DownloadRepository *m_repository;
    SettingsRepository *m_settings;
    std::map<qint64, DownloadTask> m_tasks;
    QHash<qint64, ActiveTransfer> m_active;
    QSet<qint64> m_dirtyProgress;
    bool m_queueRunning = false;
    QTimer m_tick;
};

} // namespace haul
