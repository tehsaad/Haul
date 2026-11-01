#include "Schedule.h"

namespace haul {

ScheduleAction evaluateSchedule(const ScheduleConfig &config, const QDateTime &now,
                                ScheduleState &state, bool queueRunning)
{
    if (!config.enabled || !config.startTime.isValid())
        return ScheduleAction::None;

    const QTime t = now.time();
    const QDate today = now.date();
    const bool hasStop = config.stopEnabled && config.stopTime.isValid() && config.stopTime != config.startTime;

    // Which day's window (if any) are we inside right now?
    bool inWindow = false;
    QDate windowDate;
    if (!hasStop) {
        inWindow = t >= config.startTime;
        windowDate = today;
    } else if (config.startTime < config.stopTime) {
        inWindow = t >= config.startTime && t < config.stopTime;
        windowDate = today;
    } else if (t >= config.startTime) {
        inWindow = true;
        windowDate = today;
    } else if (t < config.stopTime) {
        inWindow = true; // the window started yesterday evening
        windowDate = today.addDays(-1);
    }

    if (inWindow && state.lastStartWindow != windowDate) {
        state.lastStartWindow = windowDate;
        return queueRunning ? ScheduleAction::None : ScheduleAction::StartQueue;
    }

    if (hasStop && !inWindow && t >= config.stopTime && state.lastStopDate != today) {
        state.lastStopDate = today;
        return queueRunning ? ScheduleAction::StopQueue : ScheduleAction::None;
    }
    return ScheduleAction::None;
}

} // namespace haul
