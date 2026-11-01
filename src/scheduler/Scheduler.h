#pragma once

#include "Schedule.h"

#include <QObject>
#include <QTimer>

namespace haul {

class DownloadManager;
class SettingsRepository;

// Checks the schedule every 15 seconds (and whenever settings change) and starts or stops
// the download queue. It only runs while Haul is open; there is no background service.
class Scheduler : public QObject
{
    Q_OBJECT
public:
    Scheduler(SettingsRepository *settings, DownloadManager *manager, QObject *parent = nullptr);

    // Runs one check immediately (used at startup so a missed start is handled right away).
    void check();

    // Human-readable description of the next scheduled action, for the Queue page.
    QString describe() const;

signals:
    void acted(const QString &message);

private:
    SettingsRepository *m_settings;
    DownloadManager *m_manager;
    QTimer m_timer;
};

} // namespace haul
