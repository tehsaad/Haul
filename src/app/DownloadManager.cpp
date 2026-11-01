#include "DownloadManager.h"

#include "core/Checksum.h"
#include "core/FileNameUtils.h"
#include "core/SegmentPlan.h"
#include "core/UrlValidator.h"
#include "scheduler/DownloadQueue.h"
#include "storage/DownloadRepository.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>
#include <utility>

namespace haul {

DownloadManager::DownloadManager(DownloadRepository *repository, SettingsRepository *settings, QObject *parent)
    : QObject(parent)
    , m_repository(repository)
    , m_settings(settings)
{
    qRegisterMetaType<haul::WorkerOutcome>();
    qRegisterMetaType<haul::SegmentInfo>();
    qRegisterMetaType<QVector<haul::SegmentInfo>>();

    connect(m_settings, &SettingsRepository::changed, this, &DownloadManager::onSettingsChanged);
    connect(&m_tick, &QTimer::timeout, this, &DownloadManager::tick);
    m_tick.start(1000);
}

DownloadManager::~DownloadManager()
{
    shutdown();
}

// ---------------------------------------------------------------- lookup helpers

DownloadTask *DownloadManager::find(qint64 id)
{
    auto it = m_tasks.find(id);
    return it == m_tasks.end() ? nullptr : &it->second;
}

const DownloadTask *DownloadManager::task(qint64 id) const
{
    auto it = m_tasks.find(id);
    return it == m_tasks.end() ? nullptr : &it->second;
}

QVector<DownloadTask> DownloadManager::tasks() const
{
    QVector<DownloadTask> list;
    list.reserve(static_cast<int>(m_tasks.size()));
    for (const auto &pair : m_tasks)
        list.append(pair.second);
    return list;
}

QVector<DownloadTask> DownloadManager::queueOrder() const
{
    QVector<QueueEntry> entries;
    for (const auto &pair : m_tasks)
        entries.append(queueEntryFor(pair.second));
    QVector<DownloadTask> list;
    for (const QueueEntry &entry : DownloadQueue::ordered(entries))
        list.append(m_tasks.at(entry.id));
    return list;
}

DownloadStats DownloadManager::stats() const
{
    DownloadStats s;
    for (const auto &pair : m_tasks) {
        const DownloadTask &t = pair.second;
        ++s.total;
        s.downloadedBytes += t.downloadedBytes;
        switch (t.state) {
        case TaskState::Downloading:
            ++s.active;
            s.speed += t.speed;
            break;
        case TaskState::Queued: ++s.queued; break;
        case TaskState::Paused: ++s.paused; break;
        case TaskState::Completed: ++s.completed; break;
        case TaskState::Failed: ++s.failed; break;
        case TaskState::Cancelled: break;
        }
    }
    return s;
}

bool DownloadManager::isDuplicate(const QString &url, const QString &directory) const
{
    const QString cleanDir = QDir::cleanPath(directory);
    for (const auto &pair : m_tasks) {
        const DownloadTask &t = pair.second;
        if (isFinishedState(t.state))
            continue;
        if (t.url == url && QDir::cleanPath(t.directory) == cleanDir)
            return true;
    }
    return false;
}

QSet<QString> DownloadManager::reservedNames(const QString &directory) const
{
    QSet<QString> names;
    const QString cleanDir = QDir::cleanPath(directory);
    for (const auto &pair : m_tasks) {
        const DownloadTask &t = pair.second;
        if (!isFinishedState(t.state) && QDir::cleanPath(t.directory) == cleanDir)
            names.insert(t.fileName);
    }
    return names;
}

RetryPolicy DownloadManager::retryPolicy() const
{
    const AppSettings &s = m_settings->current();
    RetryPolicy policy;
    policy.enabled = s.autoRetry;
    policy.maxRetries = s.maxRetries;
    policy.baseDelaySeconds = s.retryDelaySeconds;
    return policy;
}

bool DownloadManager::changeState(DownloadTask &task, TaskState to)
{
    if (!canTransition(task.state, to))
        return false;
    task.state = to;
    return true;
}

void DownloadManager::persist(const DownloadTask &task)
{
    if (m_repository)
        m_repository->update(task);
}

void DownloadManager::deletePartialFiles(const DownloadTask &task)
{
    QFile::remove(task.partPath());
    for (int i = 0; i < kMaxSegments; ++i)
        QFile::remove(task.segmentPath(i));
}

// ---------------------------------------------------------------- startup

void DownloadManager::restore()
{
    if (!m_repository)
        return;

    QVector<DownloadTask> loaded = m_repository->loadAll();
    const int interrupted = applyStartupRecovery(loaded);
    for (DownloadTask &t : loaded) {
        if (t.interrupted || t.state != TaskState::Completed)
            persist(t);
        m_tasks.emplace(t.id, t);
        emit taskAdded(t.id);
    }
    if (interrupted > 0) {
        emit notification(QStringLiteral("Downloads interrupted"),
                          QStringLiteral("%1 download(s) were interrupted last time. Resume them when you are ready.").arg(interrupted),
                          false);
    }
    if (m_settings->current().startQueueOnLaunch)
        startQueue();
    schedule();
    emit statsChanged();
}

// ---------------------------------------------------------------- adding

qint64 DownloadManager::addDownload(const AddRequest &request, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return qint64(0);
    };

