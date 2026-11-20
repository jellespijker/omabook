// What is written by hand in a tenant, and what a relation was noted in each
// book and tenant (graph.h, ADR 0003, #109). A relation is one row however
// many books say it: its own note is that of the book it was first learned
// in (or of none), and graph_edges.notes keeps the rest,
//
//   {"books": {book's global id: note}, "tenants": {tenant's global id: note}}
//
// which TenancySchema reads for where it is, and told() for what each tenant
// reads of it. So one tenant's book, or its hand, never changes the note
// another tenant reads.

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <ranges>

#include "graph.h"
#include "graphrows.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;
using namespace GraphRows;

namespace {

[[nodiscard]] QJsonObject notesFrom(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }

[[nodiscard]] QString notesText(const QJsonObject &notes) {
    return notes.isEmpty() ? u""_s : QString::fromUtf8(QJsonDocument(notes).toJson(QJsonDocument::Compact));
}

// The notes with `said` kept for `key` among `whose` ("books" or "tenants"):
// a note said replaces the one there, and nothing said keeps it (or keeps
// that it was said there, with no note).
[[nodiscard]] QJsonObject noted(QJsonObject notes, const QString &whose, const QString &key, const QString &said) {
    QJsonObject part = notes.value(whose).toObject();
    if (!said.isEmpty() || !part.contains(key))
        part.insert(key, said);
    notes.insert(whose, part);
    return notes;
}

// Both notes, those of `theirs` added to `mine` where mine has none.
[[nodiscard]] QJsonObject merged(QJsonObject mine, const QJsonObject &theirs) {
    for (const QString &whose : {u"books"_s, u"tenants"_s}) {
        QJsonObject part = mine.value(whose).toObject();
        const QJsonObject other = theirs.value(whose).toObject();
        for (auto note = other.begin(); note != other.end(); ++note)
            if (part.value(note.key()).toString().isEmpty())
                part.insert(note.key(), note.value());
        if (!part.isEmpty())
            mine.insert(whose, part);
    }
    return mine;
}

// A relation said: where (bookId 0 for no book), with what note, by hand in
// which tenant ("" for none), and the words there.
struct Saying {
    GraphPlace place;
    QString said, tenant, anchor;
};

// The note, place, anchor and notes of a relation said again. By hand in a
// tenant, or in another book, it is noted beside what it is, which stays as
// its own book (or none) says it; in its own book, an earlier place stands.
[[nodiscard]] QVariantList again(const Library &library, const GraphEdge &edge, QJsonObject notes, const Saying &saying) {
    const qint64 bookId = saying.place.bookId;
    const bool beside = !saying.tenant.isEmpty() || (bookId > 0 && bookId != edge.learned.bookId);
    if (!saying.tenant.isEmpty())
        notes = noted(notes, u"tenants"_s, saying.tenant, saying.said);
    else if (beside)
        notes = noted(notes, u"books"_s, Spaces::bookId(library, bookId), saying.said);
    const bool earlier = !beside && bookId > 0 && saying.place.notAfter(edge.learned);
    const GraphPlace place = earlier ? saying.place : edge.learned;
    return {beside || saying.said.isEmpty() ? edge.note : saying.said,
            place.bookId,
            place.section,
            place.fraction,
            earlier ? saying.anchor : edge.anchor,
            notesText(notes)};
}

} // namespace

QString Graph::writtenIn(qint64 node) const {
    if (!m_tenant || *m_tenant == Tenant::wholeLibrary() || *m_tenant == Tenant::everyone())
        return u""_s;
    QSqlQuery query = m_library->query(u"SELECT scope FROM graph_nodes WHERE id = ?"_s, {node});
    return query.next() && query.value(0).toString().isEmpty() ? m_tenant->globalId() : u""_s;
}

