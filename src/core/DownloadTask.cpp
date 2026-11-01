#include "DownloadTask.h"

#include "Formatters.h"

#include <QDir>

namespace haul {

QString priorityLabel(Priority priority)
{
    switch (priority) {
    case Priority::Low: return QStringLiteral("Low");
    case Priority::Normal: return QStringLiteral("Normal");
    case Priority::High: return QStringLiteral("High");
    }
    return QStringLiteral("Normal");
}

QString DownloadTask::finalPath() const
{
    return QDir(directory).filePath(fileName);
}

QString DownloadTask::partPath() const
{
    return finalPath() + QStringLiteral(".haul-part");
}

QString DownloadTask::segmentPath(int index) const
{
    return finalPath() + QStringLiteral(".haul-seg%1").arg(index);
}

QString DownloadTask::streamDirectory() const
{
    return finalPath() + QStringLiteral(".haul-stream");
}

int DownloadTask::progressPercent() const
{
    return haul::progressPercent(downloadedBytes, totalBytes);
}

} // namespace haul
