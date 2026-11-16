#pragma once

#include <QString>
#include <QVariantList>
#include <QtTypes>
#include <expected>
#include <optional>

struct Progress;
class Library;
class QSqlQuery;

// The transactional outbox (ADR 0001): every write to a table that travels
// goes through ChangeLog inside a transaction, which updates the row, appends
// the operation(s) to `ops` and records the resulting `library_state`
// counters. On opening, writes that bypassed the log are caught by those
// counters and turned into operations.
class ChangeLog {
public:
    // Re-entrancy guard while ChangeLog or SpaceReplay is writing the tables.
    class Scope {
    public:
        Scope() noexcept;
        ~Scope();
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
        Scope(Scope &&) = delete;
        Scope &operator=(Scope &&) = delete;
    };

    struct Entry {
        QString space;
        QString entity;
        QString id;
        QString field;
        QString value;
        std::optional<qint64> hlc;
    };

    // Writes the initial baseline when the library has no personal space yet,
    // or reconciles any writes that bypassed the log since the last open.
    static void open(const Library &library);
    static void writeBaseline(const Library &library);
    static void reconcile(const Library &library);

    // Called at the end of a write or batch transaction to append operations
    // for any travelling rows that changed in that transaction.
    static void afterWrite(const Library &library);

    // Appends one operation in `entry` (ticking the library's HLC when
    // `entry.hlc` is not given) and updates `seen`.
    static qint64 append(const Library &library, const Entry &entry);

    // Records the current `library_state` counters as seen by the log.
    static void markLogged(const Library &library);

    // Single write path for rows that travel: each mutates the row and
    // records its operation(s) in the same transaction.
    static void setProgress(Library &library, qint64 bookId, const Progress &progress);
    static bool setDetailsAt(Library &library, qint64 bookId, qint64 at);
    static void recordRemoval(const Library &library, const QString &key, qint64 removedAt);

    // Narrow SQL access for ChangeLog and SpaceReplay.
    [[nodiscard]] static QSqlQuery query(const Library &library, const QString &sql, const QVariantList &values = {});
    static bool exec(const Library &library, const QString &sql, const QVariantList &values = {});
    [[nodiscard]] static std::expected<qint64, QString> insert(const Library &library, const QString &sql, const QVariantList &values = {});
};
