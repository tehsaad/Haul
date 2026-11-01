#pragma once

#include "Database.h"
#include "core/DownloadTask.h"

#include <QVector>

namespace haul {

// Saves and loads downloads (and their segments) in SQLite.
// Credentials are never stored: URLs with user info are rejected before they get here.
class DownloadRepository
{
public:
    explicit DownloadRepository(Database &db);

    // Inserts a new task and assigns task.id. Returns false on failure.
    bool insert(DownloadTask &task);
    // Writes every persistent field and replaces the segment rows, in one transaction.
    bool update(const DownloadTask &task);
    // Cheap update used while a transfer is running.
    bool updateProgress(const DownloadTask &task);
    bool remove(qint64 id);

    QVector<DownloadTask> loadAll();

private:
    bool writeSegments(const DownloadTask &task);

    Database &m_db;
};

// Applies Haul's startup recovery policy to tasks loaded from a previous session:
//  * "Downloading" tasks were interrupted (crash, power loss): they become Paused and are
//    flagged so the user sees "Interrupted". They are revalidated before resuming.
//  * "Queued" tasks keep their place in the queue.
//  * downloaded byte counts are corrected to what is really on disk.
//  * a task is never marked Completed here: only tasks already Completed stay Completed.
// Returns the number of tasks that were interrupted.
int applyStartupRecovery(QVector<DownloadTask> &tasks);

} // namespace haul
