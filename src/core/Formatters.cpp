#include "Formatters.h"

#include <cmath>

namespace haul {

QString formatBytes(qint64 bytes)
{
    if (bytes < 0)
        return QStringLiteral("Unknown");

    const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(bytes);
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10.0 ? 2 : 1).arg(units[unit]);
}

QString formatSpeed(double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0)
        return QString();
    return formatBytes(static_cast<qint64>(bytesPerSecond)) + QStringLiteral("/s");
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 0)
        return QStringLiteral("—");
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
    if (seconds < 86400)
        return QStringLiteral("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1d %2h").arg(seconds / 86400).arg((seconds % 86400) / 3600);
}

int progressPercent(qint64 downloaded, qint64 total)
{
    if (total <= 0)
        return total == 0 ? 100 : -1;
    if (downloaded >= total)
        return 100;
    if (downloaded <= 0)
        return 0;
    return static_cast<int>((downloaded * 100) / total);
}

qint64 estimateRemainingSeconds(qint64 downloaded, qint64 total, double bytesPerSecond)
{
    if (total <= 0 || downloaded >= total)
        return -1;
    if (bytesPerSecond < 1.0)
        return -1;
    const double remaining = static_cast<double>(total - downloaded);
    return static_cast<qint64>(std::ceil(remaining / bytesPerSecond));
}

} // namespace haul
