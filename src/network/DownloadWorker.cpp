#include "DownloadWorker.h"

#include "HttpDownloader.h"
#include "HttpProbe.h"
#include "SegmentDownloader.h"
#include "core/Checksum.h"
#include "core/FileNameUtils.h"
#include "core/SegmentPlan.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

namespace haul {

DownloadWorker::DownloadWorker(const DownloadTask &task, int maxConnections, const NetworkOptions &options,
                               std::shared_ptr<TransferControl> control)
    : m_task(task)
    , m_maxConnections(maxConnections)
    , m_options(options)
    , m_control(std::move(control))
{
}

void DownloadWorker::discardPartialData(DownloadTask &task)
{
    QFile::remove(task.partPath());
    for (const SegmentInfo &segment : task.segments)
        QFile::remove(task.segmentPath(segment.index));
    task.segments.clear();
    task.downloadedBytes = 0;
}

void DownloadWorker::run()
{
    DownloadTask task = m_task;
    WorkerOutcome outcome;
    const std::atomic_bool &stop = m_control->stop;

    auto finish = [&](const TransferError &error) {
        outcome.task = task;
        outcome.error = error;
        outcome.note = m_note;
        emit finished(outcome);
    };

    // 1. Revalidate the remote file.
    const ProbeResult probe = probeUrl(QUrl(task.url), m_options, &stop);
    if (stop.load()) {
        finish({ ErrorKind::Cancelled, QStringLiteral("Stopped by the user."), 0, -1 });
        return;
    }
    if (!probe.ok) {
        finish(probe.error);
        return;
    }

    // 2. Did the file change since we started? Compare validators and size.
    const bool hasPartial = task.downloadedBytes > 0 || QFileInfo::exists(task.partPath()) || !task.segments.isEmpty();
    bool changed = false;
    if (!task.etag.isEmpty() && !probe.etag.isEmpty())
        changed = task.etag != probe.etag;
    else if (!task.lastModified.isEmpty() && !probe.lastModified.isEmpty())
        changed = task.lastModified != probe.lastModified;
    if (task.totalBytes > 0 && probe.totalSize > 0 && task.totalBytes != probe.totalSize)
        changed = true;

    if (hasPartial && changed) {
        discardPartialData(task);
        m_note = QStringLiteral("The remote file changed since the last attempt, so the download restarted from the beginning.");
    } else if (hasPartial && !probe.rangeSupported) {
        discardPartialData(task);
        m_note = QStringLiteral("The server does not support resuming, so the download restarted from the beginning.");
    }

    // A name guessed from the URL is replaced once by the server's suggestion
    // (Content-Disposition or the final URL after redirects), before any data is written.
    if (task.autoName && !QFileInfo::exists(task.partPath()) && task.segments.isEmpty()
        && !probe.suggestedName.isEmpty() && probe.suggestedName != task.fileName) {
        task.fileName = uniqueFileName(task.directory, probe.suggestedName);
    }
    task.autoName = false;

    task.totalBytes = probe.totalSize;
    task.etag = probe.etag;
    task.lastModified = probe.lastModified;
    task.acceptRanges = probe.rangeSupported;

    // 3. Make sure there is room for the file.
    if (task.totalBytes > 0) {
        const QStorageInfo storage(task.directory);
        const qint64 needed = task.totalBytes - task.downloadedBytes;
        if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < needed) {
            finish({ ErrorKind::Permanent,
                     QStringLiteral("Not enough free disk space: %1 bytes needed.").arg(needed), 0, -1 });
            return;
        }
    }

    // 4. Choose single or segmented mode. An existing single-connection partial file is
    //    continued as it is; existing segment files are continued with their original plan.
    const qint64 partSize = QFileInfo(task.partPath()).size();
    bool segmented = task.acceptRanges && task.totalBytes > 0 && !isFtpUrl(QUrl(task.url));
    if (segmented && task.segments.isEmpty()) {
        const int count = segmentCountFor(task.totalBytes, m_maxConnections);
        segmented = count > 1 && partSize == 0;
        if (segmented) {
            // Leftover segment files from an unknown plan must never be mixed into a new one.
            for (int i = 0; i < kMaxSegments; ++i)
                QFile::remove(task.segmentPath(i));
            task.segments = planSegments(task.totalBytes, count);
        }
    } else if (segmented && !segmentsCoverExactly(task.segments, task.totalBytes)) {
        discardPartialData(task);
        task.segments = planSegments(task.totalBytes, segmentCountFor(task.totalBytes, m_maxConnections));
        segmented = task.segments.size() > 1;
    }
    if (!segmented && !task.segments.isEmpty()) {
        for (const SegmentInfo &segment : task.segments)
            QFile::remove(task.segmentPath(segment.index));
        task.segments.clear();
    }

