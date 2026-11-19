#include "publishedspace.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <tuple>
#include <utility>

#include "changelog.h"
#include "compaction.h"
#include "graphcontext.h"
#include "library.h"
#include "publishedlog.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// A book's identity: what finds it in another library, or in a shop.
constexpr std::array identityFields{"title"_L1, "authors"_L1, "series"_L1, "series_index"_L1, "isbn"_L1, "identifiers"_L1};
constexpr std::array readingFields{"progress"_L1, "shelf"_L1, "finished"_L1};
// A collection's name, where it sits, what it selects and how its shelf shows it.
constexpr std::array collectionFields{"name"_L1, "parent"_L1, "rule"_L1, "shelf"_L1};

template <std::size_t N> [[nodiscard]] bool among(const std::array<QLatin1StringView, N> &fields, const QString &field) {
    return std::ranges::any_of(fields, [&field](QLatin1StringView known) { return field == known; });
}

// Ids as the log's same-as links make them one.
struct Canon {
    QHash<QString, QString> books;
    QHash<QString, QString> things;

    [[nodiscard]] QString book(const QString &gid) const { return books.value(gid, gid); }
    [[nodiscard]] QString thing(const QString &gid) const { return things.value(gid, gid); }
};

// What of the owner's log belongs to the space, by global id.
struct Members {
    QString root;                  // the collection published
    QSet<QString> collections;     // it and every collection inside it
    QSet<QString> books;           // the books it holds, by the id their links stand for
    QSet<QString> nodes;           // by the id their links stand for
    QHash<QString, QString> kinds; // of each of those, its kind
    QSet<QString> facts;
    QSet<QString> edges;
    QSet<QString> days;
    // How far the owner has read each of its books (GraphContext::reachOf).
    QHash<QString, GraphContext::Reach> reach;

    [[nodiscard]] bool holds(const Canon &canon, const QString &bookGid) const {
        return !bookGid.isEmpty() && books.contains(canon.book(bookGid));
    }
    // Whether the owner has met a place of a book: nothing past where they
    // got in it, nothing of one not begun, all of one read. A place of no
    // book, or of a book the space doesn't hold, is not theirs to bound.
    [[nodiscard]] bool met(const Canon &canon, const QString &bookGid, int section, double fraction) const {
        if (!holds(canon, bookGid))
            return true;
        return reach.value(canon.book(bookGid)).covers(GraphPlace{.bookId = 0, .section = section, .fraction = fraction});
    }
    [[nodiscard]] bool met(const Canon &canon, const QString &bookGid, const SpaceMoment &moment) const {
        return met(canon, bookGid, moment.section, moment.fraction);
    }
};

template <typename Thing> [[nodiscard]] bool alive(const Thing &thing) { return !SpaceReducer::isRemoved(thing.alive, thing.removed); }

[[nodiscard]] std::optional<Collection> collectionOf(const Library &library, qint64 collectionId) {
    for (const Collection &collection : library.collections())
        if (collection.id == collectionId)
            return collection;
    return std::nullopt;
}

void collectTree(const Library &library, const Collection &root, const Canon &canon, Members &members) {
    const QList<Collection> all = library.collections();
    QSet<qint64> tree{root.id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const Collection &collection : all)
            if (!tree.contains(collection.id) && tree.contains(collection.parent)) {
                tree.insert(collection.id);
                grew = true;
            }
    }
    members.root = Spaces::collectionId(library, root.id);
    for (const qint64 id : std::as_const(tree))
        members.collections.insert(Spaces::collectionId(library, id));
    for (const Book &book : root.of(library.books())) {
        const QString gid = canon.book(Spaces::bookId(library, book.id));
        members.books.insert(gid);
        members.reach.insert(gid, GraphContext::reachOf(library, book.id));
    }
}

// Whether a thing was met in one of the space's books, where the owner has read.
[[nodiscard]] bool metInBooks(const ReducedNode &node, const Canon &canon, const Members &members) {
    for (auto seen = node.seen.cbegin(); seen != node.seen.cend(); ++seen)
        if (members.holds(canon, seen.key()) && members.met(canon, seen.key(), *seen))
            return true;
    return false;
}

