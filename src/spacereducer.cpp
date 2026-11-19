#include "spacereducer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <tuple>
#include <utility>

#include "hlc.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QJsonObject parseObject(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

void takeMax(SpaceStamp &target, const SpaceStamp &candidate) {
    if (candidate > target)
        target = candidate;
}

void foldCollection(ReducedCollection &col, const SpaceOperation &op) {
    if (op.field == "removed"_L1) {
        if (op.value == "1"_L1)
            takeMax(col.removed, {op.hlc, op.device, 0, op.value});
        else
            takeMax(col.alive, {op.hlc, op.device, 1, op.value});
        return;
    }
    takeMax(col.alive, {op.hlc, op.device, 1, op.value});
    if (op.field.startsWith("book:"_L1)) {
        const QString bookGid = op.field.mid(5);
        int prio = 0;
        if (op.value == "in"_L1)
            prio = 2;
        else if (op.value == "out"_L1)
            prio = 1;
        takeMax(col.books[bookGid], {op.hlc, op.device, prio, op.value});
        return;
    }
    takeMax(col.fields[op.field], {op.hlc, op.device, 1, op.value});
}

void foldNode(ReducedNode &node, const SpaceOperation &op) {
    if (op.field == "removed"_L1) {
        if (op.value == "1"_L1)
            takeMax(node.removed, {op.hlc, op.device, 0, op.value});
        else
            takeMax(node.alive, {op.hlc, op.device, 1, op.value});
        return;
    }
    takeMax(node.alive, {op.hlc, op.device, 1, op.value});
    if (op.field.startsWith("seen:"_L1)) {
        const QString bookGid = op.field.mid(5);
        const QJsonObject obj = parseObject(op.value);
        const SpaceMoment candidate{obj.value("section").toInt(), obj.value("fraction").toDouble(), obj.value("anchor").toString()};
        if (!node.seen.contains(bookGid) || candidate.earlierThan(node.seen.value(bookGid)))
            node.seen.insert(bookGid, candidate);
        return;
    }
    if (op.field.startsWith("alias:"_L1)) {
        const QString alias = op.field.mid(6).trimmed();
        const int prio = (op.value != "0"_L1 && !op.value.isEmpty()) ? 2 : 0;
        if (!alias.isEmpty())
            takeMax(node.aliases[alias], {op.hlc, op.device, prio, prio > 0 ? u"1"_s : u"0"_s});
        return;
    }
    takeMax(node.fields[op.field], {op.hlc, op.device, 1, op.value});
}

void foldFact(ReducedFact &fact, const SpaceOperation &op) {
    if (op.field == "removed"_L1 && op.value == "1"_L1) {
        takeMax(fact.removed, {op.hlc, op.device, 0, op.value});
        return;
    }
    if (op.field != "revealed"_L1)
        return;
    takeMax(fact.alive, {op.hlc, op.device, 1, op.value});
    const QJsonObject obj = parseObject(op.value);
    fact.nodeGid = obj.value("node").toString();
    fact.kind = obj.value("kind").toString();
    fact.value = obj.value("value").toString();
    fact.bookGid = obj.value("book").toString();
    const SpaceMoment candidate{obj.value("section").toInt(), obj.value("fraction").toDouble(), obj.value("anchor").toString()};
    if (!fact.hasMoment || (!fact.bookGid.isEmpty() && candidate.earlierThan(fact.moment))) {
        fact.moment = candidate;
        fact.hasMoment = true;
    }
}

void foldEdge(ReducedEdge &edge, const SpaceOperation &op) {
    if (op.field == "removed"_L1 && op.value == "1"_L1) {
        takeMax(edge.removed, {op.hlc, op.device, 0, op.value});
        return;
    }
    if (op.field != "link"_L1)
        return;
    takeMax(edge.alive, {op.hlc, op.device, 1, op.value});
    const QJsonObject obj = parseObject(op.value);
    edge.sourceGid = obj.value("source").toString();
    edge.targetGid = obj.value("target").toString();
    edge.kind = obj.value("kind").toString();
    if (const QString note = obj.value("note").toString(); !note.isEmpty() || edge.note.hlc < 0)
        takeMax(edge.note, {op.hlc, op.device, note.isEmpty() ? 0 : 1, note});
    const QString bookGid = obj.value("book").toString();
    const SpaceMoment candidate{obj.value("section").toInt(), obj.value("fraction").toDouble(), obj.value("anchor").toString()};
    if (!bookGid.isEmpty() && (!edge.hasLearned || edge.bookGid.isEmpty() || candidate.earlierThan(edge.learned))) {
        edge.bookGid = bookGid;
        edge.learned = candidate;
        edge.hasLearned = true;
    }
}

