#pragma once

#include <QString>
#include <QtGlobal>

struct sqlite3;
struct sqlite3_stmt;

namespace haul {

// RAII wrapper around one prepared SQLite statement. All values are bound as
// parameters (never pasted into the SQL text), which prevents SQL injection.
class Statement
{
public:
    Statement(sqlite3 *db, const char *sql);
    ~Statement();
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    bool isValid() const { return m_stmt != nullptr; }

    void bind(int index, const QString &value);
    void bind(int index, qint64 value);
    void bind(int index, int value) { bind(index, static_cast<qint64>(value)); }
    void bindNull(int index);

    // Returns true while a row is available.
    bool next();
    // Runs a statement that returns no rows. Returns true on success.
    bool execute();

    qint64 int64(int column) const;
    QString text(int column) const;
    bool isNull(int column) const;

private:
    sqlite3 *m_db = nullptr;
    sqlite3_stmt *m_stmt = nullptr;
};

class Database
{
public:
    Database() = default;
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(const QString &path);
    void close();
    bool isOpen() const { return m_db != nullptr; }

    // Creates or upgrades the schema to the newest version.
    bool migrate();
    int schemaVersion();
    static int latestSchemaVersion();

    bool exec(const char *sql);
    bool begin() { return exec("BEGIN IMMEDIATE"); }
    bool commit() { return exec("COMMIT"); }
    void rollback() { exec("ROLLBACK"); }

    qint64 lastInsertId() const;
    QString lastError() const { return m_lastError; }
    sqlite3 *handle() const { return m_db; }

private:
    sqlite3 *m_db = nullptr;
    QString m_lastError;
};

} // namespace haul