// Whether a relation was learned where the owner has read, or in no book of the space.
[[nodiscard]] bool learned(const ReducedEdge &edge, const Canon &canon, const Members &members) {
    return !edge.hasLearned || members.met(canon, edge.bookGid, edge.learned);
}

// The things of the graph its books hold: met in one of them, learned of
// there, related there, a book's own, or related to one; questions apart.
struct GraphOfBooks {
    QSet<QString> things; // alive, of the library's own graph (not a story world's)
    QHash<QString, QString> kinds;
    QSet<QString> bookNodes; // the books' own things
    QSet<QString> questions; // things that are a question asked
    QSet<QString> met;
};

// The things of the library's own graph, and of those which its books hold
// themselves (a book's own thing, one met in a book).
void readThings(const ReducedState &state, const Canon &canon, const Members &members, GraphOfBooks &graph) {
    for (auto node = state.nodes.cbegin(); node != state.nodes.cend(); ++node) {
        const QString kind = node->fields.value(u"kind"_s).value;
        // A tag is the owner's own word for a book, which the space leaves out.
        if (!alive(*node) || !node->fields.value(u"scope"_s).value.isEmpty() || kind == "tag"_L1)
            continue;
        graph.things.insert(node.key());
        graph.kinds.insert(node.key(), kind);
        if (members.holds(canon, node->fields.value(u"book"_s).value))
            graph.bookNodes.insert(node.key());
        if (kind == "question"_L1)
            graph.questions.insert(node.key());
        if (metInBooks(*node, canon, members))
            graph.met.insert(node.key());
    }
}

// What was learned in the books, or is related there or to a book's own thing.
void readRelations(const ReducedState &state, const Canon &canon, const Members &members, GraphOfBooks &graph) {
    for (const ReducedFact &fact : state.facts)
        if (alive(fact) && members.holds(canon, fact.bookGid) && members.met(canon, fact.bookGid, fact.moment))
            graph.met.insert(canon.thing(fact.nodeGid));
    for (const ReducedEdge &edge : state.edges) {
        if (!alive(edge) || !learned(edge, canon, members))
            continue;
        const QString source = canon.thing(edge.sourceGid);
        const QString target = canon.thing(edge.targetGid);
        const bool inBooks = members.holds(canon, edge.bookGid);
        if (inBooks || graph.bookNodes.contains(target))
            graph.met.insert(source);
        if (inBooks || graph.bookNodes.contains(source))
            graph.met.insert(target);
    }
}

// The things the parts take: of the graph, its things; of the questions, the
// questions and what they were asked of.
void chooseThings(const GraphOfBooks &graph, const PublishedSpace::Parts &parts, Members &members) {
    for (const QString &thing : graph.met + graph.bookNodes)
        if (graph.things.contains(thing) && (graph.questions.contains(thing) ? parts.questions : parts.graph))
            members.nodes.insert(thing);
    if (parts.questions)
        members.nodes += graph.bookNodes;
    for (const QString &thing : std::as_const(members.nodes))
        members.kinds.insert(thing, graph.kinds.value(thing));
}

// Of the things taken, what is known of them and how they relate, in the
// space's books (where the owner has read) or in none.
void chooseFactsAndRelations(const ReducedState &state, const Canon &canon, Members &members) {
    const auto ofBooks = [&](const QString &book) { return book.isEmpty() || members.holds(canon, book); };
    for (auto fact = state.facts.cbegin(); fact != state.facts.cend(); ++fact)
        if (alive(*fact) && members.nodes.contains(canon.thing(fact->nodeGid)) && ofBooks(fact->bookGid)
            && members.met(canon, fact->bookGid, fact->moment))
            members.facts.insert(fact.key());
    for (auto edge = state.edges.cbegin(); edge != state.edges.cend(); ++edge) {
        const bool between = members.nodes.contains(canon.thing(edge->sourceGid)) && members.nodes.contains(canon.thing(edge->targetGid));
        if (alive(*edge) && between && ofBooks(edge->bookGid) && learned(*edge, canon, members))
            members.edges.insert(edge.key());
    }
}

