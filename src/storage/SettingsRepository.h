#pragma once

#include "scheduler/Schedule.h"

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

namespace haul {

struct AppSettings {
    QString downloadDirectory;
    int maxConcurrentDownloads = 3;  // 1..10
    int maxConnectionsPerDownload = 4; // 1..16
    int speedLimitKiB = 0;           // 0 = unlimited, else KiB/s for all downloads together
    bool autoRetry = true;
    int maxRetries = 3;              // 0..10
    int retryDelaySeconds = 5;       // 1..3600 (doubles after every failed attempt)
    bool startImmediately = true;    // default button in the Add Download dialog
    QString theme = QStringLiteral("dark"); // "dark" or "light"
    bool confirmBeforeDelete = true;
    bool startQueueOnLaunch = false;
    bool launchAtLogin = false;
    ScheduleConfig schedule;
};

bool operator==(const AppSettings &a, const AppSettings &b);
inline bool operator!=(const AppSettings &a, const AppSettings &b) { return !(a == b); }

// Returns a list of problems; empty means the settings are valid.
QStringList validateSettings(const AppSettings &settings);

// Loads, validates and saves settings with QSettings
// (an INI file in ~/.config/Haul on Linux, the registry on Windows).
class SettingsRepository : public QObject
{
    Q_OBJECT
public:
    // Uses the platform default location.
    explicit SettingsRepository(QObject *parent = nullptr);
    // Uses a specific INI file (used by the tests).
    explicit SettingsRepository(const QString &iniPath, QObject *parent = nullptr);

    static AppSettings defaults();

    const AppSettings &current() const { return m_current; }

    // Validates first; nothing is saved if validation fails.
    bool save(const AppSettings &settings, QStringList *errors = nullptr);
    void restoreDefaults();

    ScheduleState scheduleState() const;
    void setScheduleState(const ScheduleState &state);

signals:
    void changed(const haul::AppSettings &settings);

private:
    AppSettings load() const;
    void write(const AppSettings &settings);

    std::unique_ptr<QSettings> m_store;
    AppSettings m_current;
};

} // namespace haul
