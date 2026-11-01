// Tests for streamed video: HLS and DASH parsing, refusing encrypted, live and malformed streams,
// downloading pieces in order, retries, resuming, and (when FFmpeg is installed) real merging.

#include "TestSupport.h"

#include "app/DownloadManager.h"
#include "media/DashParser.h"
#include "media/HlsParser.h"
#include "media/MediaMerger.h"
#include "media/StreamDownloader.h"
#include "media/StreamResolver.h"
#include "network/DownloadWorker.h"
#include "storage/Database.h"
#include "storage/DownloadRepository.h"
#include "storage/SettingsRepository.h"

#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>

#include <thread>

using namespace haul;
using namespace testsupport;

namespace {

// What the test server sends for pieces [first, first + count) of `size` bytes each.
QByteArray expectedPieces(int first, int count, qint64 size)
{
    QByteArray out;
    for (int i = 0; i < count; ++i)
        out += expectedContent(size, first + i);
    return out;
}

WorkerOutcome runStreamWorker(const QString &url, const QString &directory, StreamKind kind, const QString &variant = QString(), int connections = 4)
{
    DownloadTask task;
    task.id = 1;
    task.url = url;
    task.directory = directory;
    task.fileName = "video.mp4";
    task.streamKind = kind;
    task.streamVariant = variant;
    auto control = std::make_shared<TransferControl>();
    DownloadWorker worker(task, connections, NetworkOptions(), control);
    QSignalSpy finished(&worker, &DownloadWorker::finished);
    worker.run();
    return finished.first().first().value<WorkerOutcome>();
}

bool runProcess(const QString &program, const QStringList &arguments, QByteArray *output = nullptr)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    if (!process.waitForFinished(120000) || process.exitCode() != 0) {
        qWarning().noquote() << program << arguments.join(' ') << process.readAll().right(800);
        return false;
    }
    if (output)
        *output = process.readAll();
    return true;
}

} // namespace

class TestMedia : public QObject
{
    Q_OBJECT

    TestServer server;
    QTemporaryDir fixtures;
    QString ffmpeg;
    QString ffprobe;

private slots:
    void initTestCase()
    {
        qRegisterMetaType<haul::WorkerOutcome>();
        qRegisterMetaType<QVector<haul::SegmentInfo>>();
        curl_global_init(CURL_GLOBAL_DEFAULT);
        ffmpeg = QStandardPaths::findExecutable("ffmpeg");
        ffprobe = QStandardPaths::findExecutable("ffprobe");
        QVERIFY(server.start(fixtures.path()));
    }

    void cleanupTestCase() { curl_global_cleanup(); }

    // ---------------------------------------------------------------- HLS parser

    void hlsMasterListsQualitiesBestFirst()
    {
        const QByteArray text =
            "#EXTM3U\n"
            "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"English, main\",DEFAULT=YES,URI=\"a/en.m3u8\"\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=640x360,CODECS=\"avc1.4d401e,mp4a.40.2\",AUDIO=\"aud\"\n"
            "360/index.m3u8\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,AUDIO=\"aud\"\n"
            "https://cdn.example.com/1080/index.m3u8\n"
            "#EXT-X-I-FRAME-STREAM-INF:BANDWIDTH=1000,URI=\"iframes.m3u8\"\n";
        HlsMaster master;
        QString error;
        QVERIFY(parseHlsMaster(text, QUrl("https://example.com/v/master.m3u8?t=1"), &master, &error));
        QCOMPARE(master.variants.size(), 2); // the I-frame list is not a playable quality
        QCOMPARE(master.variants[0].height, 1080);
        QCOMPARE(master.variants[0].label(), QString("1080p"));
        QCOMPARE(master.variants[0].url, QString("https://cdn.example.com/1080/index.m3u8"));
        QCOMPARE(master.variants[1].url, QString("https://example.com/v/360/index.m3u8"));
        QVERIFY(master.variants[1].separateAudio);
        QCOMPARE(master.audioGroups.value("aud"), QString("https://example.com/v/a/en.m3u8"));
    }

