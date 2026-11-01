// Tests for queue ordering, concurrency limits, moving tasks and the scheduler policy.

#include "scheduler/DownloadQueue.h"
#include "scheduler/Schedule.h"

#include <QTest>

using namespace haul;

namespace {

QueueEntry entry(qint64 id, TaskState state, Priority priority, int position, bool startRequested = false)
{
    QueueEntry e;
    e.id = id;
    e.state = state;
    e.priority = priority;
    e.queuePosition = position;
    e.startRequested = startRequested;
    return e;
}

QVector<qint64> ids(const QVector<QueueEntry> &entries)
{
    QVector<qint64> out;
    for (const QueueEntry &e : entries)
        out.append(e.id);
    return out;
}

QDateTime at(int h, int m)
{
    return QDateTime(QDate(2026, 10, 24), QTime(h, m));
}

} // namespace

class TestQueue : public QObject
{
    Q_OBJECT

private slots:
    void fifoWithinPriority()
    {
        const QVector<QueueEntry> entries = {
            entry(1, TaskState::Queued, Priority::Normal, 3),
            entry(2, TaskState::Queued, Priority::Normal, 1),
            entry(3, TaskState::Queued, Priority::Normal, 2),
        };
        QCOMPARE(ids(DownloadQueue::ordered(entries)), QVector<qint64>({ 2, 3, 1 }));
    }

    void priorityBeforePosition()
    {
        const QVector<QueueEntry> entries = {
            entry(1, TaskState::Queued, Priority::Low, 1),
            entry(2, TaskState::Queued, Priority::Normal, 2),
            entry(3, TaskState::Queued, Priority::High, 3),
            entry(4, TaskState::Completed, Priority::High, 0), // not waiting
        };
        QCOMPARE(ids(DownloadQueue::ordered(entries)), QVector<qint64>({ 3, 2, 1 }));
    }

    void concurrencyLimit()
    {
        QVector<QueueEntry> entries;
        for (int i = 1; i <= 6; ++i)
            entries.append(entry(i, TaskState::Queued, Priority::Normal, i));
        entries.append(entry(7, TaskState::Downloading, Priority::Normal, 0));

        const QDateTime now = QDateTime::currentDateTime();
        QCOMPARE(DownloadQueue::selectToStart(entries, 1, 3, true, now), QVector<qint64>({ 1, 2 }));
        QVERIFY(DownloadQueue::selectToStart(entries, 3, 3, true, now).isEmpty());
        QVERIFY(DownloadQueue::selectToStart(entries, 5, 3, true, now).isEmpty());
    }

    void stoppedQueueOnlyStartsRequested()
    {
        const QVector<QueueEntry> entries = {
            entry(1, TaskState::Queued, Priority::High, 1),
            entry(2, TaskState::Queued, Priority::Normal, 2, true),
        };
        const QDateTime now = QDateTime::currentDateTime();
        QCOMPARE(DownloadQueue::selectToStart(entries, 0, 5, false, now), QVector<qint64>({ 2 }));
        QCOMPARE(DownloadQueue::selectToStart(entries, 0, 5, true, now), QVector<qint64>({ 1, 2 }));
    }

    void retryDelayRespected()
    {
        QueueEntry waiting = entry(1, TaskState::Queued, Priority::Normal, 1, true);
        const QDateTime now = QDateTime::currentDateTime();
        waiting.retryAt = now.addSecs(30);
        QVERIFY(DownloadQueue::selectToStart({ waiting }, 0, 3, true, now).isEmpty());
        QCOMPARE(DownloadQueue::selectToStart({ waiting }, 0, 3, true, now.addSecs(31)), QVector<qint64>({ 1 }));
    }

    void moveUpAndDown()
    {
        QVector<QueueEntry> entries = {
            entry(1, TaskState::Queued, Priority::Normal, 1),
            entry(2, TaskState::Queued, Priority::Normal, 2),
            entry(3, TaskState::Queued, Priority::Normal, 3),
        };
        QVERIFY(DownloadQueue::move(entries, 3, -1));
        QCOMPARE(ids(DownloadQueue::ordered(entries)), QVector<qint64>({ 1, 3, 2 }));
        QVERIFY(DownloadQueue::move(entries, 1, +1));
        QCOMPARE(ids(DownloadQueue::ordered(entries)), QVector<qint64>({ 3, 1, 2 }));
        QVERIFY(!DownloadQueue::move(entries, 3, -1)); // already first
        QVERIFY(!DownloadQueue::move(entries, 2, +1)); // already last
        QVERIFY(!DownloadQueue::move(entries, 99, +1)); // unknown id
    }

