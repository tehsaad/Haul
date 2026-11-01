// Unit tests for the pure core helpers: URL validation, state machine, formatting,
// file names, retry policy, speed measurement, rate limiting, segments and checksums.

#include "core/Checksum.h"
#include "core/DownloadTask.h"
#include "core/FileNameUtils.h"
#include "core/Formatters.h"
#include "core/RateLimiter.h"
#include "core/RetryPolicy.h"
#include "core/SegmentPlan.h"
#include "core/SpeedMeter.h"
#include "core/TaskState.h"
#include "core/UrlValidator.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace haul;

class TestCore : public QObject
{
    Q_OBJECT

private slots:
    void urlValidation_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<bool>("ok");
        QTest::newRow("https") << "https://example.com/file.zip" << true;
        QTest::newRow("http with port and query") << "http://127.0.0.1:8080/a/b.iso?x=1" << true;
        QTest::newRow("surrounding spaces") << "  https://example.com/x  " << true;
        QTest::newRow("empty") << "" << false;
        QTest::newRow("no scheme") << "example.com/file.zip" << false;
        QTest::newRow("ftp") << "ftp://example.com/file" << true;
        QTest::newRow("gopher") << "gopher://example.com/file" << false;
        QTest::newRow("file") << "file:///etc/passwd" << false;
        QTest::newRow("javascript") << "javascript:alert(1)" << false;
        QTest::newRow("data") << "data:text/plain,hi" << false;
        QTest::newRow("no host") << "http:///path" << false;
        QTest::newRow("credentials") << "https://user:pw@example.com/f" << false;
        QTest::newRow("inner space") << "https://exa mple.com/f" << false;
    }

    void urlValidation()
    {
        QFETCH(QString, input);
        QFETCH(bool, ok);
        const UrlCheck check = validateDownloadUrl(input);
        QCOMPARE(check.ok, ok);
        if (!ok)
            QVERIFY(!check.error.isEmpty());
    }

    void redactUrlHidesQuery()
    {
        const QString text = redactUrl(QUrl("https://example.com/file.zip?token=SECRET#frag"));
        QVERIFY(!text.contains("SECRET"));
        QVERIFY(text.startsWith("https://example.com/file.zip"));
    }

    void stateTransitions()
    {
        QVERIFY(canTransition(TaskState::Queued, TaskState::Downloading));
        QVERIFY(canTransition(TaskState::Downloading, TaskState::Paused));
        QVERIFY(canTransition(TaskState::Downloading, TaskState::Completed));
        QVERIFY(canTransition(TaskState::Paused, TaskState::Queued));
        QVERIFY(canTransition(TaskState::Failed, TaskState::Queued));
        QVERIFY(canTransition(TaskState::Cancelled, TaskState::Queued));

        QVERIFY(!canTransition(TaskState::Completed, TaskState::Downloading));
        QVERIFY(!canTransition(TaskState::Completed, TaskState::Queued));
        QVERIFY(!canTransition(TaskState::Queued, TaskState::Completed));
        QVERIFY(!canTransition(TaskState::Paused, TaskState::Completed));
        QVERIFY(!canTransition(TaskState::Cancelled, TaskState::Downloading));
        QVERIFY(!canTransition(TaskState::Downloading, TaskState::Downloading));
    }

    void stateKeysRoundTrip()
    {
        const TaskState all[] = { TaskState::Queued, TaskState::Downloading, TaskState::Paused,
                                  TaskState::Completed, TaskState::Failed, TaskState::Cancelled };
        for (TaskState s : all) {
            bool ok = false;
            QCOMPARE(stateFromKey(stateKey(s), &ok), s);
            QVERIFY(ok);
        }
        bool ok = true;
        stateFromKey("nonsense", &ok);
        QVERIFY(!ok);
    }

    void progressAndEta()
    {
        QCOMPARE(progressPercent(50, 200), 25);
        QCOMPARE(progressPercent(200, 200), 100);
        QCOMPARE(progressPercent(10, -1), -1);      // unknown size
        QCOMPARE(estimateRemainingSeconds(0, -1, 1000.0), qint64(-1)); // unknown size: no ETA
        QCOMPARE(estimateRemainingSeconds(0, 1000, 0.0), qint64(-1));  // no speed: no ETA
        QCOMPARE(estimateRemainingSeconds(500, 1000, 100.0), qint64(5));
        QCOMPARE(estimateRemainingSeconds(1000, 1000, 100.0), qint64(-1)); // finished
        QCOMPARE(formatDuration(-1), QString("—"));
        QCOMPARE(formatDuration(65), QString("1m 05s"));
    }

    void formatBytesUnits()
    {
        QCOMPARE(formatBytes(0), QString("0 B"));
        QCOMPARE(formatBytes(1023), QString("1023 B"));
        QCOMPARE(formatBytes(1536), QString("1.50 KB"));
        QCOMPARE(formatBytes(-1), QString("Unknown"));
        QVERIFY(formatSpeed(0).isEmpty());
        QCOMPARE(formatSpeed(2048), QString("2.00 KB/s"));
    }

    void sanitizeFileNames_data()
    {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << "report.pdf" << "report.pdf";
        QTest::newRow("traversal") << "../../etc/passwd" << "passwd";
        QTest::newRow("windows traversal") << "..\\..\\boot.ini" << "boot.ini";
        QTest::newRow("reserved chars") << "a<b>c:d|e?.txt" << "a_b_c_d_e_.txt";
        QTest::newRow("leading dots") << "...hidden" << "hidden";
        QTest::newRow("only dots") << ".." << "download";
        QTest::newRow("empty") << "" << "download";
        QTest::newRow("device name") << "CON.txt" << "_CON.txt";
        QTest::newRow("control chars") << QString("a\x01" "b\n.bin") << "ab.bin";
        QTest::newRow("our suffix") << "x.haul-part" << "x_haul-part";
    }

    void sanitizeFileNames()
    {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(sanitizeFileName(input), expected);
    }

    void sanitizeLimitsLength()
    {
        const QString longName = QString(500, QChar('a')) + ".zip";
        const QString safe = sanitizeFileName(longName);
        QVERIFY(safe.size() <= 200);
        QVERIFY(safe.endsWith(".zip"));
    }

    void contentDisposition()
    {
        QCOMPARE(fileNameFromContentDisposition("attachment; filename=\"report.pdf\""), QString("report.pdf"));
        QCOMPARE(fileNameFromContentDisposition("attachment; filename*=UTF-8''na%C3%AFve%20file.txt"), QString("naïve file.txt"));
        QCOMPARE(fileNameFromContentDisposition("attachment; filename=\"../../evil.sh\""), QString("evil.sh"));
        QCOMPARE(fileNameFromContentDisposition("inline"), QString());
    }

    void fileNameFromUrlUsesLastSegment()
    {
        QCOMPARE(fileNameFromUrl(QUrl("https://example.com/dir/file%20name.iso?x=1")), QString("file name.iso"));
        QCOMPARE(fileNameFromUrl(QUrl("https://example.com/")), QString("example.com"));
    }

    void uniqueNamesAvoidConflicts()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QCOMPARE(uniqueFileName(dir.path(), "a.zip"), QString("a.zip"));
        QFile f(dir.filePath("a.zip"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
        QCOMPARE(uniqueFileName(dir.path(), "a.zip"), QString("a (1).zip"));
        QCOMPARE(uniqueFileName(dir.path(), "a.zip", { "a (1).zip" }), QString("a (2).zip"));
        QCOMPARE(uniqueFileName(dir.path(), "b.tar.gz", { "b.tar.gz" }), QString("b (1).tar.gz"));
    }

    void categories()
    {
        QCOMPARE(categoryForFileName("movie.MKV"), QString("Video"));
        QCOMPARE(categoryForFileName("setup.exe"), QString("Program"));
        QCOMPARE(categoryForFileName("x.tar.gz"), QString("Archive"));
        QCOMPARE(categoryForFileName("noext"), QString("Other"));
        for (const char *video : { "a.mp4", "a.mkv", "a.webm", "a.mov", "a.avi", "a.flv", "a.ts", "a.m4v", "a.mpeg", "a.mpg", "a.3gp" })
            QCOMPARE(categoryForFileName(video), QString("Video"));
        for (const char *audio : { "a.mp3", "a.wav", "a.flac", "a.aac", "a.m4a", "a.ogg", "a.opus", "a.wma" })
            QCOMPARE(categoryForFileName(audio), QString("Audio"));
    }

    void retryPolicy()
    {
        RetryPolicy policy;
        policy.maxRetries = 3;
        policy.baseDelaySeconds = 5;
        policy.maxDelaySeconds = 60;

        const TransferError transient { ErrorKind::Transient, "timeout", 0, -1 };
        const TransferError permanent { ErrorKind::Permanent, "404", 404, -1 };
        QVERIFY(policy.shouldRetry(transient, 0));
        QVERIFY(policy.shouldRetry(transient, 2));
        QVERIFY(!policy.shouldRetry(transient, 3)); // bounded
        QVERIFY(!policy.shouldRetry(permanent, 0)); // never retry permanent errors

        QCOMPARE(policy.delayForAttempt(0), 5);
        QCOMPARE(policy.delayForAttempt(1), 10);
        QCOMPARE(policy.delayForAttempt(2), 20);
        QCOMPARE(policy.delayForAttempt(10), 60);     // capped
        QCOMPARE(policy.delayForAttempt(0, 30), 30);  // honours Retry-After
        QCOMPARE(policy.delayForAttempt(0, 9999), 60); // but caps it

        policy.enabled = false;
        QVERIFY(!policy.shouldRetry(transient, 0));
    }

    void httpStatusClassification()
    {
        QCOMPARE(classifyHttpStatus(200), ErrorKind::None);
        QCOMPARE(classifyHttpStatus(206), ErrorKind::None);
        QCOMPARE(classifyHttpStatus(404), ErrorKind::Permanent);
        QCOMPARE(classifyHttpStatus(403), ErrorKind::Permanent);
        QCOMPARE(classifyHttpStatus(401), ErrorKind::Permanent);
        QCOMPARE(classifyHttpStatus(429), ErrorKind::Transient);
        QCOMPARE(classifyHttpStatus(503), ErrorKind::Transient);
        QCOMPARE(classifyHttpStatus(500), ErrorKind::Transient);
        QCOMPARE(classifyHttpStatus(501), ErrorKind::Permanent);
    }

    void speedMeterSmooths()
    {
        SpeedMeter meter(0.5, 100);
        meter.addSample(0, 0);
        QVERIFY(!meter.hasMeasurement());
        meter.addSample(1000, 1000); // 1000 B/s
        QCOMPARE(meter.bytesPerSecond(), 1000.0);
        meter.addSample(4000, 2000); // instant 3000 -> 0.5*3000 + 0.5*1000
        QCOMPARE(meter.bytesPerSecond(), 2000.0);
        meter.addSample(4100, 2050); // too soon: ignored
        QCOMPARE(meter.bytesPerSecond(), 2000.0);
    }

    void rateLimiter()
    {
        RateLimiter limiter;
        QCOMPARE(limiter.consume(1'000'000), qint64(0)); // unlimited
        limiter.setLimit(1000);
        const qint64 wait = limiter.consume(500);
        QVERIFY(wait >= 400 && wait <= 500); // 500 bytes at 1000 B/s = ~500 ms
    }

    void segmentPlanning()
    {
        const QVector<SegmentInfo> plan = planSegments(10'000'001, 4);
        QCOMPARE(plan.size(), 4);
        QVERIFY(segmentsCoverExactly(plan, 10'000'001));
        QCOMPARE(plan.first().start, qint64(0));
        QCOMPARE(plan.last().end, qint64(10'000'000));
        for (int i = 1; i < plan.size(); ++i)
            QCOMPARE(plan[i].start, plan[i - 1].end + 1); // no gaps, no overlaps

        QCOMPARE(segmentCountFor(500 * 1024, 8), 1);     // too small to split
        QCOMPARE(segmentCountFor(3 * 1024 * 1024, 8), 3); // at least 1 MiB per segment
        QCOMPARE(segmentCountFor(100 * 1024 * 1024, 4), 4);
        QCOMPARE(segmentCountFor(-1, 8), 1);              // unknown size: one connection
        QVERIFY(planSegments(0, 4).isEmpty());

        QVector<SegmentInfo> broken = plan;
        broken[2].start += 1;
        QVERIFY(!segmentsCoverExactly(broken, 10'000'001));
    }

    void checksum()
    {
        QVERIFY(isValidSha256Hex(QString(64, QChar('a'))));
        QVERIFY(!isValidSha256Hex(QString(63, QChar('a'))));
        QVERIFY(!isValidSha256Hex(QString(64, QChar('g'))));

        QTemporaryDir dir;
        const QString path = dir.filePath("abc.txt");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("abc");
        f.close();
        QCOMPARE(sha256OfFile(path), QString("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
        QVERIFY(sha256OfFile(dir.filePath("missing")).isEmpty());
    }

    void taskPaths()
    {
        DownloadTask t;
        t.directory = "/tmp/x";
        t.fileName = "a.bin";
        QCOMPARE(t.finalPath(), QString("/tmp/x/a.bin"));
        QCOMPARE(t.partPath(), QString("/tmp/x/a.bin.haul-part"));
        QCOMPARE(t.segmentPath(2), QString("/tmp/x/a.bin.haul-seg2"));
        t.totalBytes = -1;
        QCOMPARE(t.progressPercent(), -1);
    }
};

QTEST_GUILESS_MAIN(TestCore)
#include "tst_core.moc"
