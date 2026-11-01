#include "TaskState.h"

namespace haul {

bool canTransition(TaskState from, TaskState to)
{
    if (from == to)
        return false;

    switch (from) {
    case TaskState::Queued:
        return to == TaskState::Downloading || to == TaskState::Paused
            || to == TaskState::Cancelled;
    case TaskState::Downloading:
        // Downloading -> Queued happens when the queue is stopped or a retry is scheduled.
        return to == TaskState::Paused || to == TaskState::Completed || to == TaskState::Failed
            || to == TaskState::Cancelled || to == TaskState::Queued;
    case TaskState::Paused:
        return to == TaskState::Queued || to == TaskState::Downloading
            || to == TaskState::Cancelled;
    case TaskState::Failed:
        return to == TaskState::Queued || to == TaskState::Downloading
            || to == TaskState::Cancelled;
    case TaskState::Cancelled:
        // A cancelled task can be retried, which starts again from zero.
        return to == TaskState::Queued;
    case TaskState::Completed:
        return false;
    }
    return false;
}

QString stateKey(TaskState state)
{
    switch (state) {
    case TaskState::Queued: return QStringLiteral("queued");
    case TaskState::Downloading: return QStringLiteral("downloading");
    case TaskState::Paused: return QStringLiteral("paused");
    case TaskState::Completed: return QStringLiteral("completed");
    case TaskState::Failed: return QStringLiteral("failed");
    case TaskState::Cancelled: return QStringLiteral("cancelled");
    }
    return QStringLiteral("queued");
}

TaskState stateFromKey(const QString &key, bool *ok)
{
    const TaskState all[] = { TaskState::Queued, TaskState::Downloading, TaskState::Paused,
                              TaskState::Completed, TaskState::Failed, TaskState::Cancelled };
    for (TaskState s : all) {
        if (stateKey(s) == key) {
            if (ok)
                *ok = true;
            return s;
        }
    }
    if (ok)
        *ok = false;
    return TaskState::Queued;
}

QString stateLabel(TaskState state)
{
    switch (state) {
    case TaskState::Queued: return QStringLiteral("Queued");
    case TaskState::Downloading: return QStringLiteral("Downloading");
    case TaskState::Paused: return QStringLiteral("Paused");
    case TaskState::Completed: return QStringLiteral("Completed");
    case TaskState::Failed: return QStringLiteral("Failed");
    case TaskState::Cancelled: return QStringLiteral("Cancelled");
    }
    return QString();
}

bool isActiveState(TaskState state)
{
    return state == TaskState::Downloading;
}

bool isFinishedState(TaskState state)
{
    return state == TaskState::Completed || state == TaskState::Failed
        || state == TaskState::Cancelled;
}

} // namespace haul