    const UrlCheck check = validateDownloadUrl(request.url);
    if (!check.ok)
        return fail(check.error);
    const QString url = check.url.toString(QUrl::FullyEncoded);

    const QString directory = QDir::cleanPath(request.directory.trimmed());
    if (directory.isEmpty() || !QDir::isAbsolutePath(directory))
        return fail(QStringLiteral("Choose a destination folder."));
    if (!QDir().mkpath(directory))
        return fail(QStringLiteral("The destination folder cannot be created."));
    if (!QFileInfo(directory).isWritable())
        return fail(QStringLiteral("You do not have permission to save files in this folder."));

    if (isDuplicate(url, directory))
        return fail(QStringLiteral("This URL is already in the download list for that folder."));

    const QString sha = request.expectedSha256.trimmed().toLower();
    if (!sha.isEmpty() && !isValidSha256Hex(sha))
        return fail(QStringLiteral("The SHA-256 checksum must be 64 hexadecimal characters."));

    QString name = request.fileName.trimmed().isEmpty() ? fileNameFromUrl(check.url)
                                                        : sanitizeFileName(request.fileName);
    name = uniqueFileName(directory, name, reservedNames(directory));

    QVector<QueueEntry> entries;
    for (const auto &pair : m_tasks)
        entries.append(queueEntryFor(pair.second));

    DownloadTask task;
    task.url = url;
    task.directory = directory;
    task.fileName = name;
    task.autoName = request.fileName.trimmed().isEmpty();
    task.state = TaskState::Queued;
    task.priority = request.priority;
    task.queuePosition = DownloadQueue::nextPosition(entries);
    task.expectedSha256 = sha;
    task.createdAt = QDateTime::currentDateTime();
    task.startRequested = request.startNow;

    if (m_repository && !m_repository->insert(task))
        return fail(QStringLiteral("Could not save the download to the database."));
    if (!m_repository)
        task.id = m_tasks.empty() ? 1 : m_tasks.rbegin()->first + 1;

    const qint64 id = task.id;
    if (!request.login.isEmpty())
        m_logins.insert(id, request.login);
    m_tasks.emplace(id, task);
    emit taskAdded(id);
    schedule();
    emit statsChanged();
    return id;
}

// ---------------------------------------------------------------- user actions

