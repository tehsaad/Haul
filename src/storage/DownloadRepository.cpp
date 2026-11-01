#include "DownloadRepository.h"

#include "core/SegmentPlan.h"

#include <QFileInfo>
#include <QHash>

namespace haul {

namespace {

void bindTimestamp(Statement &statement, int index, const QDateTime &time)
{
    if (time.isValid())
        statement.bind(index, time.toMSecsSinceEpoch());
    else
        statement.bindNull(index);
}

QDateTime readTimestamp(const Statement &statement, int column)
{
    if (statement.isNull(column))
        return QDateTime();
    return QDateTime::fromMSecsSinceEpoch(statement.int64(column));
}

} // namespace

DownloadRepository::DownloadRepository(Database &db)
    : m_db(db)
{
}

bool DownloadRepository::insert(DownloadTask &task)
{
    Statement s(m_db.handle(),
                "INSERT INTO downloads (url, directory, file_name, status, priority, queue_position, total_bytes,"
                " downloaded, etag, last_modified, accept_ranges, retry_count, error, interrupted, expected_sha256,"
                " checksum_status, created_at, started_at, completed_at, auto_name)"
                " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!s.isValid())
        return false;
    s.bind(1, task.url);
    s.bind(2, task.directory);
    s.bind(3, task.fileName);
    s.bind(4, stateKey(task.state));
    s.bind(5, static_cast<int>(task.priority));
    s.bind(6, task.queuePosition);
    s.bind(7, task.totalBytes);
    s.bind(8, task.downloadedBytes);
    s.bind(9, task.etag);
    s.bind(10, task.lastModified);
    s.bind(11, task.acceptRanges ? 1 : 0);
    s.bind(12, task.retryCount);
    s.bind(13, task.errorMessage);
    s.bind(14, task.interrupted ? 1 : 0);
    s.bind(15, task.expectedSha256);
    s.bind(16, static_cast<int>(task.checksumStatus));
    bindTimestamp(s, 17, task.createdAt);
    bindTimestamp(s, 18, task.startedAt);
    bindTimestamp(s, 19, task.completedAt);
    s.bind(20, task.autoName ? 1 : 0);
    if (!s.execute())
        return false;
    task.id = m_db.lastInsertId();
    return writeSegments(task);
}

bool DownloadRepository::update(const DownloadTask &task)
{
    if (!m_db.begin())
        return false;

    Statement s(m_db.handle(),
                "UPDATE downloads SET url=?, directory=?, file_name=?, status=?, priority=?, queue_position=?,"
                " total_bytes=?, downloaded=?, etag=?, last_modified=?, accept_ranges=?, retry_count=?, error=?,"
                " interrupted=?, expected_sha256=?, checksum_status=?, created_at=?, started_at=?, completed_at=?,"
                " auto_name=? WHERE id=?");
    s.bind(1, task.url);
    s.bind(2, task.directory);
    s.bind(3, task.fileName);
    s.bind(4, stateKey(task.state));
    s.bind(5, static_cast<int>(task.priority));
    s.bind(6, task.queuePosition);
    s.bind(7, task.totalBytes);
    s.bind(8, task.downloadedBytes);
    s.bind(9, task.etag);
    s.bind(10, task.lastModified);
    s.bind(11, task.acceptRanges ? 1 : 0);
    s.bind(12, task.retryCount);
    s.bind(13, task.errorMessage);
    s.bind(14, task.interrupted ? 1 : 0);
    s.bind(15, task.expectedSha256);
    s.bind(16, static_cast<int>(task.checksumStatus));
    bindTimestamp(s, 17, task.createdAt);
    bindTimestamp(s, 18, task.startedAt);
    bindTimestamp(s, 19, task.completedAt);
    s.bind(20, task.autoName ? 1 : 0);
    s.bind(21, task.id);

    if (!s.isValid() || !s.execute() || !writeSegments(task) || !m_db.commit()) {
        m_db.rollback();
        return false;
    }
    return true;
}

bool DownloadRepository::updateProgress(const DownloadTask &task)
{
    if (!m_db.begin())
        return false;
    Statement s(m_db.handle(), "UPDATE downloads SET downloaded=?, total_bytes=? WHERE id=?");
    s.bind(1, task.downloadedBytes);
    s.bind(2, task.totalBytes);
    s.bind(3, task.id);
    bool ok = s.execute();
    for (const SegmentInfo &segment : task.segments) {
        Statement seg(m_db.handle(), "UPDATE segments SET downloaded=? WHERE download_id=? AND idx=?");
        seg.bind(1, segment.downloaded);
        seg.bind(2, task.id);
        seg.bind(3, segment.index);
        ok = ok && seg.execute();
    }
    if (!ok || !m_db.commit()) {
        m_db.rollback();
        return false;
    }
    return true;
}

bool DownloadRepository::writeSegments(const DownloadTask &task)
{
    Statement clear(m_db.handle(), "DELETE FROM segments WHERE download_id=?");
    clear.bind(1, task.id);
    if (!clear.execute())
        return false;
    for (const SegmentInfo &segment : task.segments) {
        Statement s(m_db.handle(),
                    "INSERT INTO segments (download_id, idx, start_byte, end_byte, downloaded) VALUES (?,?,?,?,?)");
        s.bind(1, task.id);
        s.bind(2, segment.index);
        s.bind(3, segment.start);
        s.bind(4, segment.end);
        s.bind(5, segment.downloaded);
        if (!s.execute())
            return false;
    }
    return true;
}

bool DownloadRepository::remove(qint64 id)
{
    Statement s(m_db.handle(), "DELETE FROM downloads WHERE id=?");
    s.bind(1, id);
    return s.execute();
}

QVector<DownloadTask> DownloadRepository::loadAll()
{
    QVector<DownloadTask> tasks;
    QHash<qint64, int> indexById;

    Statement s(m_db.handle(),
                "SELECT id, url, directory, file_name, status, priority, queue_position, total_bytes, downloaded,"
                " etag, last_modified, accept_ranges, retry_count, error, interrupted, expected_sha256,"
                " checksum_status, created_at, started_at, completed_at, auto_name FROM downloads ORDER BY id");
    while (s.next()) {
        DownloadTask task;
        task.id = s.int64(0);
        task.url = s.text(1);
        task.directory = s.text(2);
        task.fileName = s.text(3);
        task.state = stateFromKey(s.text(4));
        task.priority = static_cast<Priority>(static_cast<int>(qBound(qint64(0), s.int64(5), qint64(2))));
        task.queuePosition = static_cast<int>(s.int64(6));
        task.totalBytes = s.int64(7);
        task.downloadedBytes = s.int64(8);
        task.etag = s.text(9);
        task.lastModified = s.text(10);
        task.acceptRanges = s.int64(11) != 0;
        task.retryCount = static_cast<int>(s.int64(12));
        task.errorMessage = s.text(13);
        task.interrupted = s.int64(14) != 0;
        task.expectedSha256 = s.text(15);
        task.checksumStatus = static_cast<ChecksumStatus>(static_cast<int>(qBound(qint64(0), s.int64(16), qint64(2))));
        task.createdAt = readTimestamp(s, 17);
        task.startedAt = readTimestamp(s, 18);
        task.completedAt = readTimestamp(s, 19);
        task.autoName = s.int64(20) != 0;
        indexById.insert(task.id, tasks.size());
        tasks.append(task);
    }

    Statement seg(m_db.handle(),
                  "SELECT download_id, idx, start_byte, end_byte, downloaded FROM segments ORDER BY download_id, idx");
    while (seg.next()) {
        const auto it = indexById.constFind(seg.int64(0));
        if (it == indexById.constEnd())
            continue;
        SegmentInfo info;
        info.index = static_cast<int>(seg.int64(1));
        info.start = seg.int64(2);
        info.end = seg.int64(3);
        info.downloaded = seg.int64(4);
        tasks[*it].segments.append(info);
    }
    return tasks;
}

int applyStartupRecovery(QVector<DownloadTask> &tasks)
{
    int interrupted = 0;
    for (DownloadTask &task : tasks) {
        if (task.state == TaskState::Downloading) {
            task.state = TaskState::Paused;
            task.interrupted = true;
            task.errorMessage = QStringLiteral("Interrupted when Haul last closed. It will be revalidated with the server before resuming.");
            ++interrupted;
        }
        if (task.state == TaskState::Completed)
            continue;

        // The database may lag behind the disk (progress is saved about once a second),
        // so the files are the source of truth.
        if (!task.segments.isEmpty()) {
            for (SegmentInfo &segment : task.segments) {
                const qint64 size = QFileInfo(task.segmentPath(segment.index)).size();
                segment.downloaded = qBound<qint64>(0, size, segment.length());
            }
            task.downloadedBytes = segmentsDownloaded(task.segments);
        } else {
            task.downloadedBytes = QFileInfo(task.partPath()).size();
        }
    }
    return interrupted;
}

} // namespace haul
