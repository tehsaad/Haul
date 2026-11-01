// End-to-end tests of DownloadManager with real worker threads, a real SQLite database and
// the local test server: life cycle, pause/resume, cancel, retries, concurrency, the queue
// and recovery after a restart.

#include "TestSupport.h"

#include "app/DownloadManager.h"
#include "storage/Database.h"
#include "storage/DownloadRepository.h"
#include "storage/SettingsRepository.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>

using namespace haul;
using namespace testsupport;

// One self-contained Haul "installation" in a temporary folder.
struct Fixture {
    QTemporaryDir dir;
    Database db;
    std::unique_ptr<DownloadRepository> repo;
    std::unique_ptr<SettingsRepository> settings;
    std::unique_ptr<DownloadManager> manager;

    explicit Fixture(int maxConcurrent = 3, int connections = 1)
    {
        open();
        AppSettings s = settings->current();
        s.downloadDirectory = dir.path();
        s.maxConcurrentDownloads = maxConcurrent;
        s.maxConnectionsPerDownload = connections;
        s.retryDelaySeconds = 1;
        s.maxRetries = 3;
        settings->save(s);
    }

    void open()
    {
        db.open(dir.filePath("haul.db"));
        db.migrate();
        repo = std::make_unique<DownloadRepository>(db);
        settings = std::make_unique<SettingsRepository>(dir.filePath("settings.ini"));
        manager = std::make_unique<DownloadManager>(repo.get(), settings.get());
    }

    // Simulates quitting and starting Haul again with the same data.
    void restart()
    {
        manager.reset();
        settings.reset();
        repo.reset();
        db.close();
        open();
        manager->restore();
    }

    qint64 add(const QString &url, bool startNow = true)
    {
        AddRequest r;
        r.url = url;
        r.directory = dir.path();
        r.startNow = startNow;
        QString error;
        const qint64 id = manager->addDownload(r, &error);
        if (id == 0)
            qWarning() << "add failed:" << error;
        return id;
    }

    TaskState state(qint64 id) const { return manager->task(id) ? manager->task(id)->state : TaskState::Cancelled; }
};

class TestManager : public QObject
{
    Q_OBJECT