bool DownloadManager::pause(qint64 id)
{
    DownloadTask *t = find(id);
    if (!t)
        return false;
    if (t->state == TaskState::Downloading) {
        stopTransfer(id, StopReason::Pause);
        return true;
    }
    if (!changeState(*t, TaskState::Paused))
        return false;
    t->retryAt = QDateTime();
    t->startRequested = false;
    persist(*t);
    emit taskUpdated(id);
    emit statsChanged();
    return true;
}

bool DownloadManager::resume(qint64 id)
{
    DownloadTask *t = find(id);
    if (!t || (t->state != TaskState::Paused && t->state != TaskState::Failed))
        return false;
    if (!changeState(*t, TaskState::Queued))
        return false;
    t->startRequested = true;
    t->retryAt = QDateTime();
    t->retryCount = 0;
    t->errorMessage.clear();
    persist(*t);
    emit taskUpdated(id);
    schedule();
    emit statsChanged();
    return true;
}

bool DownloadManager::retry(qint64 id)
{
    DownloadTask *t = find(id);
    if (!t)
        return false;
    if (t->state == TaskState::Paused || t->state == TaskState::Failed)
        return resume(id);
    if (t->state == TaskState::Queued && t->retryAt.isValid()) {
        t->retryAt = QDateTime(); // "retry now" instead of waiting for the back-off delay
        schedule();
        return true;
    }
    if (t->state != TaskState::Cancelled || !changeState(*t, TaskState::Queued))
        return false;
    t->startRequested = true;
    t->retryCount = 0;
    t->errorMessage.clear();
    t->downloadedBytes = 0;
    t->segments.clear();
    persist(*t);
    emit taskUpdated(id);
    schedule();
    emit statsChanged();
    return true;
}

bool DownloadManager::logIn(qint64 id, const DownloadLogin &login)
{
    const DownloadTask *t = task(id);
    if (!t || t->state == TaskState::Completed)
        return false;
    m_logins.insert(id, login);
    if (t->state == TaskState::Paused || t->state == TaskState::Failed || t->state == TaskState::Cancelled)
        return retry(id);
    return true;
}

bool DownloadManager::cancel(qint64 id)
{
    DownloadTask *t = find(id);
    if (!t)
        return false;
    if (t->state == TaskState::Downloading) {
        stopTransfer(id, StopReason::Cancel);
        return true;
    }
    if (!changeState(*t, TaskState::Cancelled))
        return false;
    deletePartialFiles(*t);
    t->downloadedBytes = 0;
    t->segments.clear();
    t->retryAt = QDateTime();
    t->errorMessage.clear();
    persist(*t);
    emit taskUpdated(id);
    emit statsChanged();
    return true;
}

bool DownloadManager::remove(qint64 id, bool deleteFile)
{
    DownloadTask *t = find(id);
    if (!t)
        return false;
    if (m_active.contains(id)) {
        m_active[id].deleteFilesOnRemove = deleteFile;
        stopTransfer(id, StopReason::Remove);
        return true;
    }
    if (t->state != TaskState::Completed)
        deletePartialFiles(*t);
    else if (deleteFile)
        QFile::remove(t->finalPath());
    if (m_repository)
        m_repository->remove(id);
    m_tasks.erase(id);
    m_logins.remove(id);
    m_dirtyProgress.remove(id);
    emit taskRemoved(id);
    emit statsChanged();
    return true;
}

void DownloadManager::pauseAll()
{
    QVector<qint64> ids;
    for (const auto &pair : m_tasks) {
        if (pair.second.state == TaskState::Downloading || pair.second.state == TaskState::Queued)
            ids.append(pair.first);
    }
    for (qint64 id : ids)
        pause(id);
}

void DownloadManager::resumeAll()
{
    QVector<qint64> ids;
    for (const auto &pair : m_tasks) {
        if (pair.second.state == TaskState::Paused)
            ids.append(pair.first);
    }
    for (qint64 id : ids)
        resume(id);
}

void DownloadManager::startQueue()
{
    if (m_queueRunning)
        return;
    m_queueRunning = true;
    emit queueStateChanged(true);
    schedule();
}

