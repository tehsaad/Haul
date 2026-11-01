#include "SettingsRepository.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

namespace haul {

bool operator==(const AppSettings &a, const AppSettings &b)
{
    return a.downloadDirectory == b.downloadDirectory
        && a.maxConcurrentDownloads == b.maxConcurrentDownloads
        && a.maxConnectionsPerDownload == b.maxConnectionsPerDownload
        && a.speedLimitKiB == b.speedLimitKiB && a.autoRetry == b.autoRetry
        && a.maxRetries == b.maxRetries && a.retryDelaySeconds == b.retryDelaySeconds
        && a.startImmediately == b.startImmediately && a.theme == b.theme
        && a.confirmBeforeDelete == b.confirmBeforeDelete
        && a.startQueueOnLaunch == b.startQueueOnLaunch && a.launchAtLogin == b.launchAtLogin
        && a.schedule.enabled == b.schedule.enabled && a.schedule.startTime == b.schedule.startTime
        && a.schedule.stopEnabled == b.schedule.stopEnabled && a.schedule.stopTime == b.schedule.stopTime;
}

QStringList validateSettings(const AppSettings &s)
{
    QStringList errors;
    if (s.downloadDirectory.trimmed().isEmpty())
        errors << QStringLiteral("Choose a default download folder.");
    else if (!QDir::isAbsolutePath(s.downloadDirectory))
        errors << QStringLiteral("The download folder must be an absolute path.");
    else if (QFileInfo::exists(s.downloadDirectory) && !QFileInfo(s.downloadDirectory).isDir())
        errors << QStringLiteral("The download folder path points to a file, not a folder.");

    if (s.maxConcurrentDownloads < 1 || s.maxConcurrentDownloads > 10)
        errors << QStringLiteral("Simultaneous downloads must be between 1 and 10.");
    if (s.maxConnectionsPerDownload < 1 || s.maxConnectionsPerDownload > 16)
        errors << QStringLiteral("Connections per download must be between 1 and 16.");
    if (s.speedLimitKiB < 0 || s.speedLimitKiB > 10 * 1024 * 1024)
        errors << QStringLiteral("The bandwidth limit must be between 0 (unlimited) and 10485760 KB/s.");
    if (s.maxRetries < 0 || s.maxRetries > 10)
        errors << QStringLiteral("Automatic retries must be between 0 and 10.");
    if (s.retryDelaySeconds < 1 || s.retryDelaySeconds > 3600)
        errors << QStringLiteral("The retry delay must be between 1 and 3600 seconds.");
    if (s.theme != QLatin1String("dark") && s.theme != QLatin1String("light"))
        errors << QStringLiteral("Unknown theme.");
    if (s.schedule.enabled && !s.schedule.startTime.isValid())
        errors << QStringLiteral("Choose a valid scheduled start time.");
    if (s.schedule.enabled && s.schedule.stopEnabled && s.schedule.stopTime == s.schedule.startTime)
        errors << QStringLiteral("The scheduled stop time must differ from the start time.");
    return errors;
}

SettingsRepository::SettingsRepository(QObject *parent)
    : QObject(parent)
    , m_store(std::make_unique<QSettings>())
{
    m_current = load();
}

SettingsRepository::SettingsRepository(const QString &iniPath, QObject *parent)
    : QObject(parent)
    , m_store(std::make_unique<QSettings>(iniPath, QSettings::IniFormat))
{
    m_current = load();
}

AppSettings SettingsRepository::defaults()
{
    AppSettings s;
    QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads.isEmpty())
        downloads = QDir::homePath();
    s.downloadDirectory = QDir::cleanPath(downloads);
    return s;
}