    TestServer server;

private slots:
    void initTestCase()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        QVERIFY(server.start());
    }

    void cleanupTestCase() { curl_global_cleanup(); }

    void addAndComplete()
    {
        Fixture f;
        QSignalSpy added(f.manager.get(), &DownloadManager::taskAdded);
        const qint64 id = f.add(server.url("/file/200000"));
        QVERIFY(id > 0);
        QCOMPARE(added.count(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 15000);

        const DownloadTask *t = f.manager->task(id);
        QCOMPARE(t->downloadedBytes, qint64(200000));
        QVERIFY(t->completedAt.isValid());
        QCOMPARE(readFile(t->finalPath()), expectedContent(200000));
        QCOMPARE(f.repo->loadAll().first().state, TaskState::Completed); // persisted
        QCOMPARE(f.manager->stats().completed, 1);
    }

    void rejectsInvalidAndDuplicate()
    {
        Fixture f;
        AddRequest r;
        r.directory = f.dir.path();
        QString error;

        r.url = "file:///etc/passwd";
        QCOMPARE(f.manager->addDownload(r, &error), qint64(0));
        QVERIFY(!error.isEmpty());

        r.url = "https://example.com/x";
        r.directory = "relative";
        QCOMPARE(f.manager->addDownload(r, &error), qint64(0));

        r.directory = f.dir.path();
        r.expectedSha256 = "nothex";
        QCOMPARE(f.manager->addDownload(r, &error), qint64(0));

        const qint64 first = f.add(server.url("/file/100000000?rate=10000"), false);
        QVERIFY(first > 0);
        r.expectedSha256.clear();
        r.url = server.url("/file/100000000?rate=10000");
        QCOMPARE(f.manager->addDownload(r, &error), qint64(0)); // duplicate
        QVERIFY(error.contains("already"));
    }

    void namesNeverCollide()
    {
        Fixture f;
        const qint64 a = f.add(server.url("/file/100?x=1"), false);
        const qint64 b = f.add(server.url("/file/100?x=2"), false);
        QVERIFY(f.manager->task(a)->fileName != f.manager->task(b)->fileName);
    }

    void pauseAndResume()
    {
        Fixture f;
        const qint64 size = 2'000'000;
        const qint64 id = f.add(server.url(QString("/file/%1?rate=500000").arg(size)));
        QTRY_VERIFY_WITH_TIMEOUT(f.manager->task(id)->downloadedBytes > 100000, 10000);

        QVERIFY(f.manager->pause(id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Paused, 5000);
        const qint64 pausedAt = QFileInfo(f.manager->task(id)->partPath()).size();
        QVERIFY(pausedAt > 0 && pausedAt < size);
        QTest::qWait(600);
        QCOMPARE(QFileInfo(f.manager->task(id)->partPath()).size(), pausedAt); // really stopped

        QVERIFY(f.manager->resume(id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 20000);
        QCOMPARE(readFile(f.manager->task(id)->finalPath()), expectedContent(size));
    }

    void segmentedPauseAndResume()
    {
        Fixture f(3, 4);
        const qint64 size = 4 * 1024 * 1024;
        const qint64 id = f.add(server.url(QString("/file/%1?rate=1000000").arg(size)));
        QTRY_VERIFY_WITH_TIMEOUT(f.manager->task(id)->segments.size() == 4
                                     && f.manager->task(id)->downloadedBytes > 200000, 10000);
        QVERIFY(f.manager->pause(id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Paused, 5000);
        QCOMPARE(f.manager->task(id)->segments.size(), 4);

        QVERIFY(f.manager->resume(id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 30000);
        QCOMPARE(sha256Hex(readFile(f.manager->task(id)->finalPath())), sha256Hex(expectedContent(size)));
    }

    void cancelRemovesPartialData()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/5000000?rate=200000"));
        QTRY_VERIFY_WITH_TIMEOUT(f.manager->task(id)->downloadedBytes > 0, 10000);
        const QString part = f.manager->task(id)->partPath();
        QVERIFY(f.manager->cancel(id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Cancelled, 5000);
        QVERIFY(!QFile::exists(part));
        QCOMPARE(f.manager->task(id)->downloadedBytes, qint64(0));
    }

    void invalidOperationsAreRejected()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/1000"));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 10000);
        QVERIFY(!f.manager->pause(id));  // cannot pause a finished download
        QVERIFY(!f.manager->resume(id));
        QVERIFY(!f.manager->cancel(id));
        QVERIFY(!f.manager->retry(id));
        QVERIFY(!f.manager->pause(424242)); // unknown id
    }

    void transientErrorsAreRetried()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/flaky/50000?key=manager-retry&fail=2"));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 30000);
        QCOMPARE(f.manager->task(id)->retryCount, 2);
    }

    void retriesAreBounded()
    {
        Fixture f;
        AppSettings s = f.settings->current();
        s.maxRetries = 1;
        f.settings->save(s);
        const qint64 id = f.add(server.url("/file/10?status=503"));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Failed, 20000);
        QCOMPARE(f.manager->task(id)->retryCount, 1);
        QVERIFY(!f.manager->task(id)->errorMessage.isEmpty());
    }

    void permanentErrorsAreNotRetried()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/10?status=404"));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Failed, 10000);
        QCOMPARE(f.manager->task(id)->retryCount, 0);
        QVERIFY(f.manager->task(id)->errorMessage.contains("404"));
    }

    void concurrencyLimitIsRespected()
    {
        Fixture f(1);
        QVector<qint64> ids;
        for (int i = 0; i < 3; ++i)
            ids.append(f.add(server.url(QString("/file/300000?rate=300000&n=%1").arg(i))));

        int maxActive = 0;
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 20000) {
            maxActive = std::max(maxActive, f.manager->activeCount());
            bool allDone = true;
            for (qint64 id : ids)
                allDone = allDone && f.state(id) == TaskState::Completed;
            if (allDone)
                break;
            QTest::qWait(50);
        }
        QCOMPARE(maxActive, 1);
        for (qint64 id : ids)
            QCOMPARE(f.state(id), TaskState::Completed);
    }

    void queueWaitsUntilStarted()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/1000"), false);
        QTest::qWait(1200);
        QCOMPARE(f.state(id), TaskState::Queued);
        f.manager->startQueue();
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 10000);
    }

    void stopQueueRequeuesQueueTasks()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/5000000?rate=300000"), false);
        f.manager->startQueue();
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Downloading, 5000);
        f.manager->stopQueue();
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Queued, 5000);
        QCOMPARE(f.manager->activeCount(), 0);
    }

    void priorityAndOrdering()
    {
        Fixture f;
        const qint64 a = f.add(server.url("/file/10?o=a"), false);
        const qint64 b = f.add(server.url("/file/10?o=b"), false);
        const qint64 c = f.add(server.url("/file/10?o=c"), false);
        auto order = [&] {
            QVector<qint64> out;
            for (const DownloadTask &t : f.manager->queueOrder())
                out.append(t.id);
            return out;
        };
        QCOMPARE(order(), QVector<qint64>({ a, b, c }));
        QVERIFY(f.manager->moveUp(c));
        QCOMPARE(order(), QVector<qint64>({ a, c, b }));
        QVERIFY(f.manager->setPriority(b, Priority::High));
        QCOMPARE(order(), QVector<qint64>({ b, a, c }));
    }

    void gracefulRestartRestoresTasks()
    {
        Fixture f;
        const qint64 size = 3'000'000;
        const qint64 id = f.add(server.url(QString("/file/%1?rate=400000").arg(size)));
        QTRY_VERIFY_WITH_TIMEOUT(f.manager->task(id)->downloadedBytes > 100000, 10000);

        f.restart(); // the manager is destroyed mid-download and rebuilt from the database
        const DownloadTask *t = f.manager->task(id);
        QVERIFY(t);
        QCOMPARE(t->state, TaskState::Paused);
        QVERIFY(t->interrupted);
        QVERIFY(t->downloadedBytes > 0);

        QVERIFY(f.manager->resume(id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 20000);
        QCOMPARE(readFile(f.manager->task(id)->finalPath()), expectedContent(size));
    }

    void crashRecovery()
    {
        Fixture f;
        // Pretend Haul crashed while this task was downloading.
        DownloadTask t;
        t.url = server.url("/file/100000");
        t.directory = f.dir.path();
        t.fileName = "crashed.bin";
        t.state = TaskState::Downloading;
        t.totalBytes = 100000;
        t.acceptRanges = true;
        t.etag = "\"fixed-100000\"";
        QVERIFY(f.repo->insert(t));
        QFile part(t.partPath());
        QVERIFY(part.open(QIODevice::WriteOnly));
        part.write(expectedContent(40000));
        part.close();

        QSignalSpy notes(f.manager.get(), &DownloadManager::notification);
        f.manager->restore();
        const DownloadTask *r = f.manager->task(t.id);
        QCOMPARE(r->state, TaskState::Paused);
        QVERIFY(r->interrupted);
        QCOMPARE(r->downloadedBytes, qint64(40000));
        QCOMPARE(f.manager->activeCount(), 0); // nothing starts by itself
        QVERIFY(notes.count() >= 1);

        QVERIFY(f.manager->resume(t.id));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(t.id), TaskState::Completed, 10000);
        QCOMPARE(readFile(t.finalPath()), expectedContent(100000));
    }

    void removeWithFile()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/1000"));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 10000);
        const QString path = f.manager->task(id)->finalPath();
        QVERIFY(QFile::exists(path));
        QSignalSpy removed(f.manager.get(), &DownloadManager::taskRemoved);
        QVERIFY(f.manager->remove(id, true));
        QCOMPARE(removed.count(), 1);
        QVERIFY(!f.manager->task(id));
        QVERIFY(!QFile::exists(path));
        QVERIFY(f.repo->loadAll().isEmpty());
    }

    void removeWhileDownloading()
    {
        Fixture f;
        const qint64 id = f.add(server.url("/file/5000000?rate=200000"));
        QTRY_VERIFY_WITH_TIMEOUT(f.manager->task(id)->downloadedBytes > 0, 10000);
        const QString part = f.manager->task(id)->partPath();
        QVERIFY(f.manager->remove(id, false));
        QTRY_VERIFY_WITH_TIMEOUT(!f.manager->task(id), 5000);
        QVERIFY(!QFile::exists(part));
    }

    void bandwidthLimitFromSettings()
    {
        Fixture f;
        AppSettings s = f.settings->current();
        s.speedLimitKiB = 100;
        f.settings->save(s);
        QElapsedTimer timer;
        timer.start();
        const qint64 id = f.add(server.url("/file/300000"));
        QTRY_COMPARE_WITH_TIMEOUT(f.state(id), TaskState::Completed, 20000);
        QVERIFY2(timer.elapsed() >= 2000, qPrintable(QString("took only %1 ms").arg(timer.elapsed())));
    }
};

QTEST_GUILESS_MAIN(TestManager)
#include "tst_manager.moc"