void DownloadManager::stopQueue()
{
    if (!m_queueRunning)
        return;
    m_queueRunning = false;
    // Downloads the queue started go back to waiting; downloads the user started stay running.
    QVector<qint64> ids;
    for (auto it = m_active.cbegin(); it != m_active.cend(); ++it) {
        const DownloadTask *t = task(it.key());
        if (t && t->startedByQueue)
            ids.append(it.key());
    }
    for (qint64 id : ids)
        stopTransfer(id, StopReason::Requeue);
    emit queueStateChanged(false);
}

bool DownloadManager::setPriority(qint64 id, Priority priority)
{
    DownloadTask *t = find(id);
    if (!t || t->state == TaskState::Completed)
        return false;
    t->priority = priority;
    persist(*t);
    emit taskUpdated(id);
    schedule();
    return true;
}

bool DownloadManager::moveUp(qint64 id)
{
    QVector<QueueEntry> entries;
    for (const auto &pair : m_tasks)
        entries.append(queueEntryFor(pair.second));
    if (!DownloadQueue::move(entries, id, -1))
        return false;
    for (const QueueEntry &entry : entries) {
        DownloadTask *t = find(entry.id);
        if (t->queuePosition != entry.queuePosition || t->priority != entry.priority) {
            t->queuePosition = entry.queuePosition;
            t->priority = entry.priority;
            persist(*t);
            emit taskUpdated(t->id);
        }
    }
    return true;
}

bool DownloadManager::moveDown(qint64 id)
{
    QVector<QueueEntry> entries;
    for (const auto &pair : m_tasks)
        entries.append(queueEntryFor(pair.second));
    if (!DownloadQueue::move(entries, id, +1))
        return false;
    for (const QueueEntry &entry : entries) {
        DownloadTask *t = find(entry.id);
        if (t->queuePosition != entry.queuePosition || t->priority != entry.priority) {
            t->queuePosition = entry.queuePosition;
            t->priority = entry.priority;
            persist(*t);
            emit taskUpdated(t->id);
        }
    }
    return true;
}

// ---------------------------------------------------------------- scheduling

void DownloadManager::schedule()
{
    QVector<QueueEntry> entries;
    for (const auto &pair : m_tasks)
        entries.append(queueEntryFor(pair.second));

    const QVector<qint64> toStart = DownloadQueue::selectToStart(
        entries, m_active.size(), m_settings->current().maxConcurrentDownloads, m_queueRunning,
        QDateTime::currentDateTime());
    for (qint64 id : toStart) {
        if (DownloadTask *t = find(id))
            startTask(*t);
    }
}

bool DownloadManager::startTask(DownloadTask &task)
{
    if (m_active.contains(task.id)) // never run two workers for one task
        return false;
    if (!changeState(task, TaskState::Downloading))
        return false;

    if (!task.startedAt.isValid())
        task.startedAt = QDateTime::currentDateTime();
    task.startedByQueue = !task.startRequested;
    task.startRequested = false;
    task.retryAt = QDateTime();
    task.interrupted = false;
    task.speed = 0.0;

    const AppSettings &settings = m_settings->current();
    ActiveTransfer transfer;
    transfer.control = std::make_shared<TransferControl>();
    transfer.thread = new QThread(this);
    NetworkOptions options;
    options.login = m_logins.value(task.id);
    transfer.worker = new DownloadWorker(task, settings.maxConnectionsPerDownload, options, transfer.control);
    transfer.worker->moveToThread(transfer.thread);

    connect(transfer.thread, &QThread::started, transfer.worker, &DownloadWorker::run);
    connect(transfer.worker, &DownloadWorker::metadataReady, this, &DownloadManager::onMetadata);
    connect(transfer.worker, &DownloadWorker::progress, this, &DownloadManager::onProgress);
    connect(transfer.worker, &DownloadWorker::finished, this, &DownloadManager::onFinished);
    connect(transfer.thread, &QThread::finished, transfer.worker, &QObject::deleteLater);
    connect(transfer.thread, &QThread::finished, transfer.thread, &QObject::deleteLater);

    m_active.insert(task.id, transfer);
    rebalanceBandwidth();
    transfer.thread->start();

    persist(task);
    emit taskUpdated(task.id);
    emit statsChanged();
    return true;
}

