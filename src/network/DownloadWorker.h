#pragma once

#include "CurlEasy.h"
#include "core/DownloadTask.h"
#include "core/RateLimiter.h"

#include <QObject>

#include <atomic>
#include <memory>

namespace haul {

// Shared between the GUI thread (which can stop the transfer or change its speed limit)
// and the worker thread (which obeys them).
struct TransferControl {
    std::atomic_bool stop { false };
    RateLimiter limiter;
};

struct WorkerOutcome {
    DownloadTask task;   // updated sizes, validators, segments and (possibly) file name
    TransferError error; // kind None means the file is complete and in its final place
    QString note;        // extra information, e.g. "restarted because the file changed"
};

// Performs one download attempt for one task, start to finish, on a background thread:
//   1. probe the URL again (revalidate size, range support and validators),
//   2. discard partial data if the remote file changed,
//   3. download with several connections when possible, otherwise one,
//   4. assemble, rename the ".haul-part" file to its final name, and
//   5. verify the SHA-256 if the user supplied one.
// It never touches widgets; it only reports through signals.
class DownloadWorker : public QObject
{
    Q_OBJECT
public:
    DownloadWorker(const DownloadTask &task, int maxConnections, const NetworkOptions &options,
                   std::shared_ptr<TransferControl> control);

public slots:
    void run();

signals:
    void metadataReady(qint64 taskId, const QString &fileName, qint64 totalBytes, bool acceptRanges, int connections);
    void progress(qint64 taskId, qint64 downloaded, qint64 total, QVector<haul::SegmentInfo> segments);
    void finished(haul::WorkerOutcome outcome);

private:
    TransferError downloadSingleConnection(DownloadTask &task);
    TransferError downloadSegmented(DownloadTask &task, bool *fallBack);
    TransferError finalize(DownloadTask &task);
    void discardPartialData(DownloadTask &task);

    DownloadTask m_task;
    int m_maxConnections;
    NetworkOptions m_options;
    std::shared_ptr<TransferControl> m_control;
    QString m_note;
};

} // namespace haul

Q_DECLARE_METATYPE(haul::WorkerOutcome)
Q_DECLARE_METATYPE(haul::SegmentInfo)
