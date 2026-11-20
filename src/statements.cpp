#include "statements.h"

#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <utility>

namespace {

// More statements than this, most of them made up of values, are let go of
// together, so a library open for days keeps only what it runs often.
constexpr std::size_t keptAtMost = 256;

// A null QString binds as SQL NULL, which the NOT NULL columns refuse; a book
// with no series has the empty string for one.
[[nodiscard]] QVariant bindable(const QVariant &value) {
    return value.typeId() == QMetaType::QString && value.toString().isNull() ? QVariant(QString("")) : value;
}

} // namespace

struct Statements::Kept {
    explicit Kept(QSqlQuery made) : query(std::move(made)) {}
    QSqlQuery query;
    bool busy = false;
};

// One run of a statement: the kept one, or one of its own while that one is
// running still; reset when the run is done.
class Statements::Use {
public:
    Use(Statements &statements, const QString &sql, const QVariantList &values, bool writes) : m_statements(statements) {
        ++m_statements.m_running;
        const auto found = m_statements.m_kept.find(sql);
        if (found != m_statements.m_kept.end() && !found->second->busy) {
            m_kept = found->second.get();
        } else {
            QSqlQuery made(QSqlDatabase::database(m_statements.m_connection));
            made.setForwardOnly(true);
            m_prepared = made.prepare(sql);
            if (m_prepared && found == m_statements.m_kept.end()) {
                // Only this run is under way, so none of those let go of is in use.
                if (m_statements.m_kept.size() >= keptAtMost && m_statements.m_running == 1)
                    m_statements.m_kept.clear();
                m_kept = m_statements.m_kept.emplace(sql, std::make_unique<Kept>(std::move(made))).first->second.get();
            } else {
                m_query = &m_own.emplace(std::move(made));
            }
        }
        if (m_kept) {
            m_kept->busy = true;
            m_query = &m_kept->query;
        }
        for (qsizetype at = 0; at < values.size(); ++at)
            query().bindValue(static_cast<int>(at), writes ? bindable(values.at(at)) : values.at(at));
    }
    ~Use() {
        query().finish();
        if (m_kept)
            m_kept->busy = false;
        --m_statements.m_running;
    }
    Use(const Use &) = delete;
    Use &operator=(const Use &) = delete;
    Use(Use &&) = delete;
    Use &operator=(Use &&) = delete;

    [[nodiscard]] QSqlQuery &query() { return *m_query; }
    // Whether it ran, and found what it asks for or did what it says.
    [[nodiscard]] bool exec() { return m_prepared && query().exec(); }

private:
    Statements &m_statements;
    Kept *m_kept = nullptr;
    std::optional<QSqlQuery> m_own;
    QSqlQuery *m_query = nullptr; // the kept one's, or m_own's
    bool m_prepared = true;
};

Statements::Statements(QString connection) : m_connection(std::move(connection)) {}

Statements::~Statements() = default;

std::expected<qint64, QString> Statements::run(const QString &sql, const QVariantList &values) {
    Use use(*this, sql, values, true);
    if (!use.exec())
        return std::unexpected(use.query().lastError().text());
    return use.query().lastInsertId().toLongLong();
}

void Statements::each(const QString &sql, const QVariantList &values, const std::function<void(const QSqlQuery &)> &row) {
    Use use(*this, sql, values, false);
    if (!use.exec())
        return;
    while (use.query().next())
        row(use.query());
}

std::optional<QVariant> Statements::first(const QString &sql, const QVariantList &values) {
    Use use(*this, sql, values, false);
    if (!use.exec() || !use.query().next())
        return std::nullopt;
    return use.query().value(0);
}

void Statements::clear() { m_kept.clear(); }