void collectGraph(const ReducedState &state, const Canon &canon, const PublishedSpace::Parts &parts, Members &members) {
    if (!parts.graph && !parts.questions)
        return;
    GraphOfBooks graph;
    readThings(state, canon, members, graph);
    readRelations(state, canon, members, graph);
    chooseThings(graph, parts, members);
    chooseFactsAndRelations(state, canon, members);
}

void collectDays(const ReducedState &state, const Canon &canon, Members &members) {
    for (auto day = state.days.cbegin(); day != state.days.cend(); ++day)
        if (day->valid && members.holds(canon, day->bookGid))
            members.days.insert(day.key());
}

// Where an operation of the graph says something was met or learned: its
// book (of a sighting, the field's) and the place in it.
struct Place {
    QString book;
    int section = 0;
    double fraction = 0;
};

[[nodiscard]] Place placeIn(const QString &json, const QString &book = {}) {
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    return {.book = book.isEmpty() ? object.value("book").toString() : book,
            .section = object.value("section").toInt(),
            .fraction = object.value("fraction").toDouble()};
}

[[nodiscard]] bool metAt(const Members &members, const Canon &canon, const Place &place) {
    return members.met(canon, place.book, place.section, place.fraction);
}

[[nodiscard]] bool bookFieldIn(const SpaceOperation &op, const PublishedSpace::Parts &parts) {
    return among(identityFields, op.field) || op.field == "same_as"_L1 || (parts.ratings && op.field == "rating"_L1)
           || (parts.reading && among(readingFields, op.field));
}

[[nodiscard]] bool nodeFieldIn(const SpaceOperation &op, const Members &members, const Canon &canon) {
    // Its summary is the latest it was given, wherever in a book that was;
    // each summary it had is a fact, with the place it was learned at.
    if (op.field == "removed"_L1 || op.field == "summary"_L1)
        return false;
    if (op.field.startsWith("seen:"_L1))
        return members.holds(canon, op.field.mid(5)) && metAt(members, canon, placeIn(op.value, op.field.mid(5)));
    return op.field != "book"_L1 || op.value.isEmpty() || members.holds(canon, op.value);
}

[[nodiscard]] bool graphOpIn(const SpaceOperation &op, const Members &members, const Canon &canon) {
    if (op.entity == "node"_L1)
        return members.nodes.contains(canon.thing(op.id)) && nodeFieldIn(op, members, canon);
    if (op.entity == "fact"_L1)
        return members.facts.contains(op.id) && op.field == "revealed"_L1 && metAt(members, canon, placeIn(op.value));
    const Place place = placeIn(op.value);
    return op.entity == "edge"_L1 && members.edges.contains(op.id) && op.field == "link"_L1
           && (place.book.isEmpty() || members.holds(canon, place.book)) && metAt(members, canon, place);
}

// Whether the operation says something the space holds. Of the graph, a
// thing's sightings and a relation's places only in the space's books, and
// of those only what the owner has met: a sighting, a fact or a relation
// learned past where they are in its book stays out until they get there.
[[nodiscard]] bool belongs(const SpaceOperation &op, const Members &members, const Canon &canon, const PublishedSpace::Parts &parts) {
    if (op.entity == "book"_L1)
        return members.books.contains(canon.book(op.id)) && bookFieldIn(op, parts);
    if (op.entity == "collection"_L1)
        return members.collections.contains(op.id) && (among(collectionFields, op.field) || op.field.startsWith("book:"_L1));
    if (op.entity == "reading_log"_L1)
        return parts.reading && members.days.contains(op.id) && op.field == "range"_L1;
    return graphOpIn(op, members, canon);
}

