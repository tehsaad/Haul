#include "DownloadQueue.h"

#include <algorithm>
#include <utility>

namespace haul {

QueueEntry queueEntryFor(const DownloadTask &task)
{
    QueueEntry entry;
    entry.id = task.id;
    entry.state = task.state;
    entry.priority = task.priority;
    entry.queuePosition = task.queuePosition;
    entry.startRequested = task.startRequested;
    entry.retryAt = task.retryAt;
    return entry;
}

static bool comesBefore(const QueueEntry &a, const QueueEntry &b)
{
    if (a.priority != b.priority)
        return static_cast<int>(a.priority) > static_cast<int>(b.priority);
    if (a.queuePosition != b.queuePosition)
        return a.queuePosition < b.queuePosition;
    return a.id < b.id;
}

QVector<QueueEntry> DownloadQueue::ordered(const QVector<QueueEntry> &entries)
{
    QVector<QueueEntry> waiting;
    for (const QueueEntry &entry : entries) {
        if (entry.state == TaskState::Queued)
            waiting.append(entry);
    }
    std::stable_sort(waiting.begin(), waiting.end(), comesBefore);
    return waiting;
}

QVector<qint64> DownloadQueue::selectToStart(const QVector<QueueEntry> &entries, int activeCount,
                                             int maxConcurrent, bool queueRunning, const QDateTime &now)
{
    QVector<qint64> chosen;
    int freeSlots = maxConcurrent - activeCount;
    if (freeSlots <= 0)
        return chosen;

    for (const QueueEntry &entry : ordered(entries)) {
        if (freeSlots == 0)
            break;
        if (!queueRunning && !entry.startRequested)
            continue;
        if (entry.retryAt.isValid() && entry.retryAt > now)
            continue;
        chosen.append(entry.id);
        --freeSlots;
    }
    return chosen;
}

bool DownloadQueue::move(QVector<QueueEntry> &entries, qint64 id, int direction)
{
    if (direction != -1 && direction != 1)
        return false;

    const QVector<QueueEntry> order = ordered(entries);
    int index = -1;
    for (int i = 0; i < order.size(); ++i) {
        if (order[i].id == id)
            index = i;
    }
    const int neighbourIndex = index + direction;
    if (index < 0 || neighbourIndex < 0 || neighbourIndex >= order.size())
        return false;

    // Swap the two in the visible order, adopt the neighbour's priority, then renumber
    // every waiting task 1, 2, 3 ... so the new order is exactly what the user sees.
    QVector<QueueEntry> newOrder = order;
    std::swap(newOrder[index], newOrder[neighbourIndex]);
    newOrder[neighbourIndex].priority = order[neighbourIndex].priority;

    for (int i = 0; i < newOrder.size(); ++i) {
        for (QueueEntry &entry : entries) {
            if (entry.id == newOrder[i].id) {
                entry.queuePosition = i + 1;
                entry.priority = newOrder[i].priority;
            }
        }
    }
    return true;
}

int DownloadQueue::nextPosition(const QVector<QueueEntry> &entries)
{
    int highest = 0;
    for (const QueueEntry &entry : entries)
        highest = std::max(highest, entry.queuePosition);
    return highest + 1;
}

} // namespace haul