AppSettings SettingsRepository::load() const
{
    const AppSettings d = defaults();
    QSettings &q = *m_store;
    AppSettings s;
    s.downloadDirectory = q.value("downloads/directory", d.downloadDirectory).toString();
    s.maxConcurrentDownloads = q.value("downloads/maxConcurrent", d.maxConcurrentDownloads).toInt();
    s.maxConnectionsPerDownload = q.value("downloads/maxConnections", d.maxConnectionsPerDownload).toInt();
    s.speedLimitKiB = q.value("downloads/speedLimitKiB", d.speedLimitKiB).toInt();
    s.autoRetry = q.value("retry/enabled", d.autoRetry).toBool();
    s.maxRetries = q.value("retry/maxRetries", d.maxRetries).toInt();
    s.retryDelaySeconds = q.value("retry/delaySeconds", d.retryDelaySeconds).toInt();
    s.startImmediately = q.value("behavior/startImmediately", d.startImmediately).toBool();
    s.theme = q.value("appearance/theme", d.theme).toString();
    s.confirmBeforeDelete = q.value("behavior/confirmDelete", d.confirmBeforeDelete).toBool();
    s.startQueueOnLaunch = q.value("behavior/startQueueOnLaunch", d.startQueueOnLaunch).toBool();
    s.launchAtLogin = q.value("behavior/launchAtLogin", d.launchAtLogin).toBool();
    s.schedule.enabled = q.value("schedule/enabled", d.schedule.enabled).toBool();
    s.schedule.startTime = QTime::fromString(q.value("schedule/start", d.schedule.startTime.toString("HH:mm")).toString(), "HH:mm");
    s.schedule.stopEnabled = q.value("schedule/stopEnabled", d.schedule.stopEnabled).toBool();
    s.schedule.stopTime = QTime::fromString(q.value("schedule/stop", d.schedule.stopTime.toString("HH:mm")).toString(), "HH:mm");

    // A hand-edited or corrupted file must not break the application: fall back to defaults.
    if (!validateSettings(s).isEmpty())
        return d;
    return s;
}

void SettingsRepository::write(const AppSettings &s)
{
    QSettings &q = *m_store;
    q.setValue("downloads/directory", s.downloadDirectory);
    q.setValue("downloads/maxConcurrent", s.maxConcurrentDownloads);
    q.setValue("downloads/maxConnections", s.maxConnectionsPerDownload);
    q.setValue("downloads/speedLimitKiB", s.speedLimitKiB);
    q.setValue("retry/enabled", s.autoRetry);
    q.setValue("retry/maxRetries", s.maxRetries);
    q.setValue("retry/delaySeconds", s.retryDelaySeconds);
    q.setValue("behavior/startImmediately", s.startImmediately);
    q.setValue("appearance/theme", s.theme);
    q.setValue("behavior/confirmDelete", s.confirmBeforeDelete);
    q.setValue("behavior/startQueueOnLaunch", s.startQueueOnLaunch);
    q.setValue("behavior/launchAtLogin", s.launchAtLogin);
    q.setValue("schedule/enabled", s.schedule.enabled);
    q.setValue("schedule/start", s.schedule.startTime.toString("HH:mm"));
    q.setValue("schedule/stopEnabled", s.schedule.stopEnabled);
    q.setValue("schedule/stop", s.schedule.stopTime.toString("HH:mm"));
    q.sync();
}

bool SettingsRepository::save(const AppSettings &settings, QStringList *errors)
{
    const QStringList problems = validateSettings(settings);
    if (errors)
        *errors = problems;
    if (!problems.isEmpty())
        return false;

    write(settings);
    m_current = settings;
    emit changed(m_current);
    return true;
}

void SettingsRepository::restoreDefaults()
{
    save(defaults());
}

ScheduleState SettingsRepository::scheduleState() const
{
    ScheduleState state;
    state.lastStartWindow = QDate::fromString(m_store->value("scheduleState/lastStartWindow").toString(), Qt::ISODate);
    state.lastStopDate = QDate::fromString(m_store->value("scheduleState/lastStopDate").toString(), Qt::ISODate);
    return state;
}

void SettingsRepository::setScheduleState(const ScheduleState &state)
{
    m_store->setValue("scheduleState/lastStartWindow", state.lastStartWindow.toString(Qt::ISODate));
    m_store->setValue("scheduleState/lastStopDate", state.lastStopDate.toString(Qt::ISODate));
    m_store->sync();
}

} // namespace haul
