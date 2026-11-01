#pragma once

#include <QString>

namespace haul {

// Every download is always in exactly one of these states.
enum class TaskState {
    Queued,
    Downloading,
    Paused,
    Completed,
    Failed,
    Cancelled
};

// Returns true when moving from `from` to `to` is allowed.
// Completed is terminal: a finished file is never re-downloaded in place.
bool canTransition(TaskState from, TaskState to);

// Stable identifiers used for the database ("queued", "downloading", ...).
QString stateKey(TaskState state);
TaskState stateFromKey(const QString &key, bool *ok = nullptr);

// Human readable label for the UI ("Queued", "Downloading", ...).
QString stateLabel(TaskState state);

bool isActiveState(TaskState state);
bool isFinishedState(TaskState state);

} // namespace haul
