#include "Database.h"

#include <QDir>
#include <QFileInfo>

#include <sqlite3.h>

namespace haul {

namespace {

// Each entry upgrades the schema by one version. Never edit a released migration:
// add a new one at the end instead, so existing databases upgrade step by step.
const char *const kMigrations[] = {
    // Version 1: downloads and their segments.
    R"SQL(
    CREATE TABLE downloads (
        id              INTEGER PRIMARY KEY AUTOINCREMENT,
        url             TEXT    NOT NULL,
        directory       TEXT    NOT NULL,
        file_name       TEXT    NOT NULL,
        status          TEXT    NOT NULL,
        priority        INTEGER NOT NULL DEFAULT 1,
        queue_position  INTEGER NOT NULL DEFAULT 0,
        total_bytes     INTEGER NOT NULL DEFAULT -1,
        downloaded      INTEGER NOT NULL DEFAULT 0,
        etag            TEXT,
        last_modified   TEXT,
        accept_ranges   INTEGER NOT NULL DEFAULT 0,
        retry_count     INTEGER NOT NULL DEFAULT 0,
        error           TEXT,
        interrupted     INTEGER NOT NULL DEFAULT 0,
        expected_sha256 TEXT,
        checksum_status INTEGER NOT NULL DEFAULT 0,
        created_at      INTEGER,
        started_at      INTEGER,
        completed_at    INTEGER
    );
    CREATE TABLE segments (
        download_id INTEGER NOT NULL REFERENCES downloads(id) ON DELETE CASCADE,
        idx         INTEGER NOT NULL,
        start_byte  INTEGER NOT NULL,
        end_byte    INTEGER NOT NULL,
        downloaded  INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (download_id, idx)
    );
    CREATE INDEX idx_downloads_status ON downloads(status);
    )SQL",

    // Version 2: remember whether the file name may still be replaced by the server's suggestion.
    R"SQL(
    ALTER TABLE downloads ADD COLUMN auto_name INTEGER NOT NULL DEFAULT 0;
    )SQL",
};

constexpr int kMigrationCount = static_cast<int>(sizeof(kMigrations) / sizeof(kMigrations[0]));

} // namespace

Statement::Statement(sqlite3 *db, const char *sql)
    : m_db(db)
{
    if (sqlite3_prepare_v2(db, sql, -1, &m_stmt, nullptr) != SQLITE_OK)
        m_stmt = nullptr;
}

Statement::~Statement()
{
    if (m_stmt)
        sqlite3_finalize(m_stmt);
}

void Statement::bind(int index, const QString &value)
{
    if (value.isNull()) {
        bindNull(index);
        return;
    }
    const QByteArray utf8 = value.toUtf8();
    sqlite3_bind_text(m_stmt, index, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
}

void Statement::bind(int index, qint64 value)
{
    sqlite3_bind_int64(m_stmt, index, value);
}

void Statement::bindNull(int index)
{
    sqlite3_bind_null(m_stmt, index);
}

bool Statement::next()
{
    return m_stmt && sqlite3_step(m_stmt) == SQLITE_ROW;
}

bool Statement::execute()
{
    if (!m_stmt)
        return false;
    const int rc = sqlite3_step(m_stmt);
    return rc == SQLITE_DONE || rc == SQLITE_ROW;
}

qint64 Statement::int64(int column) const
{
    return sqlite3_column_int64(m_stmt, column);
}

QString Statement::text(int column) const
{
    const auto *data = reinterpret_cast<const char *>(sqlite3_column_text(m_stmt, column));
    if (!data)
        return QString();
    return QString::fromUtf8(data, sqlite3_column_bytes(m_stmt, column));
}

bool Statement::isNull(int column) const
{
    return sqlite3_column_type(m_stmt, column) == SQLITE_NULL;
}

Database::~Database()
{
    close();
}

bool Database::open(const QString &path)
{
    close();
    if (path != QLatin1String(":memory:"))
        QDir().mkpath(QFileInfo(path).absolutePath());

    const QByteArray utf8 = path.toUtf8();
    if (sqlite3_open_v2(utf8.constData(), &m_db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        m_lastError = m_db ? QString::fromUtf8(sqlite3_errmsg(m_db)) : QStringLiteral("Cannot open database");
        close();
        return false;
    }
    sqlite3_busy_timeout(m_db, 3000);
    exec("PRAGMA foreign_keys = ON");
    exec("PRAGMA journal_mode = WAL");
    exec("PRAGMA synchronous = NORMAL");
    return true;
}

void Database::close()
{
    if (m_db) {
        sqlite3_close_v2(m_db);
        m_db = nullptr;
    }
}

bool Database::exec(const char *sql)
{
    char *message = nullptr;
    if (sqlite3_exec(m_db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        m_lastError = QString::fromUtf8(message ? message : "unknown SQLite error");
        sqlite3_free(message);
        return false;
    }
    return true;
}

int Database::latestSchemaVersion()
{
    return kMigrationCount;
}

int Database::schemaVersion()
{
    Statement query(m_db, "SELECT version FROM schema_version LIMIT 1");
    return query.next() ? static_cast<int>(query.int64(0)) : 0;
}

bool Database::migrate()
{
    if (!exec("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL)"))
        return false;

    int version = schemaVersion();
    if (version > kMigrationCount) {
        m_lastError = QStringLiteral("The database was created by a newer version of Haul.");
        return false;
    }

    while (version < kMigrationCount) {
        if (!begin())
            return false;
        bool ok = exec(kMigrations[version]);
        if (ok) {
            exec("DELETE FROM schema_version");
            Statement update(m_db, "INSERT INTO schema_version (version) VALUES (?)");
            update.bind(1, version + 1);
            ok = update.execute();
        }
        if (!ok || !commit()) {
            rollback();
            return false;
        }
        ++version;
    }
    return true;
}

qint64 Database::lastInsertId() const
{
    return sqlite3_last_insert_rowid(m_db);
}

} // namespace haul