void foldDay(ReducedDay &day, const SpaceOperation &op) {
    if (op.field == "removed"_L1 && op.value == "1"_L1) {
        takeMax(day.removed, {op.hlc, op.device, 0, op.value});
    } else if (op.field == "range"_L1) {
        takeMax(day.alive, {op.hlc, op.device, 1, op.value});
        const QJsonObject obj = parseObject(op.value);
        day.bookGid = obj.value("book").toString();
        day.day = obj.value("day").toString();
        day.from = std::min(day.from, obj.value("from").toDouble());
        day.to = std::max(day.to, obj.value("to").toDouble());
    } else {
        return;
    }
    day.valid = !day.bookGid.isEmpty() && !day.day.isEmpty() && !SpaceReducer::isRemoved(day.alive, day.removed);
}

} // namespace

bool SpaceMoment::earlierThan(const SpaceMoment &other) const noexcept {
    return std::tie(section, fraction) < std::tie(other.section, other.fraction)
           || (std::tie(section, fraction) == std::tie(other.section, other.fraction) && anchor > other.anchor);
}

bool SpaceReducer::isRemoved(const SpaceStamp &alive, const SpaceStamp &removed) noexcept {
    if (removed.hlc < 0)
        return false;
    if (alive.hlc < 0)
        return true;
    // A tie of the same device is one write that removed it, as a baseline
    // writes a removed collection's name and its removal at the time it has.
    return std::tie(removed.hlc, removed.device) >= std::tie(alive.hlc, alive.device);
}

void SpaceReducer::foldBook(ReducedBook &book, const SpaceOperation &op) {
    if (op.field == "removed"_L1) {
        if (op.value != "0"_L1 && !op.value.isEmpty())
            takeMax(book.removed, {op.hlc, op.device, 0, op.value});
        else
            takeMax(book.alive, {op.hlc, op.device, 1, op.value});
        return;
    }
    if (op.field == "removed_at"_L1) {
        takeMax(book.fields[op.field], {op.hlc, op.device, 1, op.value});
        return;
    }
    takeMax(book.alive, {op.hlc, op.device, 1, op.value});
    if (op.field.startsWith("tag:"_L1)) {
        const QString tag = op.field.mid(4).trimmed();
        const int prio = (op.value != "0"_L1 && !op.value.isEmpty()) ? 2 : 0;
        if (!tag.isEmpty())
            takeMax(book.tagSet[tag], {op.hlc, op.device, prio, prio > 0 ? u"1"_s : u"0"_s});
        return;
    }
    takeMax(book.fields[op.field], {op.hlc, op.device, 1, op.value});
}

namespace {

[[nodiscard]] qint64 stampedHlc(const ReducedBook &book, qint64 raw, const QString &stampField) {
    if (raw < 0)
        return 0;
    const auto stamp = book.fields.constFind(stampField);
    if (stamp != book.fields.cend() && stamp->hlc >= raw)
        return Hlc::normalize(stamp->value.toLongLong());
    return Hlc::normalize(raw);
}

[[nodiscard]] bool isReadingField(const QString &field) {
    return field == "shelf"_L1 || field == "rating"_L1 || field == "finished"_L1 || field == "paper"_L1;
}

[[nodiscard]] qint64 rawFieldHlc(const ReducedBook &book, const QString &field) {
    qint64 raw = -1;
    if (const auto it = book.fields.constFind(field); it != book.fields.cend())
        raw = it->hlc;
    if (field == "tags"_L1) {
        for (auto it = book.tagSet.cbegin(); it != book.tagSet.cend(); ++it)
            raw = std::max(raw, it.value().hlc);
    }
    return raw;
}

} // namespace

qint64 SpaceReducer::fieldHlc(const ReducedBook &book, const QString &field) {
    if (field == "progress"_L1) {
        const auto it = book.fields.constFind(field);
        if (it == book.fields.cend() || it->value.isEmpty())
            return 0;
        return Hlc::normalize(parseObject(it->value).value("hlc").toInteger(it->hlc));
    }
    if (field == "removed"_L1)
        return stampedHlc(book, book.removed.hlc, u"removed_at"_s);
    return stampedHlc(book, rawFieldHlc(book, field), isReadingField(field) ? u"reading_at"_s : u"details_at"_s);
}