    void hlsMediaHandlesMapAndByteRange()
    {
        const QByteArray text =
            "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n"
            "#EXTINF:4.0,\n#EXT-X-BYTERANGE:1000@0\nmain.mp4\n"
            "#EXTINF:4.0,\n#EXT-X-BYTERANGE:500\nmain.mp4\n#EXT-X-ENDLIST\n";
        HlsMedia media;
        QString error;
        QVERIFY(parseHlsMedia(text, QUrl("https://e.com/x/p.m3u8"), &media, &error));
        QVERIFY(!media.live);
        QVERIFY(!media.encrypted);
        QVERIFY(media.fragmentedMp4);
        QCOMPARE(media.segments.size(), 3);
        QVERIFY(media.segments[0].isInit);
        QCOMPARE(media.segments[1].rangeStart, qint64(0));
        QCOMPARE(media.segments[2].rangeStart, qint64(1000)); // continues after the previous range
        QCOMPARE(media.segments[2].rangeLength, qint64(500));
    }

    void hlsSpotsEncryptionAndLive_data()
    {
        QTest::addColumn<QByteArray>("text");
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<bool>("live");
        QTest::newRow("aes-128") << QByteArray("#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"k\"\n#EXTINF:2,\na.ts\n#EXT-X-ENDLIST") << true << false;
        QTest::newRow("sample-aes starts late") << QByteArray("#EXTM3U\n#EXTINF:2,\na.ts\n#EXT-X-KEY:METHOD=SAMPLE-AES,URI=\"k\"\n#EXTINF:2,\nb.ts\n#EXT-X-ENDLIST") << true << false;
        QTest::newRow("method none") << QByteArray("#EXTM3U\n#EXT-X-KEY:METHOD=NONE\n#EXTINF:2,\na.ts\n#EXT-X-ENDLIST") << false << false;
        QTest::newRow("no endlist") << QByteArray("#EXTM3U\n#EXTINF:2,\na.ts\n") << false << true;
    }

    void hlsSpotsEncryptionAndLive()
    {
        QFETCH(QByteArray, text);
        QFETCH(bool, encrypted);
        QFETCH(bool, live);
        HlsMedia media;
        QString error;
        QVERIFY(parseHlsMedia(text, QUrl("https://e.com/p.m3u8"), &media, &error));
        QCOMPARE(media.encrypted, encrypted);
        QCOMPARE(media.live, live);
    }

    void hlsRejectsMalformed()
    {
        HlsMedia media;
        HlsMaster master;
        QString error;
        QVERIFY(!parseHlsMedia("<html>nope</html>", QUrl("https://e.com/"), &media, &error));
        QVERIFY(!parseHlsMedia("#EXTM3U\n#EXT-X-ENDLIST\n", QUrl("https://e.com/"), &media, &error));
        QVERIFY(!parseHlsMaster("#EXTM3U\n", QUrl("https://e.com/"), &master, &error));
        QVERIFY(!error.isEmpty());
    }

    // ---------------------------------------------------------------- DASH parser

    void dashDurations()
    {
        QCOMPARE(parseIsoDuration("PT1H2M3.5S"), 3723.5);
        QCOMPARE(parseIsoDuration("PT30S"), 30.0);
        QCOMPARE(parseIsoDuration("P1DT1S"), 86401.0);
        QVERIFY(parseIsoDuration("nonsense") < 0);
    }

