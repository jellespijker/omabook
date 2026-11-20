#include "graphindex.h"

#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <ranges>

#include "changelog.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString trigger(const QString &name, const QString &when, const QStringList &body) {
    return u"CREATE TRIGGER IF NOT EXISTS %1 %2 BEGIN %3; END"_s.arg(name, when, body.join(u"; "_s));
}

// The facts whose words the index holds as said of a thing: the summaries
// it was given (format 31), and since format 33 the names a tenant gave it
// by hand too (#110).
constexpr auto summaries = "'summary'"_L1;
constexpr auto summariesAndNames = "'summary', 'name'"_L1;

// A thing's words as the index holds them, put in again whole: FTS5 keeps no
// copy of what it was given, so a row is replaced, never changed in part.
// The things that meet `where` (of graph_nodes AS n), or one by its id.
[[nodiscard]] QString wordsWhere(const QString &where, QLatin1StringView said) {
    return u"INSERT OR REPLACE INTO graph_words (rowid, name, summary, data, said) SELECT n.id, n.name, n.summary, n.data, "
           "COALESCE((SELECT group_concat(f.value, ' ') FROM graph_facts AS f WHERE f.node_id = n.id AND f.kind IN (%2)), '') "
           "FROM graph_nodes AS n WHERE %1"_s.arg(where, said);
}
[[nodiscard]] QString wordsOf(const QString &node, QLatin1StringView said = summaries) { return wordsWhere(u"n.id = "_s + node, said); }

// The triggers that keep a thing's words as it and its facts are written,
// with what of its facts is said of it.
[[nodiscard]] QStringList wordTriggers(QLatin1StringView said) {
    const auto words = [said](const QString &node) { return wordsOf(node, said); };
    return {
        trigger(u"graph_nodes_words_insert"_s, u"AFTER INSERT ON graph_nodes"_s, {words(u"NEW.id"_s)}),
        trigger(u"graph_nodes_words_update"_s, u"AFTER UPDATE OF name, summary, data ON graph_nodes"_s, {words(u"NEW.id"_s)}),
        trigger(u"graph_nodes_words_delete"_s, u"AFTER DELETE ON graph_nodes"_s, {u"DELETE FROM graph_words WHERE rowid = OLD.id"_s}),
        trigger(u"graph_facts_words_insert"_s, u"AFTER INSERT ON graph_facts WHEN NEW.kind IN (%1)"_s.arg(said), {words(u"NEW.node_id"_s)}),
        trigger(u"graph_facts_words_update"_s, u"AFTER UPDATE OF node_id, kind, value ON graph_facts"_s,
                {words(u"OLD.node_id"_s), words(u"NEW.node_id"_s)}),
        trigger(u"graph_facts_words_delete"_s, u"AFTER DELETE ON graph_facts WHEN OLD.kind IN (%1)"_s.arg(said), {words(u"OLD.node_id"_s)}),
    };
}

// A thing touched now, at the graph counter as it is.
[[nodiscard]] QString touch(const QString &node) {
    return u"INSERT OR REPLACE INTO graph_touched (node_id, at) SELECT %1, value FROM library_state WHERE key = 'graph' "
           "AND %1 IS NOT NULL"_s.arg(node);
}

// While Tenancy fills its tables whole the triggers that follow one row at a
// time stand aside (TenancySchema), and so do these: the refill is counted
// when it is done (tenancy_refilled).
[[nodiscard]] QString notBulk() { return u"WHEN (SELECT value FROM library_state WHERE key = 'tenancy_bulk') = 0"_s; }

// The triggers that touch what a row of `table` is of, on each of its writes;
// `ends(row)` the things a row (NEW or OLD) is of.
void touching(QStringList &all, const QString &table, const auto &ends, const QString &when = {}) {
    const auto of = [&ends](const QString &row) { return ends(row) | std::views::transform(touch) | std::ranges::to<QStringList>(); };
    const QString suffix = when.isEmpty() ? QString() : u" "_s + when;
    all << trigger(table + u"_touched_insert"_s, u"AFTER INSERT ON "_s + table + suffix, of(u"NEW"_s));
    if (when.isEmpty())
        all << trigger(table + u"_touched_update"_s, u"AFTER UPDATE ON "_s + table, of(u"OLD"_s) + of(u"NEW"_s));
    all << trigger(table + u"_touched_delete"_s, u"AFTER DELETE ON "_s + table + suffix, of(u"OLD"_s));
}