// The operation as the space has it, or none. The collection is at the top
// of the space, whatever it sits in at home. A book's own thing of the graph
// goes without what its data says of the owner's reading of it (each library
// makes its own), and a question without the row number of the book it was
// asked of here, which names another book anywhere else.
[[nodiscard]] std::optional<SpaceOperation> published(SpaceOperation op, const Members &members, const Canon &canon) {
    if (op.entity == "collection"_L1 && op.id == members.root && op.field == "parent"_L1)
        op.value.clear();
    if (op.entity == "node"_L1 && op.field == "data"_L1) {
        const QString kind = members.kinds.value(canon.thing(op.id));
        if (kind == "book"_L1)
            return std::nullopt;
        if (kind == "question"_L1) {
            QJsonObject data = QJsonDocument::fromJson(op.value.toUtf8()).object();
            data.remove("book");
            op.value = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
        }
    }
    return op;
}

[[nodiscard]] QString hashed(const QString &one, const QString &other) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(one.toUtf8());
    hash.addData(QByteArrayView("\x1f", 1));
    hash.addData(other.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(32));
}

// What the space shows of the owner's own. A collection by an id of this
// space alone: its id at home is its path there, which names what holds it.
// Each of the owner's computers by its place among them, which keeps how
// their writes order (SpaceReducer breaks a tie by device) and says only
// how many wrote it.
void disguise(QList<SpaceOperation> &ops, const QString &space) {
    QStringList devices;
    for (const SpaceOperation &op : std::as_const(ops))
        devices << op.device << op.author;
    devices.sort();
    devices.removeDuplicates();
    QHash<QString, QString> alias;
    for (qsizetype at = 0; at < devices.size(); ++at)
        alias.insert(devices.at(at), u"device-%1"_s.arg(at + 1, 3, 10, QChar(u'0')));
    const auto collection = [&space](const QString &home) { return u"c:"_s + hashed(space, home); };
    for (SpaceOperation &op : ops) {
        op.device = alias.value(op.device);
        op.author = alias.value(op.author);
        if (op.entity != "collection"_L1)
            continue;
        op.id = collection(op.id);
        if (op.field == "parent"_L1 && !op.value.isEmpty())
            op.value = collection(op.value);
    }
}

} // namespace

QStringList PublishedSpace::Parts::names() const {
    QStringList names;
    for (const auto &[on, name] :
         {std::pair{graph, u"graph"_s}, {questions, u"questions"_s}, {ratings, u"ratings"_s}, {reading, u"reading"_s}})
        if (on)
            names << name;
    return names;
}

PublishedSpace::Parts PublishedSpace::Parts::fromNames(const QStringList &names) {
    return {.graph = names.contains("graph"_L1),
            .questions = names.contains("questions"_L1),
            .ratings = names.contains("ratings"_L1),
            .reading = names.contains("reading"_L1)};
}

QString PublishedSpace::idFor(const QString &person, const QString &collectionGlobalId) {
    return u"published:"_s + hashed(person, collectionGlobalId);
}

std::expected<QList<SpaceOperation>, QString> PublishedSpace::operations(const Library &library, qint64 collectionId, const Parts &parts,
                                                                         const QString &space) {
    const std::optional<Collection> root = collectionOf(library, collectionId);
    if (!root)
        return std::unexpected(u"There is no collection %1 to publish."_s.arg(collectionId));
    // The owner's own log: what the space says is taken from there.
    QList<SpaceOperation> personal = SpaceReplay::operations(library);
    personal.removeIf([](const SpaceOperation &op) { return !op.space.startsWith("personal:"_L1); });
    const Canon canon{.books = SpaceReducer::sameAs(personal), .things = SpaceReducer::sameAs(personal, "node"_L1)};
    const ReducedState state = SpaceReducer::reduce(personal);
    Members members;
    collectTree(library, *root, canon, members);
    collectGraph(state, canon, parts, members);
    if (parts.reading)
        collectDays(state, canon, members);

    QList<SpaceOperation> chosen;
    for (const SpaceOperation &op : std::as_const(personal)) {
        if (!belongs(op, members, canon, parts))
            continue;
        if (std::optional<SpaceOperation> taken = published(op, members, canon)) {
            taken->space = space;
            chosen << *taken;
        }
    }
    QList<SpaceOperation> compacted = Compaction::latest(chosen);
    disguise(compacted, space);
    std::ranges::sort(compacted, [](const SpaceOperation &one, const SpaceOperation &other) {
        return std::tie(one.device, one.hlc, one.entity, one.id, one.field, one.value)
               < std::tie(other.device, other.hlc, other.entity, other.id, other.field, other.value);
    });
    QHash<QString, qint64> numbered;
    for (SpaceOperation &op : compacted)
        op.seq = ++numbered[op.device];
    return compacted;
}

