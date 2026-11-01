#pragma once

#include <QDate>
#include <QDateTime>
#include <QTime>

namespace haul {

struct ScheduleConfig {
    bool enabled = false;
    QTime startTime = QTime(2, 0);
    bool stopEnabled = false;
    QTime stopTime = QTime(8, 0);
};

// Remembers which daily windows were already acted on, so a start never fires twice.
struct ScheduleState {
    QDate lastStartWindow;
    QDate lastStopDate;
};

enum class ScheduleAction { None, StartQueue, StopQueue };

// Decides what the scheduler should do at `now` (local time).
//
// Missed-start policy: if Haul is opened after the start time but still inside the
// window (before the stop time, or any time that day when no stop time is set), the queue
// starts immediately. If the window has already ended, nothing happens until the next day.
// Windows may cross midnight (e.g. 23:00 → 06:00).
ScheduleAction evaluateSchedule(const ScheduleConfig &config, const QDateTime &now,
                                ScheduleState &state, bool queueRunning);

} // namespace haul