qint64 Graph::link(qint64 source, qint64 target, const QString &kind, const QString &note, const std::optional<GraphPlace> &learned,
                   const QString &anchor) {
    const QString relation = kind.simplified().toLower();
    if (source == 0 || target == 0 || source == target || relation.isEmpty() || !node(source) || !node(target))
        return 0;
    const GraphPlace place = learned.value_or(GraphPlace());
    const Saying saying{place, note.trimmed(), place.bookId > 0 ? u""_s : writtenIn(source), anchorText(anchor)};
    QSqlQuery existing =
        m_library->query(QString("SELECT %1, notes FROM graph_edges WHERE source = ? AND target = ? AND kind = ?").arg(edgeColumns),
                         {source, target, relation});
    if (existing.next()) {
        const GraphEdge edge = edgeFrom(existing);
        const QJsonObject notes = notesFrom(existing.value(9).toString());
        existing.finish();
        m_library->exec("UPDATE graph_edges SET note = ?, book_id = ?, section = ?, fraction = ?, anchor = ?, notes = ? WHERE id = ?",
                        again(*m_library, edge, notes, saying) << edge.id);
        return edge.id;
    }
    existing.finish();
    const bool byHand = !saying.tenant.isEmpty();
    const QJsonObject notes = byHand ? noted({}, u"tenants"_s, saying.tenant, saying.said) : QJsonObject();
    return m_library
        ->insert("INSERT INTO graph_edges (source, target, kind, note, book_id, section, fraction, anchor, notes) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                 {source, target, relation, byHand ? u""_s : saying.said, place.bookId, place.section, place.fraction,
                  place.bookId > 0 ? saying.anchor : u""_s, notesText(notes)})
        .value_or(0);
}

void Graph::relink(const GraphEdge &edge, qint64 source, qint64 target) {
    QSqlQuery was = m_library->query(u"SELECT notes FROM graph_edges WHERE id = ?"_s, {edge.id});
    const QJsonObject theirs = was.next() ? notesFrom(was.value(0).toString()) : QJsonObject();
    was.finish();
    // As it was: of no tenant, so that its notes go along as they are.
    Graph whole(*m_library);
    const qint64 moved =
        whole.link(source, target, edge.kind, edge.note, edge.learned.bookId > 0 ? std::optional(edge.learned) : std::nullopt, edge.anchor);
    if (moved == 0 || theirs.isEmpty())
        return;
    QSqlQuery mine = m_library->query(u"SELECT notes FROM graph_edges WHERE id = ?"_s, {moved});
    const QJsonObject kept = mine.next() ? notesFrom(mine.value(0).toString()) : QJsonObject();
    mine.finish();
    m_library->exec(u"UPDATE graph_edges SET notes = ? WHERE id = ?"_s, {notesText(merged(kept, theirs)), moved});
}

void Graph::told(QList<GraphEdge> &edges) const {
    if (!m_tenant || edges.isEmpty())
        return;
    QHash<qint64, QJsonObject> notes;
    const QList<qint64> ids = edges | std::views::transform(&GraphEdge::id) | std::ranges::to<QList<qint64>>();
    for (const auto batch : ids | std::views::chunk(500))
        for (QSqlQuery query =
                 m_library->query(QString("SELECT id, notes FROM graph_edges WHERE id IN (%1) AND notes <> ''%2")
                                      .arg(marks(std::ranges::ssize(batch)), andWithin(&TenantFilter::hasEdge, u"graph_edges.id")),
                                  batch | std::ranges::to<QVariantList>());
             query.next();)
            notes.insert(query.value(0).toLongLong(), notesFrom(query.value(1).toString()));
    for (GraphEdge &edge : edges) {
        const auto found = notes.constFind(edge.id);
        if (found == notes.cend())
            continue; // its own note only, and its book is the tenant's (or it is of none)
        // Written here by hand; else its own book's, when the tenant has it;
        // else that of the first of the tenant's other books that said it.
        if (const QString hand = found->value(u"tenants"_s).toObject().value(m_tenant->globalId()).toString(); !hand.isEmpty()) {
            edge.note = hand;
            continue;
        }
        if (!edge.note.isEmpty() && (edge.learned.bookId == 0 || sees(edge.learned.bookId)))
            continue;
        const QJsonObject books = found->value(u"books"_s).toObject();
        QStringList said = books.keys();
        std::ranges::sort(said);
        const auto theirs = std::ranges::find_if(said, [&](const QString &book) {
            return !books.value(book).toString().isEmpty() && sees(Spaces::localId(*m_library, u"book"_s, book));
        });
        edge.note = theirs == said.end() ? u""_s : books.value(*theirs).toString();
    }
}

QJsonObject Graph::madeIn(QJsonObject data, qint64 id, const QString &scope, const std::optional<GraphPlace> &seen) const {
    // Made by hand in a tenant, met in no book, it is that tenant's: the
    // whole library's is every tenant's, and a world keeps a scope of its own.
    if (!scope.isEmpty() || (seen && seen->bookId > 0) || !m_tenant || *m_tenant == Tenant::wholeLibrary())
        return data;
    const auto known = Graph(*m_library).node(id);
    QStringList tenants = known ? known->data.value(Tenancy::madeInKey()).toVariant().toStringList() : QStringList();
    if (!tenants.contains(m_tenant->globalId()))
        data.insert(Tenancy::madeInKey(), QJsonArray::fromStringList(tenants << m_tenant->globalId()));
    return data;
}
