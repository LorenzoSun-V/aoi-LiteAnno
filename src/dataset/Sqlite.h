#pragma once

// A thin layer over the SQLite C API for the catalog. Not part of the dataset interface.

#include <QByteArray>
#include <QString>

#include <sqlite3.h>

#include <optional>
#include <utility>

namespace liteanno::dataset::sqlite {

class Statement
{
public:
    Statement() = default;
    Statement(sqlite3* db, const char* sql)
    {
        if (sqlite3_prepare_v2(db, sql, -1, &m_statement, nullptr) != SQLITE_OK)
            m_statement = nullptr;
    }
    Statement(Statement&& other) noexcept
        : m_statement(std::exchange(other.m_statement, nullptr))
    {
    }
    Statement& operator=(Statement&& other) noexcept
    {
        std::swap(m_statement, other.m_statement);
        return *this;
    }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    ~Statement() { sqlite3_finalize(m_statement); }

    bool isValid() const { return m_statement != nullptr; }

    Statement& bind(int index, qint64 value)
    {
        sqlite3_bind_int64(m_statement, index, value);
        return *this;
    }
    Statement& bind(int index, const QString& value)
    {
        const QByteArray utf8 = value.toUtf8();
        sqlite3_bind_text(m_statement, index, utf8.constData(), int(utf8.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Statement& bindNull(int index)
    {
        sqlite3_bind_null(m_statement, index);
        return *this;
    }

    // SQLITE_ROW, SQLITE_DONE or an error code.
    int step() { return m_statement ? sqlite3_step(m_statement) : SQLITE_MISUSE; }
    // Runs a statement that returns no rows; true on success. Resets it for the next use.
    bool run()
    {
        const int result = step();
        reset();
        return result == SQLITE_DONE || result == SQLITE_ROW;
    }
    void reset()
    {
        if (m_statement) {
            sqlite3_reset(m_statement);
            sqlite3_clear_bindings(m_statement);
        }
    }

    qint64 integer(int column) const { return sqlite3_column_int64(m_statement, column); }
    bool isNull(int column) const { return sqlite3_column_type(m_statement, column) == SQLITE_NULL; }
    QString text(int column) const
    {
        const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(m_statement, column));
        return QString::fromUtf8(data, sqlite3_column_bytes(m_statement, column));
    }

private:
    sqlite3_stmt* m_statement = nullptr;
};

inline bool exec(sqlite3* db, const char* sql)
{
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

inline QString lastError(sqlite3* db)
{
    return db ? QString::fromUtf8(sqlite3_errmsg(db)) : QStringLiteral("no database");
}

} // namespace liteanno::dataset::sqlite