std::expected<PublishedSpace::Publication, QString> PublishedSpace::publish(const Library &library, qint64 collectionId, const Parts &parts,
                                                                            const SpaceSnapshot::Owner &owner) {
    if (owner.person.isEmpty())
        return std::unexpected(u"A space is published by a person, and there is no person id to publish it as."_s);
    const std::optional<Collection> collection = collectionOf(library, collectionId);
    if (!collection)
        return std::unexpected(u"There is no collection %1 to publish."_s.arg(collectionId));
    const QString home = Spaces::collectionId(library, collectionId);
    const QString space = idFor(owner.person, home);
    Spaces::ensurePublishedSpace(library, space, collection->name, owner.person, owner.name);
    PublishedLog::remember(library, space, {.collection = home, .parts = parts.names()});
    return follow(library, space);
}

std::expected<PublishedSpace::Publication, QString> PublishedSpace::follow(const Library &library, const QString &space) {
    const std::optional<PublishedLog::Record> record = PublishedLog::recorded(library, space);
    if (!record)
        return std::unexpected(u"%1 is not a space published from this library."_s.arg(space));
    QSqlQuery owner = ChangeLog::query(library, "SELECT owner FROM spaces WHERE id = ?", {space});
    const QString person = owner.next() ? owner.value(0).toString() : QString();
    owner.finish();
    const qint64 collectionId = Spaces::localId(library, u"collection"_s, record->collection);
    const auto now = operations(library, collectionId, Parts::fromNames(record->parts), space);
    if (!now)
        return std::unexpected(now.error());
    // Written by the owner, whichever of their computers it was, and
    // unsigned: what was signed at home was signed as it is there (its
    // space, device and number), which none of these is (ADR 0001, People
    // and keys).
    const qint64 appended = PublishedLog::follow(library, space, *now, person);
    return Publication{.space = space, .appended = appended, .operations = PublishedLog::operations(library, space).size()};
}

std::expected<SpaceSnapshot, QString> PublishedSpace::snapshot(const Library &library, qint64 collectionId, const Parts &parts,
                                                               const SpaceSnapshot::Owner &owner) {
    const auto published = publish(library, collectionId, parts, owner);
    if (!published)
        return std::unexpected(published.error());
    const std::optional<Collection> collection = collectionOf(library, collectionId);
    if (!collection)
        return std::unexpected(u"There is no collection %1 to publish."_s.arg(collectionId));
    // Each operation keeps the number it has in the log, so that one who
    // holds this snapshot is sent only what the log takes after it.
    QList<SpaceOperation> ops = Compaction::latest(PublishedLog::operations(library, published->space));
    std::ranges::sort(ops, [](const SpaceOperation &one, const SpaceOperation &other) {
        return std::tie(one.device, one.seq) < std::tie(other.device, other.seq);
    });
    qint64 clock = Spaces::lastHlc(library);
    for (const SpaceOperation &op : std::as_const(ops))
        clock = std::max(clock, op.hlc);
    return SpaceSnapshot{.space = published->space,
                         .kind = QString::fromLatin1(kind),
                         .name = collection->name,
                         .owner = owner,
                         .parts = parts.names(),
                         .clock = clock,
                         .writtenBy = QString::fromLatin1(OMABOOK_VERSION),
                         .operations = std::move(ops)};
}
