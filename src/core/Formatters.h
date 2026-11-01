#pragma once

#include <QString>
#include <QtGlobal>

namespace haul {

// "0 B", "1.5 KB", "12.3 MB" ... (1 KB = 1024 bytes). Negative input means "unknown".
QString formatBytes(qint64 bytes);

// "1.2 MB/s"; returns an empty string for a zero or negative speed.
QString formatSpeed(double bytesPerSecond);

// "45s", "3m 05s", "2h 10m". Returns "—" when the ETA is unknown (negative).
QString formatDuration(qint64 seconds);

// Percentage 0..100, or -1 when the total size is unknown.
int progressPercent(qint64 downloaded, qint64 total);

// Remaining seconds, or -1 when it cannot be estimated honestly
// (unknown size, no measurable speed, or nothing left).
qint64 estimateRemainingSeconds(qint64 downloaded, qint64 total, double bytesPerSecond);

} // namespace haul