[[nodiscard]] QStringList statements() {
    QStringList all{
        // Contentless (the rows are graph_nodes'), and deletable by row.
        u"CREATE VIRTUAL TABLE IF NOT EXISTS graph_words USING fts5(name, summary, data, said, content='', contentless_delete=1, "
        "tokenize=\"unicode61 remove_diacritics 2 categories 'L* N* Co M*'\", prefix='2 3')"_s,
        u"INSERT INTO graph_words (rowid, name, summary, data, said) SELECT n.id, n.name, n.summary, n.data, COALESCE(s.said, '') "
        "FROM graph_nodes AS n LEFT JOIN (SELECT node_id, group_concat(value, ' ') AS said FROM graph_facts WHERE kind = 'summary' "
        "GROUP BY node_id) AS s ON s.node_id = n.id"_s,
        u"CREATE TABLE IF NOT EXISTS graph_touched (node_id INTEGER PRIMARY KEY, at INTEGER NOT NULL)"_s,
        u"CREATE INDEX IF NOT EXISTS graph_touched_at ON graph_touched(at)"_s,
        u"INSERT OR IGNORE INTO library_state (key, value) VALUES ('tenancy_refills', 0)"_s,
        trigger(u"tenancy_refilled"_s,
                u"AFTER UPDATE OF value ON library_state WHEN NEW.key = 'tenancy_bulk' AND NEW.value = 0 AND OLD.value <> 0"_s,
                {u"UPDATE library_state SET value = value + 1 WHERE key = 'tenancy_refills'"_s}),
    };
    all << wordTriggers(summaries);
    touching(all, u"graph_nodes"_s, [](const QString &row) { return QStringList{row + u".id"_s}; });
    touching(all, u"graph_seen"_s, [](const QString &row) { return QStringList{row + u".node_id"_s}; });
    touching(all, u"graph_facts"_s, [](const QString &row) { return QStringList{row + u".node_id"_s}; });
    touching(all, u"graph_edges"_s, [](const QString &row) { return QStringList{row + u".source"_s, row + u".target"_s}; });
    // Which tenants see a thing, a fact or a relation, one row at a time.
    touching(all, u"tenant_nodes"_s, [](const QString &row) { return QStringList{row + u".node_id"_s}; }, notBulk());
    touching(
        all, u"tenant_facts"_s,
        [](const QString &row) { return QStringList{u"(SELECT node_id FROM graph_facts WHERE id = %1.fact_id)"_s.arg(row)}; }, notBulk());
    touching(
        all, u"tenant_edges"_s,
        [](const QString &row) {
            return QStringList{u"(SELECT source FROM graph_edges WHERE id = %1.edge_id)"_s.arg(row),
                               u"(SELECT target FROM graph_edges WHERE id = %1.edge_id)"_s.arg(row)};
        },
        notBulk());
    return all;
}

// What the tokenizer takes for part of a word: letters, digits, marks and
// characters of private use.
[[nodiscard]] bool inWord(QChar c) { return c.isLetterOrNumber() || c.isMark() || c.category() == QChar::Other_PrivateUse; }

// The words of a text, folded as unicode61 with remove_diacritics folds them:
// the accents of the Latin letters taken off, and the case folded.
[[nodiscard]] QStringList wordsIn(const QString &text) {
    QStringList words;
    QString word;
    for (const QChar c : text.normalized(QString::NormalizationForm_D)) {
        if (c.unicode() >= 0x0300 && c.unicode() <= 0x036f)
            continue; // a combining accent
        if (inWord(c)) {
            word += c;
        } else if (!word.isEmpty()) {
            words << word.toCaseFolded();
            word.clear();
        }
    }
    if (!word.isEmpty())
        words << word.toCaseFolded();
    return words;
}

} // namespace

const QStringList &GraphIndex::step() {
    static const QStringList made = statements();
    return made;
}

const QStringList &GraphIndex::names() {
    static const QStringList made = [] {
        QStringList all;
        for (const auto name : {"graph_nodes_words_insert"_L1, "graph_nodes_words_update"_L1, "graph_nodes_words_delete"_L1,
                                "graph_facts_words_insert"_L1, "graph_facts_words_update"_L1, "graph_facts_words_delete"_L1})
            all << u"DROP TRIGGER IF EXISTS "_s + name;
        all << wordTriggers(summariesAndNames);
        // A name given in a tenant that came by the log before this step.
        all << wordsWhere(u"n.id IN (SELECT node_id FROM graph_facts WHERE kind = 'name')"_s, summariesAndNames);
        // The things a tenant calls by a name looked up by it (Graph::named).
        all << u"CREATE INDEX IF NOT EXISTS graph_facts_names ON graph_facts(value COLLATE NOCASE) WHERE kind = 'name'"_s;
        return all;
    }();
    return made;
}

std::optional<QString> GraphIndex::phrase(const QString &word) {
    if (std::ranges::none_of(word, inWord))
        return std::nullopt;
    return u"\""_s + QString(word).replace(u"\""_s, u"\"\""_s) + u"\"*"_s;
}

bool GraphIndex::holds(const QString &text, const QStringList &words) {
    const QStringList in = wordsIn(text);
    return std::ranges::all_of(words, [&in](const QString &word) {
        const QStringList wanted = wordsIn(word);
        if (wanted.isEmpty())
            return true; // the index has no word for it, and the search leaves it out
        for (qsizetype at = 0; at + wanted.size() <= in.size(); ++at) {
            const auto whole = [&](qsizetype i) { return in.at(at + i) == wanted.at(i); };
            const qsizetype last = wanted.size() - 1;
            if (std::ranges::all_of(std::views::iota(qsizetype{0}, last), whole) && in.at(at + last).startsWith(wanted.at(last)))
                return true;
        }
        return false;
    });
}

QSet<qint64> GraphIndex::touchedSince(const Library &library, qint64 since) {
    QSet<qint64> touched;
    for (QSqlQuery query = ChangeLog::query(library, u"SELECT node_id FROM graph_touched WHERE at >= ?"_s, {since}); query.next();)
        touched << query.value(0).toLongLong();
    return touched;
}

qint64 GraphIndex::tenancyRefills(const Library &library) {
    QSqlQuery query = ChangeLog::query(library, u"SELECT value FROM library_state WHERE key = 'tenancy_refills'"_s);
    return query.next() ? query.value(0).toLongLong() : 0;
}