void DownloadManager::stopTransfer(qint64 id, StopReason reason)
{
    auto it = m_active.find(id);
    if (it == m_active.end())
        return;
    if (it->reason == StopReason::None || reason == StopReason::Remove || reason == StopReason::Cancel)
        it->reason = reason;
    it->control->stop.store(true);
}

void DownloadManager::rebalanceBandwidth()
{
    const qint64 limit = static_cast<qint64>(m_settings->current().speedLimitKiB) * 1024;
    const qint64 share = (limit > 0 && !m_active.isEmpty()) ? std::max<qint64>(limit / m_active.size(), 1024) : 0;
    for (auto &transfer : m_active)
        transfer.control->limiter.setLimit(share);
}

// ---------------------------------------------------------------- worker callbacks

void DownloadManager::onMetadata(qint64 taskId, const QString &fileName, qint64 totalBytes, bool acceptRanges, int)
{
    DownloadTask *t = find(taskId);
    if (!t || !m_active.contains(taskId))
        return;
    t->totalBytes = totalBytes;
    t->acceptRanges = acceptRanges;
    if (t->fileName != fileName || t->autoName) {
        t->fileName = fileName;
        t->autoName = false;
        persist(*t);
    }
    emit taskUpdated(taskId);
}

void DownloadManager::onProgress(qint64 taskId, qint64 downloaded, qint64 total, QVector<SegmentInfo> segments)
{
    DownloadTask *t = find(taskId);
    auto it = m_active.find(taskId);
    if (!t || it == m_active.end())
        return;
    t->downloadedBytes = downloaded;
    if (total >= 0)
        t->totalBytes = total;
    t->segments = segments;
    it->meter.addSample(downloaded, QDateTime::currentMSecsSinceEpoch());
    t->speed = it->meter.bytesPerSecond();
    m_dirtyProgress.insert(taskId);
    emit taskUpdated(taskId);
}

