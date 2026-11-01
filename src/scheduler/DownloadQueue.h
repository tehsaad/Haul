#pragma once

#include "core/DownloadTask.h"

#include <QDateTime>
#include <QVector>

namespace haul {

// The fields of a task that matter for queue decisions.
struct QueueEntry {
    qint64 id = 0;
    TaskState state = TaskState::Queued;
    Priority priority = Priority::Normal;
    int queuePosition = 0;
    bool startRequested = false; // "Start now": does not wait for the queue to be running
    QDateTime retryAt;           // automatic retry may not start before this time
};

QueueEntry queueEntryFor(const DownloadTask &task);

// Pure queue logic, kept free of threads and networking so it is easy to test.
class DownloadQueue
{
public:
    // Waiting tasks in the order they will start: higher priority first, then by queue position (FIFO).
    static QVector<QueueEntry> ordered(const QVector<QueueEntry> &entries);

    // Which queued tasks should start now, given how many are already active.
    // Never returns more than (maxConcurrent - activeCount) ids and never an active task.
    static QVector<qint64> selectToStart(const QVector<QueueEntry> &entries, int activeCount,
                                         int maxConcurrent, bool queueRunning, const QDateTime &now);

    // Moves a waiting task one place earlier (-1) or later (+1) in the start order.
    // When the neighbour has a different priority, the moved task adopts that priority
    // so the visible order really changes. Returns false if the move is impossible.
    static bool move(QVector<QueueEntry> &entries, qint64 id, int direction);

    static int nextPosition(const QVector<QueueEntry> &entries);
};

} // namespace haul
