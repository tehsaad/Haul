// Tests for SQLite persistence, migrations, startup recovery and settings validation.

#include "storage/Database.h"
#include "storage/DownloadRepository.h"
#include "storage/SettingsRepository.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sqlite3.h>

using namespace haul;

namespace {

DownloadTask sampleTask(const QString &dir)
{
    DownloadTask t;
    t.url = "https://example.com/a.iso";
    t.directory = dir;
    t.fileName = "a.iso";
    t.state = TaskState::Queued;
    t.priority = Priority::High;
    t.queuePosition = 4;
    t.totalBytes = 4096;
    t.downloadedBytes = 1024;
    t.etag = "\"abc\"";
    t.lastModified = "Mon, 05 Oct 2026 10:00:00 GMT";
    t.acceptRanges = true;
    t.retryCount = 2;
    t.errorMessage = "timeout";
    t.expectedSha256 = QString(64, QChar('a'));
    t.autoName = true;
    t.createdAt = QDateTime::fromMSecsSinceEpoch(1'760'000'000'000);
    return t;
}

void writeBytes(const QString &path, qint64 count)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QByteArray(static_cast<int>(count), 'x'));
}

} // namespace

class TestStorage : public QObject
{
    Q_OBJECT

private slots:
    void migrationCreatesSchema()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QCOMPARE(db.schemaVersion(), 0);
        QVERIFY(db.migrate());
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        QVERIFY(db.migrate()); // running again is harmless
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
    }

    void upgradesVersionOneDatabase()
    {
        // A database exactly as Haul schema version 1 created it, with one row.
        QTemporaryDir dir;
        const QString path = dir.filePath("v1.db");
        {
            Database db;
            QVERIFY(db.open(path));
            QVERIFY(db.exec("CREATE TABLE schema_version (version INTEGER NOT NULL);"
                            "INSERT INTO schema_version VALUES (1);"
                            "CREATE TABLE downloads (id INTEGER PRIMARY KEY AUTOINCREMENT, url TEXT NOT NULL,"
                            " directory TEXT NOT NULL, file_name TEXT NOT NULL, status TEXT NOT NULL,"
                            " priority INTEGER NOT NULL DEFAULT 1, queue_position INTEGER NOT NULL DEFAULT 0,"
                            " total_bytes INTEGER NOT NULL DEFAULT -1, downloaded INTEGER NOT NULL DEFAULT 0,"
                            " etag TEXT, last_modified TEXT, accept_ranges INTEGER NOT NULL DEFAULT 0,"
                            " retry_count INTEGER NOT NULL DEFAULT 0, error TEXT, interrupted INTEGER NOT NULL DEFAULT 0,"
                            " expected_sha256 TEXT, checksum_status INTEGER NOT NULL DEFAULT 0, created_at INTEGER,"
                            " started_at INTEGER, completed_at INTEGER);"
                            "CREATE TABLE segments (download_id INTEGER NOT NULL, idx INTEGER NOT NULL,"
                            " start_byte INTEGER NOT NULL, end_byte INTEGER NOT NULL, downloaded INTEGER NOT NULL DEFAULT 0,"
                            " PRIMARY KEY (download_id, idx));"
                            "INSERT INTO downloads (url, directory, file_name, status) VALUES"
                            " ('https://example.com/old.zip', '/tmp', 'old.zip', 'completed');"));
        }
        Database db;
        QVERIFY(db.open(path));
        QVERIFY(db.migrate());
        QCOMPARE(db.schemaVersion(), Database::latestSchemaVersion());
        DownloadRepository repo(db);
        const QVector<DownloadTask> tasks = repo.loadAll();
        QCOMPARE(tasks.size(), 1); // existing history survives the upgrade
        QCOMPARE(tasks.first().fileName, QString("old.zip"));
        QVERIFY(!tasks.first().autoName);
    }

    void refusesNewerSchema()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.migrate());
        QVERIFY(db.exec("UPDATE schema_version SET version = 999"));
        QVERIFY(!db.migrate());
    }

    void roundTrip()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.migrate());
        DownloadRepository repo(db);

        DownloadTask t = sampleTask(dir.path());
        SegmentInfo s0 { 0, 0, 2047, 100 };
        SegmentInfo s1 { 1, 2048, 4095, 200 };
        t.segments = { s0, s1 };
        QVERIFY(repo.insert(t));
        QVERIFY(t.id > 0);

        const QVector<DownloadTask> loaded = repo.loadAll();
        QCOMPARE(loaded.size(), 1);
        const DownloadTask &l = loaded.first();
        QCOMPARE(l.id, t.id);
        QCOMPARE(l.url, t.url);
        QCOMPARE(l.fileName, t.fileName);
        QCOMPARE(l.state, TaskState::Queued);
        QCOMPARE(l.priority, Priority::High);
        QCOMPARE(l.queuePosition, 4);
        QCOMPARE(l.totalBytes, qint64(4096));
        QCOMPARE(l.etag, t.etag);
        QCOMPARE(l.lastModified, t.lastModified);
        QVERIFY(l.acceptRanges);
        QCOMPARE(l.retryCount, 2);
        QCOMPARE(l.expectedSha256, t.expectedSha256);
        QCOMPARE(l.createdAt, t.createdAt);
        QVERIFY(l.autoName);
        QCOMPARE(l.segments.size(), 2);
        QCOMPARE(l.segments[1].start, qint64(2048));
        QCOMPARE(l.segments[1].downloaded, qint64(200));
    }

    void updateAndRemove()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.migrate());
        DownloadRepository repo(db);

        DownloadTask t = sampleTask(dir.path());
        QVERIFY(repo.insert(t));
        t.state = TaskState::Completed;
        t.downloadedBytes = 4096;
        t.completedAt = QDateTime::currentDateTime();
        QVERIFY(repo.update(t));
        QCOMPARE(repo.loadAll().first().state, TaskState::Completed);

        t.downloadedBytes = 10;
        QVERIFY(repo.updateProgress(t));
        QCOMPARE(repo.loadAll().first().downloadedBytes, qint64(10));

        QVERIFY(repo.remove(t.id));
        QVERIFY(repo.loadAll().isEmpty());
    }

    void sqlInjectionIsHarmless()
    {
        QTemporaryDir dir;
        Database db;
        QVERIFY(db.open(dir.filePath("test.db")));
        QVERIFY(db.migrate());
        DownloadRepository repo(db);

        DownloadTask t = sampleTask(dir.path());
        t.fileName = "x'); DROP TABLE downloads; --";
        QVERIFY(repo.insert(t));
        const QVector<DownloadTask> loaded = repo.loadAll();
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded.first().fileName, t.fileName);
    }

    void persistsAcrossReopen()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("test.db");
        {
            Database db;
            QVERIFY(db.open(path));
            QVERIFY(db.migrate());
            DownloadRepository repo(db);
            DownloadTask t = sampleTask(dir.path());
            QVERIFY(repo.insert(t));
        }
        Database db;
        QVERIFY(db.open(path));
        QVERIFY(db.migrate());
        DownloadRepository repo(db);
        QCOMPARE(repo.loadAll().size(), 1);
    }

    void recoveryMarksInterrupted()
    {
        QTemporaryDir dir;
        QVector<DownloadTask> tasks;

        DownloadTask running = sampleTask(dir.path());
        running.fileName = "running.bin";
        running.state = TaskState::Downloading;
        running.downloadedBytes = 999; // database says 999 ...
        writeBytes(running.partPath(), 1500); // ... but the disk has 1500
        tasks.append(running);

        DownloadTask done = sampleTask(dir.path());
        done.fileName = "done.bin";
        done.state = TaskState::Completed;
        done.downloadedBytes = 4096;
        tasks.append(done);

        DownloadTask queued = sampleTask(dir.path());
        queued.fileName = "queued.bin";
        queued.downloadedBytes = 50; // no partial file on disk
        tasks.append(queued);

        QCOMPARE(applyStartupRecovery(tasks), 1);
        QCOMPARE(tasks[0].state, TaskState::Paused);
        QVERIFY(tasks[0].interrupted);
        QCOMPARE(tasks[0].downloadedBytes, qint64(1500));
        QCOMPARE(tasks[1].state, TaskState::Completed); // untouched
        QCOMPARE(tasks[1].downloadedBytes, qint64(4096));
        QCOMPARE(tasks[2].state, TaskState::Queued);
        QCOMPARE(tasks[2].downloadedBytes, qint64(0));
    }

    void recoveryNeverCompletes()
    {
        QTemporaryDir dir;
        DownloadTask t = sampleTask(dir.path());
        t.state = TaskState::Downloading;
        t.totalBytes = 100;
        writeBytes(t.partPath(), 100); // all bytes present, but never verified/renamed
        QVector<DownloadTask> tasks { t };
        applyStartupRecovery(tasks);
        QVERIFY(tasks[0].state != TaskState::Completed);
    }

    void recoveryReadsSegments()
    {
        QTemporaryDir dir;
        DownloadTask t = sampleTask(dir.path());
        t.state = TaskState::Downloading;
        t.segments = { SegmentInfo { 0, 0, 999, 0 }, SegmentInfo { 1, 1000, 1999, 0 } };
        writeBytes(t.segmentPath(0), 1000);
        writeBytes(t.segmentPath(1), 5000); // corrupt: larger than the segment
        QVector<DownloadTask> tasks { t };
        applyStartupRecovery(tasks);
        QCOMPARE(tasks[0].segments[0].downloaded, qint64(1000));
        QCOMPARE(tasks[0].segments[1].downloaded, qint64(1000)); // clamped to its length
        QCOMPARE(tasks[0].downloadedBytes, qint64(2000));
    }

    void settingsValidation()
    {
        AppSettings s = SettingsRepository::defaults();
        QVERIFY(validateSettings(s).isEmpty());

        AppSettings bad = s;
        bad.maxConcurrentDownloads = 0;
        bad.maxConnectionsPerDownload = 99;
        bad.speedLimitKiB = -5;
        bad.retryDelaySeconds = 0;
        bad.downloadDirectory = "relative/path";
        QCOMPARE(validateSettings(bad).size(), 5);
    }

    void settingsPersistAndReject()
    {
        QTemporaryDir dir;
        const QString ini = dir.filePath("settings.ini");
        {
            SettingsRepository repo(ini);
            QSignalSpy spy(&repo, &SettingsRepository::changed);
            AppSettings s = repo.current();
            s.maxConcurrentDownloads = 5;
            s.speedLimitKiB = 256;
            s.schedule.enabled = true;
            s.schedule.startTime = QTime(1, 30);
            QVERIFY(repo.save(s));
            QCOMPARE(spy.count(), 1);

            AppSettings invalid = s;
            invalid.maxConcurrentDownloads = 50;
            QStringList errors;
            QVERIFY(!repo.save(invalid, &errors));
            QVERIFY(!errors.isEmpty());
            QCOMPARE(repo.current().maxConcurrentDownloads, 5); // unchanged
        }
        SettingsRepository reopened(ini);
        QCOMPARE(reopened.current().maxConcurrentDownloads, 5);
        QCOMPARE(reopened.current().speedLimitKiB, 256);
        QVERIFY(reopened.current().schedule.enabled);
        QCOMPARE(reopened.current().schedule.startTime, QTime(1, 30));

        reopened.restoreDefaults();
        QCOMPARE(reopened.current().maxConcurrentDownloads, SettingsRepository::defaults().maxConcurrentDownloads);
    }

    void corruptSettingsFallBackToDefaults()
    {
        QTemporaryDir dir;
        const QString ini = dir.filePath("settings.ini");
        {
            QSettings raw(ini, QSettings::IniFormat);
            raw.setValue("downloads/maxConcurrent", 9999);
        }
        SettingsRepository repo(ini);
        QCOMPARE(repo.current().maxConcurrentDownloads, SettingsRepository::defaults().maxConcurrentDownloads);
    }
};

QTEST_GUILESS_MAIN(TestStorage)
#include "tst_storage.moc"
