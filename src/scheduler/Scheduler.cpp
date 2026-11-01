#include "Scheduler.h"

#include "app/DownloadManager.h"
#include "storage/SettingsRepository.h"

namespace haul {

Scheduler::Scheduler(SettingsRepository *settings, DownloadManager *manager, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_manager(manager)
{
    connect(&m_timer, &QTimer::timeout, this, &Scheduler::check);
    connect(m_settings, &SettingsRepository::changed, this, &Scheduler::check);
    m_timer.start(15000);
}

void Scheduler::check()
{
    ScheduleState state = m_settings->scheduleState();
    const ScheduleState before = state;
    const ScheduleAction action = evaluateSchedule(m_settings->current().schedule, QDateTime::currentDateTime(),
                                                   state, m_manager->isQueueRunning());
    if (state.lastStartWindow != before.lastStartWindow || state.lastStopDate != before.lastStopDate)
        m_settings->setScheduleState(state);

    if (action == ScheduleAction::StartQueue) {
        m_manager->startQueue();
        emit acted(QStringLiteral("Scheduled start: the download queue was started."));
    } else if (action == ScheduleAction::StopQueue) {
        m_manager->stopQueue();
        emit acted(QStringLiteral("Scheduled stop: the download queue was stopped."));
    }
}

QString Scheduler::describe() const
{
    const ScheduleConfig &config = m_settings->current().schedule;
    if (!config.enabled)
        return QStringLiteral("Scheduling is off.");
    QString text = QStringLiteral("Starts the queue daily at %1").arg(config.startTime.toString(QStringLiteral("HH:mm")));
    if (config.stopEnabled)
        text += QStringLiteral(" and stops it at %1").arg(config.stopTime.toString(QStringLiteral("HH:mm")));
    return text + QLatin1Char('.');
}

} // namespace haul