    emit metadataReady(task.id, task.fileName, task.totalBytes, task.acceptRanges, segmented ? task.segments.size() : 1);

    TransferError error;
    if (segmented) {
        bool fallBack = false;
        error = downloadSegmented(task, &fallBack);
        if (fallBack && !stop.load()) {
            discardPartialData(task);
            m_note = QStringLiteral("The server ignored multi-connection requests; continued with a single connection.");
            error = downloadSingleConnection(task);
        }
    } else {
        error = downloadSingleConnection(task);
    }

    if (!error.isError())
        error = finalize(task);
    finish(error);
}

TransferError DownloadWorker::downloadSingleConnection(DownloadTask &task)
{
    SingleRequest request;
    request.url = QUrl(task.url);
    request.filePath = task.partPath();
    request.expectedTotal = task.totalBytes;
    request.etag = task.etag;
    request.lastModified = task.lastModified;
    request.resume = task.acceptRanges;
    request.options = m_options;
    request.limiter = &m_control->limiter;

    const qint64 id = task.id;
    const TransferResult result = downloadSingle(request, m_control->stop, [this, id](qint64 downloaded, qint64 total) {
        emit progress(id, downloaded, total, {});
    });

    task.downloadedBytes = result.fileSize;
    if (result.totalBytes >= 0)
        task.totalBytes = result.totalBytes;
    if (result.restartedFromZero)
        m_note = QStringLiteral("The server did not honour the resume request, so the download restarted from the beginning.");
    if (result.discardPartial) {
        QFile::remove(task.partPath());
        task.downloadedBytes = 0;
        task.acceptRanges = false;
    }
    return result.error;
}

TransferError DownloadWorker::downloadSegmented(DownloadTask &task, bool *fallBack)
{
    const DownloadTask snapshot = task;
    SegmentedRequest request;
    request.url = QUrl(task.url);
    request.totalBytes = task.totalBytes;
    request.segments = task.segments;
    request.segmentPath = [snapshot](int index) { return snapshot.segmentPath(index); };
    request.etag = task.etag;
    request.lastModified = task.lastModified;
    request.options = m_options;
    request.limiter = &m_control->limiter;

    const qint64 id = task.id;
    const qint64 total = task.totalBytes;
    const SegmentedResult result = downloadSegments(request, m_control->stop,
        [this, id, total](const QVector<SegmentInfo> &segments) {
            emit progress(id, segmentsDownloaded(segments), total, segments);
        });

    task.segments = result.segments;
    task.downloadedBytes = segmentsDownloaded(result.segments);
    *fallBack = result.rangeUnsupported;
    if (result.rangeUnsupported)
        return {};
    if (result.error.isError())
        return result.error;

    QString assemblyError;
    if (!assembleSegments(task.segments, request.segmentPath, task.partPath(), task.totalBytes,
                          m_control->stop, &assemblyError)) {
        if (m_control->stop.load())
            return { ErrorKind::Cancelled, assemblyError, 0, -1 };
        return { ErrorKind::Permanent, assemblyError, 0, -1 };
    }
    task.segments.clear();
    task.downloadedBytes = task.totalBytes;
    return {};
}

TransferError DownloadWorker::finalize(DownloadTask &task)
{
    const QFileInfo part(task.partPath());
    if (!part.exists())
        return { ErrorKind::Permanent, QStringLiteral("The downloaded data is missing."), 0, -1 };
    if (task.totalBytes >= 0 && part.size() != task.totalBytes)
        return { ErrorKind::Transient, QStringLiteral("The downloaded file has the wrong size."), 0, -1 };

    // Never overwrite a file that appeared while we were downloading.
    if (QFileInfo::exists(task.finalPath())) {
        const QString finalName = uniqueFileName(task.directory, task.fileName);
        const QString oldPart = task.partPath();
        task.fileName = finalName;
        if (!QFile::rename(oldPart, task.partPath()))
            return { ErrorKind::Permanent, QStringLiteral("Could not rename the downloaded file."), 0, -1 };
    }
    if (!QFile::rename(task.partPath(), task.finalPath()))
        return { ErrorKind::Permanent, QStringLiteral("Could not move the finished file into place."), 0, -1 };

    task.downloadedBytes = QFileInfo(task.finalPath()).size();
    if (task.totalBytes < 0)
        task.totalBytes = task.downloadedBytes;

    if (!task.expectedSha256.isEmpty()) {
        const QString actual = sha256OfFile(task.finalPath(), &m_control->stop);
        if (actual.isEmpty() && m_control->stop.load()) {
            task.checksumStatus = ChecksumStatus::NotChecked;
        } else {
            task.checksumStatus = actual.compare(task.expectedSha256.trimmed(), Qt::CaseInsensitive) == 0
                ? ChecksumStatus::Verified
                : ChecksumStatus::Mismatch;
        }
    }
    return {};
}

} // namespace haul