    void moveAcrossPriority()
    {
        QVector<QueueEntry> entries = {
            entry(1, TaskState::Queued, Priority::High, 5),
            entry(2, TaskState::Queued, Priority::Normal, 1),
        };
        QVERIFY(DownloadQueue::move(entries, 2, -1));
        QCOMPARE(ids(DownloadQueue::ordered(entries)), QVector<qint64>({ 2, 1 }));
    }

    void schedulerStartsOncePerDay()
    {
        ScheduleConfig config;
        config.enabled = true;
        config.startTime = QTime(2, 0);
        ScheduleState state;

        QCOMPARE(evaluateSchedule(config, at(1, 59), state, false), ScheduleAction::None);
        QCOMPARE(evaluateSchedule(config, at(2, 0), state, false), ScheduleAction::StartQueue);
        // Already started for today's window: no duplicate start even if the queue was stopped.
        QCOMPARE(evaluateSchedule(config, at(2, 1), state, false), ScheduleAction::None);
        // Next day starts again.
        QCOMPARE(evaluateSchedule(config, at(2, 0).addDays(1), state, false), ScheduleAction::StartQueue);
    }

    void schedulerMissedStartInsideWindow()
    {
        ScheduleConfig config;
        config.enabled = true;
        config.startTime = QTime(2, 0);
        config.stopEnabled = true;
        config.stopTime = QTime(8, 0);
        ScheduleState state;
        // App opened at 05:00: inside the window, so the missed start happens now.
        QCOMPARE(evaluateSchedule(config, at(5, 0), state, false), ScheduleAction::StartQueue);
    }

    void schedulerMissedWindowWaits()
    {
        ScheduleConfig config;
        config.enabled = true;
        config.startTime = QTime(2, 0);
        config.stopEnabled = true;
        config.stopTime = QTime(8, 0);
        ScheduleState state;
        // App opened at 09:00: the window has ended; do not start today.
        QCOMPARE(evaluateSchedule(config, at(9, 0), state, false), ScheduleAction::None);
    }

    void schedulerStops()
    {
        ScheduleConfig config;
        config.enabled = true;
        config.startTime = QTime(2, 0);
        config.stopEnabled = true;
        config.stopTime = QTime(8, 0);
        ScheduleState state;
        QCOMPARE(evaluateSchedule(config, at(2, 0), state, false), ScheduleAction::StartQueue);
        QCOMPARE(evaluateSchedule(config, at(8, 0), state, true), ScheduleAction::StopQueue);
        QCOMPARE(evaluateSchedule(config, at(8, 1), state, true), ScheduleAction::None); // once
    }

    void schedulerWindowAcrossMidnight()
    {
        ScheduleConfig config;
        config.enabled = true;
        config.startTime = QTime(23, 0);
        config.stopEnabled = true;
        config.stopTime = QTime(6, 0);
        ScheduleState state;

        QCOMPARE(evaluateSchedule(config, at(22, 0), state, false), ScheduleAction::None);
        QCOMPARE(evaluateSchedule(config, at(23, 30), state, false), ScheduleAction::StartQueue);
        // 01:00 the next day belongs to the same window: no second start.
        QCOMPARE(evaluateSchedule(config, at(1, 0).addDays(1), state, false), ScheduleAction::None);
        QCOMPARE(evaluateSchedule(config, at(6, 0).addDays(1), state, true), ScheduleAction::StopQueue);
    }

    void schedulerDisabled()
    {
        ScheduleConfig config;
        ScheduleState state;
        QCOMPARE(evaluateSchedule(config, at(12, 0), state, false), ScheduleAction::None);
    }
};

QTEST_GUILESS_MAIN(TestQueue)
#include "tst_queue.moc"
