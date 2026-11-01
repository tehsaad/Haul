// Network tests against the deterministic local server (tests/server/test_server.py).
// They cover probing, full and resumed downloads, servers that ignore ranges, wrong
// Content-Range answers, changing resources, errors, cancellation and segmented downloads.

#include "TestSupport.h"

#include "core/SegmentPlan.h"
#include "network/DownloadWorker.h"
#include "network/HttpDownloader.h"
#include "network/HttpProbe.h"
#include "network/SegmentDownloader.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <thread>
#include <algorithm>

using namespace haul;
using namespace testsupport;

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(data);
}

SingleRequest singleRequest(const QString &url, const QString &path)
{
    SingleRequest r;
    r.url = QUrl(url);
    r.filePath = path;
    r.options.connectTimeoutSeconds = 5;
    r.options.stallTimeoutSeconds = 10;
    return r;
}

} // namespace

class TestNetwork : public QObject
{
    Q_OBJECT

    TestServer server;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<haul::WorkerOutcome>();
        qRegisterMetaType<QVector<haul::SegmentInfo>>();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        QVERIFY2(server.start(), "could not start tests/server/test_server.py");
    }

    void cleanupTestCase() { curl_global_cleanup(); }

    // ------------------------------------------------------------ probe

    void probeDetectsRangeSupport()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/file/5000")), NetworkOptions());
        QVERIFY(r.ok);
        QCOMPARE(r.httpStatus, 206);
        QVERIFY(r.rangeSupported);
        QCOMPARE(r.totalSize, qint64(5000));
        QCOMPARE(r.etag, QString("\"fixed-5000\""));
        QCOMPARE(r.suggestedName, QString("5000"));
    }

    void probeServerIgnoringRanges()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/file/300000?ranges=0")), NetworkOptions());
        QVERIFY(r.ok);
        QCOMPARE(r.httpStatus, 200);
        QVERIFY(!r.rangeSupported);
        QCOMPARE(r.totalSize, qint64(300000));
    }

    void probeUnknownLength()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/file/1000?ranges=0&nolength=1")), NetworkOptions());
        QVERIFY(r.ok);
        QCOMPARE(r.totalSize, qint64(-1));
    }

    void probeHttpError()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/missing")), NetworkOptions());
        QVERIFY(!r.ok);
        QCOMPARE(r.error.kind, ErrorKind::Permanent);
        QCOMPARE(r.error.httpStatus, 404);
    }

    void probeSanitizesServerFileName()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/file/10?name=..%2F..%2Fevil.sh")), NetworkOptions());
        QVERIFY(r.ok);
        QCOMPARE(r.suggestedName, QString("evil.sh"));
    }

    void probeFollowsHttpRedirect()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/redirect?to=/file/1234")), NetworkOptions());
        QVERIFY(r.ok);
        QCOMPARE(r.totalSize, qint64(1234));
        QVERIFY(r.effectiveUrl.endsWith("/file/1234"));
    }

    void redirectToFileSchemeIsRefused()
    {
        const ProbeResult r = probeUrl(QUrl(server.url("/redirect-file")), NetworkOptions());
        QVERIFY(!r.ok);
        QCOMPARE(r.error.kind, ErrorKind::Permanent);
    }

    void probeCanBeCancelled()
    {
        std::atomic_bool cancel { true };
        const ProbeResult r = probeUrl(QUrl(server.url("/file/100000000?ranges=0&rate=1000")), NetworkOptions(), &cancel);
        QVERIFY(!r.ok);
        QCOMPARE(r.error.kind, ErrorKind::Cancelled);
    }

    // ------------------------------------------------------------ single connection

    void successfulDownload()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.part");
        std::atomic_bool cancel { false };
        const TransferResult r = downloadSingle(singleRequest(server.url("/file/300000"), path), cancel, nullptr);
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QCOMPARE(r.totalBytes, qint64(300000));
        QCOMPARE(readFile(path), expectedContent(300000));
    }

    void unknownContentLength()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.part");
        std::atomic_bool cancel { false };
        const TransferResult r = downloadSingle(singleRequest(server.url("/file/70000?nolength=1"), path), cancel, nullptr);
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QCOMPARE(r.totalBytes, qint64(-1));
        QCOMPARE(readFile(path), expectedContent(70000));
    }

    void httpErrorsAreClassified()
    {
        QTemporaryDir dir;
        std::atomic_bool cancel { false };
        TransferResult r = downloadSingle(singleRequest(server.url("/file/10?status=404"), dir.filePath("x")), cancel, nullptr);
        QCOMPARE(r.error.kind, ErrorKind::Permanent);
        QCOMPARE(r.error.httpStatus, 404);
        QCOMPARE(r.fileSize, qint64(0)); // the error page is not saved as the file

        r = downloadSingle(singleRequest(server.url("/file/10?status=503&retryafter=7"), dir.filePath("y")), cancel, nullptr);
        QCOMPARE(r.error.kind, ErrorKind::Transient);
        QCOMPARE(r.error.retryAfterSeconds, 7);

        r = downloadSingle(singleRequest(server.url("/file/10?status=403"), dir.filePath("z")), cancel, nullptr);
        QCOMPARE(r.error.kind, ErrorKind::Permanent);
    }

    void connectionRefused()
    {
        QTemporaryDir dir;
        std::atomic_bool cancel { false };
        // Port 9 (discard) is essentially never open on a test machine.
        const TransferResult r = downloadSingle(singleRequest("http://127.0.0.1:9/x", dir.filePath("x")), cancel, nullptr);
        QCOMPARE(r.error.kind, ErrorKind::Transient);
    }

    void interruptedDownloadIsTransient()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.part");
        std::atomic_bool cancel { false };
        const TransferResult r = downloadSingle(singleRequest(server.url("/file/100000?drop=30000"), path), cancel, nullptr);
        QCOMPARE(r.error.kind, ErrorKind::Transient);
        QCOMPARE(r.fileSize, qint64(30000));
        QCOMPARE(readFile(path), expectedContent(30000)); // what arrived is correct and kept
    }

    void resumeWithRanges()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.part");
        const QByteArray full = expectedContent(300000);
        writeFile(path, full.left(100000));

        SingleRequest req = singleRequest(server.url("/file/300000"), path);
        req.expectedTotal = 300000;
        req.etag = "\"fixed-300000\"";
        std::atomic_bool cancel { false };
        qint64 firstReport = -1;
        const TransferResult r = downloadSingle(req, cancel, [&](qint64 done, qint64) {
            if (firstReport < 0)
                firstReport = done;
        });
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QVERIFY(!r.restartedFromZero);
        QVERIFY(firstReport >= 100000); // continued, did not start over
        QCOMPARE(readFile(path), full);
    }

    void resumeWhenServerIgnoresRanges()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.part");
        const QByteArray full = expectedContent(200000);
        writeFile(path, QByteArray(50000, 'Z')); // garbage that must not survive

        SingleRequest req = singleRequest(server.url("/file/200000?ranges=0"), path);
        req.expectedTotal = 200000;
        std::atomic_bool cancel { false };
        const TransferResult r = downloadSingle(req, cancel, nullptr);
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QVERIFY(r.restartedFromZero);
        QCOMPARE(readFile(path), full); // not appended to the old bytes
    }

    void resumeAfterResourceChanged()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("m.part");
        std::atomic_bool cancel { false };

        const ProbeResult before = probeUrl(QUrl(server.url("/mutable/200000")), NetworkOptions());
        TransferResult r = downloadSingle(singleRequest(server.url("/mutable/200000?drop=60000"), path), cancel, nullptr);
        QCOMPARE(r.fileSize, qint64(60000));

        QVERIFY(probeUrl(QUrl(server.url("/bump")), NetworkOptions()).ok);
        const ProbeResult after = probeUrl(QUrl(server.url("/mutable/200000")), NetworkOptions());
        QVERIFY(before.etag != after.etag);

        // Resuming with the old validator: the server's If-Range check fails, so it sends
        // the whole new file with 200 and Haul must start over instead of mixing versions.
        SingleRequest req = singleRequest(server.url("/mutable/200000"), path);
        req.expectedTotal = 200000;
        req.etag = before.etag;
        r = downloadSingle(req, cancel, nullptr);
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QVERIFY(r.restartedFromZero);
        const QByteArray data = readFile(path);
        QCOMPARE(data.size(), 200000);
        QCOMPARE(data, expectedContent(200000, after.etag.mid(2, after.etag.size() - 3).toInt()));
    }

    void wrongContentRangeIsRejected()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("a.part");
        writeFile(path, expectedContent(1000));
        SingleRequest req = singleRequest(server.url("/file/5000?badrange=1"), path);
        req.expectedTotal = 5000;
        std::atomic_bool cancel { false };
        const TransferResult r = downloadSingle(req, cancel, nullptr);
        QCOMPARE(r.error.kind, ErrorKind::Transient);
        QVERIFY(r.discardPartial);
        QCOMPARE(QFileInfo(path).size(), qint64(1000)); // nothing wrong was appended
    }

    void unwritableDestination()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("ro");
        const QString roDir = dir.filePath("ro");
        QFile::setPermissions(roDir, QFile::ReadOwner | QFile::ExeOwner);
        std::atomic_bool cancel { false };
        const TransferResult r = downloadSingle(singleRequest(server.url("/file/100"), roDir + "/x.part"), cancel, nullptr);
        QFile::setPermissions(roDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        if (QFileInfo(roDir).isWritable() && r.error.kind == ErrorKind::None)
            QSKIP("running as root: permissions are not enforced");
        QCOMPARE(r.error.kind, ErrorKind::Permanent);
    }

    void cancellationIsPrompt()
    {
        QTemporaryDir dir;
        std::atomic_bool cancel { false };
        TransferResult r;
        QElapsedTimer timer;
        timer.start();
        std::thread worker([&] {
            r = downloadSingle(singleRequest(server.url("/file/5000000?rate=100000"), dir.filePath("a")), cancel, nullptr);
        });
        QThread::msleep(400);
        cancel = true;
        worker.join();
        QCOMPARE(r.error.kind, ErrorKind::Cancelled);
        QVERIFY2(timer.elapsed() < 3000, "cancellation took too long");
    }

    void slowDownloadReportsProgress()
    {
        QTemporaryDir dir;
        std::atomic_bool cancel { false };
        QVector<qint64> reports;
        const TransferResult r = downloadSingle(singleRequest(server.url("/file/200000?rate=200000"), dir.filePath("a")),
                                                cancel, [&](qint64 done, qint64 total) {
                                                    reports.append(done);
                                                    QCOMPARE(total, qint64(200000));
                                                });
        QVERIFY(!r.error.isError());
        QVERIFY(reports.size() >= 3);
        QVERIFY(std::is_sorted(reports.begin(), reports.end()));
        QCOMPARE(reports.last(), qint64(200000));
    }

    void bandwidthLimitIsApplied()
    {
        QTemporaryDir dir;
        std::atomic_bool cancel { false };
        RateLimiter limiter;
        limiter.setLimit(100 * 1024);
        SingleRequest req = singleRequest(server.url("/file/250000"), dir.filePath("a"));
        req.limiter = &limiter;
        QElapsedTimer timer;
        timer.start();
        const TransferResult r = downloadSingle(req, cancel, nullptr);
        QVERIFY(!r.error.isError());
        QVERIFY2(timer.elapsed() >= 1500, qPrintable(QString("took only %1 ms").arg(timer.elapsed())));
    }

    // ------------------------------------------------------------ segmented

    void segmentedDownloadAssemblesCorrectly()
    {
        QTemporaryDir dir;
        const qint64 size = 5 * 1024 * 1024 + 123;
        SegmentedRequest req;
        req.url = QUrl(server.url(QString("/file/%1").arg(size)));
        req.totalBytes = size;
        req.segments = planSegments(size, 4);
        req.segmentPath = [&](int i) { return dir.filePath(QString("seg%1").arg(i)); };
        req.etag = QString("\"fixed-%1\"").arg(size);
        std::atomic_bool cancel { false };

        const SegmentedResult r = downloadSegments(req, cancel, nullptr);
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QCOMPARE(segmentsDownloaded(r.segments), size);

        QString error;
        QVERIFY2(assembleSegments(r.segments, req.segmentPath, dir.filePath("out"), size, cancel, &error), qPrintable(error));
        QCOMPARE(sha256Hex(readFile(dir.filePath("out"))), sha256Hex(expectedContent(size)));
        QVERIFY(!QFile::exists(req.segmentPath(0))); // cleaned up
    }

    void segmentedResumeFromPartialSegments()
    {
        QTemporaryDir dir;
        const qint64 size = 4 * 1024 * 1024;
        const QByteArray full = expectedContent(size);
        SegmentedRequest req;
        req.url = QUrl(server.url(QString("/file/%1").arg(size)));
        req.totalBytes = size;
        req.segments = planSegments(size, 4);
        req.segmentPath = [&](int i) { return dir.filePath(QString("seg%1").arg(i)); };
        for (const SegmentInfo &s : req.segments) // half of every segment is already on disk
            writeFile(req.segmentPath(s.index), full.mid(static_cast<int>(s.start), static_cast<int>(s.length() / 2)));

        std::atomic_bool cancel { false };
        const SegmentedResult r = downloadSegments(req, cancel, nullptr);
        QVERIFY2(!r.error.isError(), qPrintable(r.error.message));
        QString error;
        QVERIFY(assembleSegments(r.segments, req.segmentPath, dir.filePath("out"), size, cancel, &error));
        QCOMPARE(readFile(dir.filePath("out")), full);
    }

    void segmentedDetectsIgnoredRanges()
    {
        QTemporaryDir dir;
        const qint64 size = 4 * 1024 * 1024;
        SegmentedRequest req;
        req.url = QUrl(server.url(QString("/file/%1?ranges=0").arg(size)));
        req.totalBytes = size;
        req.segments = planSegments(size, 4);
        req.segmentPath = [&](int i) { return dir.filePath(QString("seg%1").arg(i)); };
        std::atomic_bool cancel { false };
        const SegmentedResult r = downloadSegments(req, cancel, nullptr);
        QVERIFY(r.rangeUnsupported);
    }

    void segmentedRejectsWrongRanges()
    {
        QTemporaryDir dir;
        const qint64 size = 4 * 1024 * 1024;
        SegmentedRequest req;
        req.url = QUrl(server.url(QString("/file/%1?badrange=1").arg(size)));
        req.totalBytes = size;
        req.segments = planSegments(size, 4);
        req.segmentPath = [&](int i) { return dir.filePath(QString("seg%1").arg(i)); };
        std::atomic_bool cancel { false };
        const SegmentedResult r = downloadSegments(req, cancel, nullptr);
        QVERIFY(!r.rangeUnsupported);
        QCOMPARE(r.error.kind, ErrorKind::Transient);
        QCOMPARE(segmentsDownloaded(r.segments), qint64(0)); // no mismatched byte was written
    }

    void assemblyRefusesIncompleteSegments()
    {
        QTemporaryDir dir;
        QVector<SegmentInfo> segments = planSegments(2000, 2);
        auto path = [&](int i) { return dir.filePath(QString("seg%1").arg(i)); };
        writeFile(path(0), QByteArray(1000, 'a'));
        writeFile(path(1), QByteArray(999, 'b')); // one byte short
        std::atomic_bool cancel { false };
        QString error;
        QVERIFY(!assembleSegments(segments, path, dir.filePath("out"), 2000, cancel, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(dir.filePath("out")));
    }

    // ------------------------------------------------------------ worker end to end

    void workerVerifiesChecksum()
    {
        QTemporaryDir dir;
        DownloadTask task;
        task.id = 1;
        task.url = server.url("/file/3000000");
        task.directory = dir.path();
        task.fileName = "data.bin";
        task.expectedSha256 = sha256Hex(expectedContent(3000000));

        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 4, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        worker.run();
        QCOMPARE(finished.count(), 1);
        const WorkerOutcome outcome = finished.first().first().value<WorkerOutcome>();
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QCOMPARE(outcome.task.checksumStatus, ChecksumStatus::Verified);
        QVERIFY(QFile::exists(dir.filePath("data.bin")));
        QVERIFY(!QFile::exists(dir.filePath("data.bin.haul-part")));
    }

    void workerReportsChecksumMismatch()
    {
        QTemporaryDir dir;
        DownloadTask task;
        task.id = 2;
        task.url = server.url("/file/1000");
        task.directory = dir.path();
        task.fileName = "data.bin";
        task.expectedSha256 = QString(64, QChar('0'));
        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 1, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        worker.run();
        const WorkerOutcome outcome = finished.first().first().value<WorkerOutcome>();
        QCOMPARE(outcome.task.checksumStatus, ChecksumStatus::Mismatch);
    }

    void workerFallsBackToSingleConnection()
    {
        QTemporaryDir dir;
        const qint64 size = 3 * 1024 * 1024;
        DownloadTask task;
        task.id = 3;
        task.url = server.url(QString("/file/%1?ranges=0").arg(size));
        task.directory = dir.path();
        task.fileName = "f.bin";
        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 8, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        QSignalSpy meta(&worker, &DownloadWorker::metadataReady);
        worker.run();
        const WorkerOutcome outcome = finished.first().first().value<WorkerOutcome>();
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QCOMPARE(meta.first().at(4).toInt(), 1); // one connection: ranges are not supported
        QCOMPARE(readFile(dir.filePath("f.bin")), expectedContent(size));
    }

    void workerNeverOverwritesExistingFile()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("taken.bin"), "user data");
        DownloadTask task;
        task.id = 4;
        task.url = server.url("/file/500");
        task.directory = dir.path();
        task.fileName = "taken.bin"; // a file with this name appeared after the task was added
        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 1, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        worker.run();
        const WorkerOutcome outcome = finished.first().first().value<WorkerOutcome>();
        QVERIFY(!outcome.error.isError());
        QCOMPARE(outcome.task.fileName, QString("taken (1).bin"));
        QCOMPARE(readFile(dir.filePath("taken.bin")), QByteArray("user data"));
        QCOMPARE(readFile(dir.filePath("taken (1).bin")), expectedContent(500));
    }

    void workerRestartsWhenRemoteChanged()
    {
        QTemporaryDir dir;
        DownloadTask task;
        task.id = 5;
        task.url = server.url("/mutable/150000");
        task.directory = dir.path();
        task.fileName = "m.bin";
        task.etag = "\"v-old\"";
        task.totalBytes = 150000;
        task.downloadedBytes = 1000;
        task.acceptRanges = true;
        writeFile(task.partPath(), QByteArray(1000, 'Q')); // stale bytes from an older version

        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 1, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        worker.run();
        const WorkerOutcome outcome = finished.first().first().value<WorkerOutcome>();
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QVERIFY(outcome.note.contains("changed"));
        const QByteArray data = readFile(dir.filePath("m.bin"));
        QCOMPARE(data.size(), 150000);
        QVERIFY(!data.startsWith(QByteArray(1000, 'Q')));
    }
};

QTEST_GUILESS_MAIN(TestNetwork)
#include "tst_network.moc"