qint64 SpaceReducer::detailsHlc(const ReducedBook &book) {
    static constexpr std::array detailKeys{"title"_L1,       "authors"_L1,   "series"_L1,    "series_index"_L1, "publisher"_L1,
                                           "language"_L1,    "isbn"_L1,      "published"_L1, "description"_L1,  "tags"_L1,
                                           "identifiers"_L1, "has_cover"_L1, "cover_hash"_L1};
    qint64 best = 0;
    for (const QLatin1StringView key : detailKeys)
        best = std::max(best, fieldHlc(book, QString(key)));
    return best;
}

qint64 SpaceReducer::readingHlc(const ReducedBook &book) {
    static constexpr std::array readingKeys{"shelf"_L1, "rating"_L1, "finished"_L1, "paper"_L1};
    qint64 best = 0;
    for (const QLatin1StringView key : readingKeys)
        best = std::max(best, fieldHlc(book, QString(key)));
    return best;
}

namespace {

[[nodiscard]] bool isSameAs(const SpaceOperation &op) {
    return op.field == "same_as"_L1 && (op.entity == "book"_L1 || op.entity == "node"_L1);
}

// The id a group of linked ids stands for, shortening the path on the way.
[[nodiscard]] QString rootOf(QHash<QString, QString> &parent, const QString &id) {
    QString root = id;
    while (parent.contains(root) && parent.value(root) != root)
        root = parent.value(root);
    for (QString at = id; at != root;)
        at = std::exchange(parent[at], root);
    return root;
}

} // namespace

QHash<QString, QString> SpaceReducer::sameAs(std::span<const SpaceOperation> ops, QLatin1StringView entity) {
    // Of two groups that are joined, the smaller root stays the root, so the
    // root of every group is its smallest id, whatever the order of the links.
    QHash<QString, QString> parent;
    for (const SpaceOperation &op : ops) {
        if (!isSameAs(op) || op.entity != entity || op.id.isEmpty() || op.value.isEmpty() || op.id == op.value)
            continue;
        const QString one = rootOf(parent, op.id);
        const QString other = rootOf(parent, op.value);
        parent.insert(std::max(one, other), std::min(one, other));
        parent.insert(std::min(one, other), std::min(one, other));
    }
    QHash<QString, QString> canonical;
    for (const QString &id : parent.keys())
        canonical.insert(id, rootOf(parent, id));
    return canonical;
}

bool SpaceReducer::grows(const QString &field) {
    return field.startsWith("seen:"_L1) || field == "revealed"_L1 || field == "link"_L1 || field == "range"_L1 || field == "same_as"_L1;
}

namespace {

ReducedState reduceOf(std::span<const SpaceOperation> ops, std::span<const SpaceOperation> links, const QString &device) {
    ReducedState state;
    const QHash<QString, QString> canonical = SpaceReducer::sameAs(links);
    const QHash<QString, QString> things = SpaceReducer::sameAs(links, "node"_L1);
    for (const SpaceOperation &op : ops) {
        if (isSameAs(op) || (!device.isEmpty() && op.device != device))
            continue;
        if (op.entity == "book"_L1)
            SpaceReducer::foldBook(state.books[canonical.value(op.id, op.id)], op);
        else if (op.entity == "collection"_L1)
            foldCollection(state.collections[op.id], op);
        else if (op.entity == "node"_L1)
            foldNode(state.nodes[things.value(op.id, op.id)], op);
        else if (op.entity == "fact"_L1)
            foldFact(state.facts[op.id], op);
        else if (op.entity == "edge"_L1)
            foldEdge(state.edges[op.id], op);
        else if (op.entity == "reading_log"_L1)
            foldDay(state.days[op.id], op);
    }
    return state;
}

} // namespace

ReducedState SpaceReducer::reduce(std::span<const SpaceOperation> ops) { return reduceOf(ops, ops, {}); }

ReducedState SpaceReducer::reduce(std::span<const SpaceOperation> ops, const QString &device) {
    // What its rows are: of each field the last it wrote, whatever its stamp
    // (one taken from elsewhere keeps the stamp it had there); of what only
    // grows, all of it.
    QHash<QString, qsizetype> last;
    QList<SpaceOperation> written;
    for (const SpaceOperation &op : ops) {
        if (op.device != device)
            continue;
        if (SpaceReducer::grows(op.field)) {
            written << op;
            continue;
        }
        const QString key = op.entity + u'\x1f' + op.id + u'\x1f' + op.field;
        const auto at = last.constFind(key);
        if (at == last.cend()) {
            last.insert(key, written.size());
            written << op;
            continue;
        }
        // In the order it wrote them: by sequence in one space, else by stamp.
        const SpaceOperation &before = written.at(*at);
        if (op.space == before.space ? op.seq > before.seq : op.hlc > before.hlc)
            written[*at] = op;
    }
    return reduceOf(written, ops, device);
}