    void dashTemplateWithNumber()
    {
        const QByteArray text = R"(<MPD type="static" mediaPresentationDuration="PT10S"><BaseURL>media/</BaseURL><Period>
          <AdaptationSet contentType="video" mimeType="video/mp4">
            <SegmentTemplate timescale="1000" duration="4000" startNumber="5" initialization="$RepresentationID$/init.mp4" media="$RepresentationID$/s$Number%03d$_$Bandwidth$.m4s"/>
            <Representation id="hd" bandwidth="2000" width="1280" height="720" codecs="avc1"/>
          </AdaptationSet></Period></MPD>)";
        DashManifest m;
        QString error;
        QVERIFY2(parseDashManifest(text, QUrl("https://e.com/a/m.mpd"), &m, &error), qPrintable(error));
        const DashRepresentation &r = m.representations.first();
        QCOMPARE(r.segments.size(), 4); // init + ceil(10 / 4)
        QVERIFY(r.segments[0].isInit);
        QCOMPARE(r.segments[0].url, QString("https://e.com/a/media/hd/init.mp4"));
        QCOMPARE(r.segments[1].url, QString("https://e.com/a/media/hd/s005_2000.m4s"));
        QCOMPARE(r.segments[3].url, QString("https://e.com/a/media/hd/s007_2000.m4s"));
    }

    void dashTimelineWithRepeat()
    {
        const QByteArray text = R"(<MPD type="static" mediaPresentationDuration="PT10S"><Period>
          <AdaptationSet contentType="audio" mimeType="audio/mp4">
            <SegmentTemplate timescale="10" media="a/$Time$.m4s"><SegmentTimeline><S t="100" d="20" r="1"/><S d="30"/><S d="10" r="-1"/></SegmentTimeline></SegmentTemplate>
            <Representation id="a" bandwidth="1"/>
          </AdaptationSet></Period></MPD>)";
        DashManifest m;
        QString error;
        QVERIFY2(parseDashManifest(text, QUrl("https://e.com/m.mpd"), &m, &error), qPrintable(error));
        const QVector<MediaSegment> &s = m.representations.first().segments;
        QVERIFY(s.size() >= 3);
        QCOMPARE(s[0].url, QString("https://e.com/a/100.m4s"));
        QCOMPARE(s[1].url, QString("https://e.com/a/120.m4s"));
        QCOMPARE(s[2].url, QString("https://e.com/a/140.m4s"));
        QCOMPARE(s[2].duration, 3.0);
    }

    void dashSegmentListAndSingleFile()
    {
        const QByteArray list = R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period><AdaptationSet contentType="video" mimeType="video/webm">
            <Representation id="v" bandwidth="1" height="480"><SegmentList duration="4" timescale="1"><Initialization sourceURL="v.webm" range="0-99"/>
              <SegmentURL media="v.webm" mediaRange="100-199"/><SegmentURL media="v.webm" mediaRange="200-299"/></SegmentList></Representation>
            </AdaptationSet></Period></MPD>)";
        DashManifest m;
        QString error;
        QVERIFY2(parseDashManifest(list, QUrl("https://e.com/m.mpd"), &m, &error), qPrintable(error));
        const DashRepresentation &r = m.representations.first();
        QCOMPARE(r.extension, QString("webm"));
        QCOMPARE(r.segments.size(), 3);
        QCOMPARE(r.segments[1].rangeStart, qint64(100));
        QCOMPARE(r.segments[1].rangeLength, qint64(100));

        const QByteArray single = R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period><AdaptationSet contentType="video" mimeType="video/mp4">
            <Representation id="v" bandwidth="1"><BaseURL>whole.mp4</BaseURL><SegmentBase indexRange="0-9"/></Representation></AdaptationSet></Period></MPD>)";
        DashManifest whole;
        QVERIFY2(parseDashManifest(single, QUrl("https://e.com/m.mpd"), &whole, &error), qPrintable(error));
        QCOMPARE(whole.representations.first().segments.size(), 1);
        QCOMPARE(whole.representations.first().segments[0].url, QString("https://e.com/whole.mp4"));
    }

    void dashMarksProtectionAndLive()
    {
        const QByteArray encrypted = R"(<MPD type="static" mediaPresentationDuration="PT8S"><Period><AdaptationSet contentType="video" mimeType="video/mp4">
            <ContentProtection schemeIdUri="urn:mpeg:dash:mp4protection:2011" value="cenc"/>
            <Representation id="v" bandwidth="1"><BaseURL>w.mp4</BaseURL></Representation></AdaptationSet></Period></MPD>)";
        DashManifest m;
        QString error;
        QVERIFY(parseDashManifest(encrypted, QUrl("https://e.com/m.mpd"), &m, &error));
        QVERIFY(m.representations.first().encrypted);

        DashManifest live;
        QVERIFY(parseDashManifest("<MPD type=\"dynamic\"></MPD>", QUrl("https://e.com/m.mpd"), &live, &error));
        QVERIFY(live.live);
    }

    void dashRejectsMalformed()
    {
        DashManifest m;
        QString error;
        QVERIFY(!parseDashManifest("<MPD><Period>", QUrl("https://e.com/"), &m, &error));
        QVERIFY(!parseDashManifest("<notdash/>", QUrl("https://e.com/"), &m, &error));
        QVERIFY(!parseDashManifest("<MPD type=\"static\"/>", QUrl("https://e.com/"), &m, &error));
        // An entity that would read a local file must not be expanded.
        DashManifest evil;
        const bool parsed = parseDashManifest(R"(<?xml version="1.0"?><!DOCTYPE MPD [<!ENTITY x SYSTEM "file:///etc/passwd">]><MPD type="static" mediaPresentationDuration="PT1S"><Period><AdaptationSet contentType="video" mimeType="video/mp4"><Representation id="v" bandwidth="1"><BaseURL>&x;</BaseURL></Representation></AdaptationSet></Period></MPD>)",
                                              QUrl("https://e.com/"), &evil, &error);
        if (parsed)
            QVERIFY(!evil.representations.first().segments.first().url.contains("root:"));
    }

    // ---------------------------------------------------------------- resolver

    void localNetworkHosts()
    {
        QVERIFY(isLocalNetworkHost("localhost"));
        QVERIFY(isLocalNetworkHost("127.0.0.1"));
        QVERIFY(isLocalNetworkHost("192.168.1.10"));
        QVERIFY(isLocalNetworkHost("10.1.2.3"));
        QVERIFY(isLocalNetworkHost("172.20.0.1"));
        QVERIFY(isLocalNetworkHost("169.254.169.254"));
        QVERIFY(!isLocalNetworkHost("8.8.8.8"));
        QVERIFY(!isLocalNetworkHost("example.com"));
    }

    void inspectFindsQualitiesAndSize()
    {
        const StreamInfo info = inspectStream(QUrl(server.url("/hls/master.m3u8")), NetworkOptions());
        QVERIFY2(info.ok, qPrintable(info.error.message));
        QCOMPARE(info.kind, StreamKind::Hls);
        QCOMPARE(info.variants.size(), 2);
        QCOMPARE(info.variants[0].label(), QString("720p"));
        QCOMPARE(info.duration, 12.0);
        QCOMPARE(info.variants[0].estimatedBytes, qint64(2500000) * 12 / 8);
        QCOMPARE(info.childUrls.size(), 2);

        const StreamInfo dash = inspectStream(QUrl(server.url("/dash/manifest.mpd")), NetworkOptions());
        QVERIFY2(dash.ok, qPrintable(dash.error.message));
        QCOMPARE(dash.kind, StreamKind::Dash);
        QCOMPARE(dash.variants.size(), 2);
        QCOMPARE(dash.variants[0].height, 720);
        QVERIFY(dash.variants[0].separateAudio);
    }

    void inspectRefuses_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("expected");
        QTest::newRow("hls key") << "/hls/high.m3u8?enc=key" << "encrypted";
        QTest::newRow("hls key late") << "/hls/high.m3u8?enc=late" << "encrypted";
        QTest::newRow("hls session key") << "/hls/master.m3u8?enc=session" << "encrypted";
        QTest::newRow("hls variant key") << "/hls/master.m3u8?enc=key" << "encrypted";
        QTest::newRow("hls live") << "/hls/high.m3u8?live=1" << "live";
        QTest::newRow("dash drm") << "/dash/manifest.mpd?enc=1" << "encrypted";
        QTest::newRow("dash live") << "/dash/manifest.mpd?live=1" << "live";
        QTest::newRow("dash periods") << "/dash/manifest.mpd?periods=2" << "periods";
        QTest::newRow("not a playlist") << "/hls/garbage.m3u8" << "valid video manifest";
        QTest::newRow("access denied") << "/file/10.m3u8?status=403" << "404"; // no such file route
    }

    void inspectRefuses()
    {
        QFETCH(QString, path);
        QFETCH(QString, expected);
        const StreamInfo info = inspectStream(QUrl(server.url(path)), NetworkOptions());
        QVERIFY(!info.ok);
        QCOMPARE(info.error.kind, ErrorKind::Permanent);
        QVERIFY2(info.error.message.contains(expected), qPrintable(info.error.message));
    }

    void ordinaryFilesAreNotStreams()
    {
        const StreamInfo info = inspectStream(QUrl(server.url("/file/5000")), NetworkOptions());
        QVERIFY(info.ok);
        QCOMPARE(info.kind, StreamKind::File);
    }

    void planChoosesRequestedQuality()
    {
        const QString url = server.url("/hls/master.m3u8");
        StreamPlan best = resolveStreamPlan(QUrl(url), StreamKind::Hls, QString(), NetworkOptions());
        QVERIFY(best.ok);
        QCOMPARE(best.label, QString("720p"));
        QVERIFY(best.tracks[0].segments[0].url.contains("/2000.ts"));

        StreamPlan low = resolveStreamPlan(QUrl(url), StreamKind::Hls, "360p-800000", NetworkOptions());
        QVERIFY(low.ok);
        QVERIFY(low.tracks[0].segments[0].url.contains("/1000.ts"));

        StreamPlan gone = resolveStreamPlan(QUrl(url), StreamKind::Hls, "4320p-1", NetworkOptions());
        QVERIFY(!gone.ok);
        QCOMPARE(gone.error.kind, ErrorKind::Permanent);

        StreamPlan audio = resolveStreamPlan(QUrl(url + "?audio=1"), StreamKind::Hls, QString(), NetworkOptions());
        QVERIFY(audio.ok);
        QCOMPARE(audio.tracks.size(), 2);
        QCOMPARE(audio.tracks[0].role, StreamTrack::Video);
        QCOMPARE(audio.tracks[1].role, StreamTrack::Audio);
    }

    void planRefusesEncryptedVariant()
    {
        StreamPlan plan = resolveStreamPlan(QUrl(server.url("/hls/master.m3u8?enc=key")), StreamKind::Hls, QString(), NetworkOptions());
        QVERIFY(!plan.ok);
        QVERIFY(plan.error.message.contains("encrypted"));
    }

    // ---------------------------------------------------------------- downloading

    void hlsDownloadKeepsPiecesInOrder()
    {
        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/hls/master.m3u8?count=12&size=30000"), dir.path(), StreamKind::Hls, "360p-800000");
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        // No FFmpeg means .ts; with FFmpeg the fake bytes can't be converted, so it also stays .ts.
        QCOMPARE(outcome.task.fileName, QString("video.ts"));
        QCOMPARE(sha256Hex(readFile(dir.filePath("video.ts"))), sha256Hex(expectedPieces(1000, 12, 30000)));
        QVERIFY(!QFile::exists(dir.filePath("video.mp4.haul-stream"))); // temporary pieces are cleaned up
        QCOMPARE(outcome.task.streamLabel, QString("360p"));
    }

    void singleConnectionAlsoWorks()
    {
        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/hls/single.m3u8?count=5"), dir.path(), StreamKind::Hls, QString(), 1);
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QCOMPARE(readFile(dir.filePath("video.ts")), expectedPieces(4000, 5, 20000));
    }

    void fragmentedMp4KeepsInitFirst()
    {
        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/hls/single.m3u8?count=3&fmp4=1"), dir.path(), StreamKind::Hls);
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QCOMPARE(outcome.task.fileName, QString("video.mp4"));
        const QByteArray data = readFile(dir.filePath("video.mp4"));
        QVERIFY(data.startsWith(expectedContent(20000, 4999).left(100))); // the init segment comes first
    }

    void failedPieceIsRetried()
    {
        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/hls/single.m3u8?count=4&flaky=2&key=retry"), dir.path(), StreamKind::Hls);
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QCOMPARE(readFile(dir.filePath("video.ts")), expectedPieces(4000, 4, 20000));
    }

    void restrictedPieceStopsTheDownload()
    {
        QTemporaryDir dir;
        WorkerOutcome outcome = runStreamWorker(server.url("/hls/single.m3u8?count=4&deny=2"), dir.path(), StreamKind::Hls);
        QCOMPARE(outcome.error.kind, ErrorKind::Permanent);
        QCOMPARE(outcome.error.httpStatus, 403);
        QVERIFY(outcome.error.message.contains("expired"));

        QTemporaryDir dir2;
        outcome = runStreamWorker(server.url("/hls/single.m3u8?count=4&html=1"), dir2.path(), StreamKind::Hls);
        QCOMPARE(outcome.error.kind, ErrorKind::Permanent); // a login page must not end up in the video
        QVERIFY(outcome.error.message.contains("web page"));
    }

    void encryptedInitDataIsRefused()
    {
        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/dash/manifest.mpd?encv=1"), dir.path(), StreamKind::Dash);
        QCOMPARE(outcome.error.kind, ErrorKind::Permanent);
        QVERIFY2(outcome.error.message.contains("encrypted"), qPrintable(outcome.error.message));
    }

    void resumeSkipsFinishedPieces()
    {
        QTemporaryDir dir;
        DownloadTask task;
        task.id = 2;
        task.url = server.url("/hls/single.m3u8?count=20&size=20000&slow=0.15");
        task.directory = dir.path();
        task.fileName = "video.mp4";
        task.streamKind = StreamKind::Hls;

        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 2, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        QTimer::singleShot(0, &worker, [&] {});
        std::thread stopper([&] {
            QThread::msleep(900);
            control->stop = true;
        });
        worker.run();
        stopper.join();
        WorkerOutcome outcome = finished.first().first().value<WorkerOutcome>();
        QCOMPARE(outcome.error.kind, ErrorKind::Cancelled);
        const qint64 kept = streamBytesOnDisk(task.streamDirectory());
        QVERIFY2(kept > 0 && kept < 400000, qPrintable(QString::number(kept)));
        QVERIFY(QDir(task.streamDirectory()).entryList({ "*.part" }).isEmpty()); // half-written pieces are not kept

        // Mark a finished piece: if it is still marked afterwards, it was not fetched a second time.
        QFile first(streamPiecePath(task.streamDirectory(), 0, 0));
        QVERIFY(first.exists());
        QVERIFY(first.open(QIODevice::WriteOnly | QIODevice::Truncate));
        first.write(QByteArray(20000, 'X'));
        first.close();

        auto control2 = std::make_shared<TransferControl>();
        DownloadWorker again(task, 2, NetworkOptions(), control2);
        QSignalSpy finished2(&again, &DownloadWorker::finished);
        again.run();
        outcome = finished2.first().first().value<WorkerOutcome>();
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        const QByteArray data = readFile(dir.filePath("video.ts"));
        QCOMPARE(data.left(20000), QByteArray(20000, 'X'));
        QCOMPARE(data.mid(20000), expectedPieces(4001, 19, 20000));
    }

    void cancelStopsPromptly()
    {
        QTemporaryDir dir;
        DownloadTask task;
        task.id = 3;
        task.url = server.url("/hls/single.m3u8?count=200&slow=0.5");
        task.directory = dir.path();
        task.fileName = "video.mp4";
        task.streamKind = StreamKind::Hls;
        auto control = std::make_shared<TransferControl>();
        DownloadWorker worker(task, 4, NetworkOptions(), control);
        QSignalSpy finished(&worker, &DownloadWorker::finished);
        QElapsedTimer timer;
        timer.start();
        std::thread stopper([&] {
            QThread::msleep(400);
            control->stop = true;
        });
        worker.run();
        stopper.join();
        QCOMPARE(finished.first().first().value<WorkerOutcome>().error.kind, ErrorKind::Cancelled);
        QVERIFY(timer.elapsed() < 3000);
    }

    void managerRunsAStreamEndToEnd()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("t.db")));
        QVERIFY(db.migrate());
        DownloadRepository repo(db);
        SettingsRepository settings(dir.filePath("t.ini"));
        AppSettings s = settings.current();
        s.downloadDirectory = dir.path();
        s.retryDelaySeconds = 1;
        settings.save(s);
        DownloadManager manager(&repo, &settings);

        AddRequest request;
        request.url = server.url("/hls/master.m3u8?count=8");
        request.directory = dir.path();
        request.fileName = "lesson.mp4";
        request.streamVariant = "720p-2500000";
        QString error;
        const qint64 id = manager.addDownload(request, &error);
        QVERIFY2(id > 0, qPrintable(error));
        QCOMPARE(manager.task(id)->streamKind, StreamKind::Hls); // spotted from the address
        QTRY_COMPARE_WITH_TIMEOUT(manager.task(id)->state, TaskState::Completed, 20000);
        QCOMPARE(manager.task(id)->fileName, QString("lesson.ts"));
        QCOMPARE(manager.task(id)->streamLabel, QString("720p"));
        QCOMPARE(readFile(dir.filePath("lesson.ts")), expectedPieces(2000, 8, 20000));

        const QVector<DownloadTask> saved = repo.loadAll();
        QCOMPARE(saved.first().streamKind, StreamKind::Hls);
        QCOMPARE(saved.first().streamVariant, QString("720p-2500000"));

        // The address alone names a useful file; "master.m3u8" would not be.
        AddRequest unnamed;
        unnamed.url = server.url("/hls/single.m3u8?count=2&n=1");
        unnamed.directory = dir.path();
        unnamed.startNow = false;
        const qint64 second = manager.addDownload(unnamed, &error);
        QCOMPARE(manager.task(second)->fileName, QString("single.mp4"));

        // Cancelling removes the pieces.
        AddRequest slow;
        slow.url = server.url("/hls/single.m3u8?count=40&slow=0.2&n=2");
        slow.directory = dir.path();
        slow.fileName = "slow.mp4";
        const qint64 third = manager.addDownload(slow, &error);
        QTRY_VERIFY_WITH_TIMEOUT(streamBytesOnDisk(manager.task(third)->streamDirectory()) > 0, 10000);
        const QString pieces = manager.task(third)->streamDirectory();
        QVERIFY(manager.cancel(third));
        QTRY_COMPARE_WITH_TIMEOUT(manager.task(third)->state, TaskState::Cancelled, 5000);
        QVERIFY(!QFile::exists(pieces));
    }

    void missingFfmpegLeavesTwoFiles()
    {
        QTemporaryDir dir;
        QVector<StreamTrack> tracks(2);
        tracks[0].role = StreamTrack::Video;
        tracks[0].extension = "mp4";
        tracks[1].role = StreamTrack::Audio;
        tracks[1].extension = "m4a";
        MediaSegment segment;
        tracks[0].segments = { segment };
        tracks[1].segments = { segment };
        QFile v(streamPiecePath(dir.path(), 0, 0));
        QFile a(streamPiecePath(dir.path(), 1, 0));
        QVERIFY(v.open(QIODevice::WriteOnly));
        QVERIFY(a.open(QIODevice::WriteOnly));
        v.write("video");
        a.write("audio");
        v.close();
        a.close();
        std::atomic_bool cancel { false };
        const MergeResult merged = mergeStream(tracks, dir.path(), cancel, QString());
        QVERIFY(merged.ok);
        QCOMPARE(readFile(merged.outputPath), QByteArray("video"));
        QCOMPARE(readFile(merged.extraPath), QByteArray("audio"));
        QVERIFY(merged.note.contains("FFmpeg"));
    }

    // ---------------------------------------------------------------- real media (needs FFmpeg)

    void realHlsBecomesPlayableMp4()
    {
        if (ffmpeg.isEmpty() || ffprobe.isEmpty())
            QSKIP("FFmpeg is not installed");
        const QDir root(fixtures.path());
        root.mkpath("hls");
        QVERIFY(runProcess(ffmpeg, { "-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=duration=6:size=320x240:rate=15",
                                     "-f", "lavfi", "-i", "sine=frequency=440:duration=6", "-c:v", "mpeg4", "-c:a", "aac", "-shortest",
                                     "-f", "hls", "-hls_time", "2", "-hls_playlist_type", "vod", root.filePath("hls/index.m3u8") }));
        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/static/hls/index.m3u8"), dir.path(), StreamKind::Hls);
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QCOMPARE(outcome.task.fileName, QString("video.mp4"));
        QByteArray report;
        QVERIFY(runProcess(ffprobe, { "-v", "error", "-show_entries", "stream=codec_type:format=duration", "-of", "default=nw=1", dir.filePath("video.mp4") }, &report));
        QVERIFY2(report.contains("codec_type=video") && report.contains("codec_type=audio"), report.constData());
    }

    void realDashMergesAudioAndVideo()
    {
        if (ffmpeg.isEmpty() || ffprobe.isEmpty())
            QSKIP("FFmpeg is not installed");
        const QDir root(fixtures.path());
        root.mkpath("dash");
        QVERIFY(runProcess(ffmpeg, { "-y", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=duration=6:size=320x240:rate=15",
                                     "-f", "lavfi", "-i", "sine=frequency=440:duration=6", "-c:v", "mpeg4", "-c:a", "aac", "-shortest",
                                     "-f", "dash", "-seg_duration", "2", root.filePath("dash/manifest.mpd") }));
        const StreamInfo info = inspectStream(QUrl(server.url("/static/dash/manifest.mpd")), NetworkOptions());
        QVERIFY2(info.ok, qPrintable(info.error.message));
        QVERIFY(info.variants.first().separateAudio);

        QTemporaryDir dir;
        const WorkerOutcome outcome = runStreamWorker(server.url("/static/dash/manifest.mpd"), dir.path(), StreamKind::Dash);
        QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
        QVERIFY(outcome.task.fileName.endsWith(".mp4"));
        QByteArray report;
        QVERIFY(runProcess(ffprobe, { "-v", "error", "-show_entries", "stream=codec_type:format=duration", "-of", "default=nw=1",
                                      dir.filePath(outcome.task.fileName) }, &report));
        QVERIFY2(report.contains("codec_type=video") && report.contains("codec_type=audio"), report.constData());
        const double seconds = report.mid(report.indexOf("duration=") + 9).trimmed().toDouble();
        QVERIFY2(qAbs(seconds - 6.0) < 1.0, report.constData());
    }
};

QTEST_GUILESS_MAIN(TestMedia)
#include "tst_media.moc"