void DownloadManager::onFinished(WorkerOutcome outcome)
{
    const qint64 id = outcome.task.id;
    auto it = m_active.find(id);
    if (it == m_active.end())
        return; // already handled by shutdown()

    const StopReason reason = it->reason;
    const bool deleteFiles = it->deleteFilesOnRemove;
    it->thread->quit();
    m_active.erase(it);
    m_dirtyProgress.remove(id);

    DownloadTask *t = find(id);
    if (!t)
        return;

    // Copy back what the worker learned.
    t->fileName = outcome.task.fileName;
    t->autoName = outcome.task.autoName;
    t->totalBytes = outcome.task.totalBytes;
    t->downloadedBytes = outcome.task.downloadedBytes;
    t->etag = outcome.task.etag;
    t->lastModified = outcome.task.lastModified;
    t->acceptRanges = outcome.task.acceptRanges;
    t->segments = outcome.task.segments;
    t->checksumStatus = outcome.task.checksumStatus;
    t->speed = 0.0;

    const TransferError &error = outcome.error;

    if (reason == StopReason::Remove) {
        if (error.isError())
            deletePartialFiles(*t);
        else if (deleteFiles)
            QFile::remove(t->finalPath());
        if (m_repository)
            m_repository->remove(id);
        m_tasks.erase(id);
        m_logins.remove(id);
        emit taskRemoved(id);
        rebalanceBandwidth();
        schedule();
        emit statsChanged();
        return;
    }

    if (!error.isError()) {
        changeState(*t, TaskState::Completed);
        t->completedAt = QDateTime::currentDateTime();
        t->errorMessage.clear();
        if (t->checksumStatus == ChecksumStatus::Mismatch) {
            t->errorMessage = QStringLiteral("SHA-256 mismatch: the file does not match the checksum you supplied and should not be trusted.");
            emit notification(QStringLiteral("Checksum mismatch"), t->fileName, true);
        } else {
            emit notification(QStringLiteral("Download complete"), t->fileName, false);
        }
        if (!outcome.note.isEmpty())
            emit notification(t->fileName, outcome.note, false);
    } else if (reason == StopReason::Pause) {
        changeState(*t, TaskState::Paused);
        t->errorMessage.clear();
    } else if (reason == StopReason::Requeue) {
        changeState(*t, TaskState::Queued);
    } else if (reason == StopReason::Cancel) {
        changeState(*t, TaskState::Cancelled);
        deletePartialFiles(*t);
        t->downloadedBytes = 0;
        t->segments.clear();
        t->errorMessage.clear();
    } else if (error.kind == ErrorKind::Cancelled) {
        changeState(*t, TaskState::Paused);
    } else {
        const RetryPolicy policy = retryPolicy();
        QString message = error.message;
        if (!outcome.note.isEmpty())
            message += QLatin1Char(' ') + outcome.note;

        if (policy.shouldRetry(error, t->retryCount)) {
            const int delay = policy.delayForAttempt(t->retryCount, error.retryAfterSeconds);
            ++t->retryCount;
            changeState(*t, TaskState::Queued);
            t->startRequested = true;
            t->retryAt = QDateTime::currentDateTime().addSecs(delay);
            t->errorMessage = QStringLiteral("%1 Retrying in %2 s (attempt %3 of %4).")
                                  .arg(message).arg(delay).arg(t->retryCount).arg(policy.maxRetries);
        } else {
            changeState(*t, TaskState::Failed);
            t->errorMessage = message;
            emit notification(QStringLiteral("Download failed"), t->fileName + QStringLiteral(": ") + error.message, true);
        }
    }

    persist(*t);
    emit taskUpdated(id);
    rebalanceBandwidth();
    schedule();
    emit statsChanged();
}

void DownloadManager::onSettingsChanged(const AppSettings &)
{
    rebalanceBandwidth();
    schedule();
}

void DownloadManager::tick()
{
    // Save running progress about once a second instead of on every packet.
    if (m_repository) {
        for (qint64 id : std::as_const(m_dirtyProgress)) {
            if (const DownloadTask *t = task(id))
                m_repository->updateProgress(*t);
        }
    }
    m_dirtyProgress.clear();

    // Speeds decay to zero when no data arrives.
    for (auto it = m_active.begin(); it != m_active.end(); ++it) {
        if (DownloadTask *t = find(it.key())) {
            it->meter.addSample(t->downloadedBytes, QDateTime::currentMSecsSinceEpoch());
            t->speed = it->meter.bytesPerSecond();
        }
    }

    schedule(); // starts automatic retries whose delay has passed
    emit statsChanged();
}

void DownloadManager::shutdown()
{
    m_tick.stop();
    if (m_active.isEmpty())
        return;

    for (auto &transfer : m_active) {
        transfer.reason = StopReason::Shutdown;
        transfer.control->stop.store(true);
    }
    for (auto it = m_active.begin(); it != m_active.end(); ++it) {
        it->thread->quit();
        it->thread->wait(15000);
        if (DownloadTask *t = find(it.key())) {
            changeState(*t, TaskState::Paused);
            t->interrupted = true;
            t->speed = 0.0;
            QVector<DownloadTask> single { *t };
            applyStartupRecovery(single); // read the real progress from disk
            t->downloadedBytes = single.first().downloadedBytes;
            t->segments = single.first().segments;
            t->errorMessage = QStringLiteral("Paused because Haul was closed.");
            persist(*t);
        }
    }
    m_active.clear();
}

} // namespace haul
