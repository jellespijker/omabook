#pragma once

#include <QString>
#include <QVariant>
#include <QVariantList>
#include <QtTypes>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

class QSqlQuery;

// The statements of one database connection, each prepared once and run
// again with new values (#104): preparing a statement costs about as much as
// running a small one, and the change log runs the same few for every row of
// a library when it first takes it in (its baseline) or reconciles it.
//
// A statement is kept only between runs: run() and each() reset it when they
// are done, so none holds a read of the database or a transaction open. One
// asked for again while it is still running (each() of the same statement
// from within its `row`) is prepared anew for that run.
class Statements {
public:
    explicit Statements(QString connection);
    ~Statements();
    Statements(const Statements &) = delete;
    Statements &operator=(const Statements &) = delete;
    Statements(Statements &&) = delete;
    Statements &operator=(Statements &&) = delete;

    // Runs a statement that changes something: the id of the row it added
    // (if it added one), or what the database said was wrong. A null string
    // is bound as the empty one, which the NOT NULL columns take.
    [[nodiscard]] std::expected<qint64, QString> run(const QString &sql, const QVariantList &values = {});
    // Calls `row` for each row a statement that asks something finds, in its
    // order. The values are bound as they are: a null string asks for NULL.
    void each(const QString &sql, const QVariantList &values, const std::function<void(const QSqlQuery &)> &row);
    // The first column of the first row it finds, if it finds one.
    [[nodiscard]] std::optional<QVariant> first(const QString &sql, const QVariantList &values = {});

    // Lets go of every statement: before the connection closes.
    void clear();

private:
    struct Kept;
    class Use;

    QString m_connection;
    std::unordered_map<QString, std::unique_ptr<Kept>> m_kept;
    int m_running = 0;
};
