#include "publishedspace.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
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
#include "privatebooks.h"
#include "publishedlog.h"
#include "spaceannotations.h"
#include "spacereducer.h"
#include "spaces.h"
#include "spaceworlds.h"

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
    QHash<QString, QSet<QString>> worlds; // a plugin's scope given to the space, and the books it goes with (#65)
    QSet<QString> files;                  // the books whose plugin gave the space their file with their world (#107)
    SpaceAnnotations::Chosen annotations; // of its books, when its annotations part is on (#85)
    QSet<QString> withheld;               // books of a kind a plugin keeps private, not let go (#127)

    [[nodiscard]] bool holds(const Canon &canon, const QString &bookGid) const {
        return !bookGid.isEmpty() && books.contains(canon.book(bookGid));
    }
    // Whether the owner has met a place of a book: nothing past where they
    // got in it, nothing of one not begun, all of one read. A place of no
    // book, or of a book the space doesn't hold, is not theirs to bound.
    [[nodiscard]] bool met(const Canon &canon, const QString &bookGid, const BookPlace &place) const {
        if (!holds(canon, bookGid))
            return true;
        return reach.value(canon.book(bookGid)).covers(GraphPlace{0, place});
    }
    [[nodiscard]] bool met(const Canon &canon, const QString &bookGid, const SpaceMoment &moment) const {
        return met(canon, bookGid, moment.place);
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
    const QList<Book> books = root.of(library.books());
    // A book its plugin keeps private stays out, all of it, unless the person let it go (#127).
    const QSet<qint64> withheld = PrivateBooks::withheld(library, books);
    for (const Book &book : books) {
        const QString gid = canon.book(Spaces::bookId(library, book.id));
        if (withheld.contains(book.id)) {
            members.withheld.insert(gid);
            continue;
        }
        members.books.insert(gid);
        members.reach.insert(gid, GraphContext::reachOf(library, book.id));
    }
}

// Whether a thing was met in one of the space's books, where the owner has
// read; a thing of a world only in a book its world goes with.
[[nodiscard]] bool metInBooks(const ReducedNode &node, const Canon &canon, const Members &members) {
    const QString scope = node.fields.value(u"scope"_s).value;
    for (auto seen = node.seen.cbegin(); seen != node.seen.cend(); ++seen)
        if (members.holds(canon, seen.key()) && members.met(canon, seen.key(), *seen)
            && (scope.isEmpty() || members.worlds.value(scope).contains(canon.book(seen.key()))))
            return true;
    return false;
}

// Of the worlds given to the space, those of its books, each scope with the books it goes with.
[[nodiscard]] QList<SpaceWorlds::World> collectWorlds(QList<SpaceWorlds::World> worlds, const Canon &canon, Members &members) {
    worlds.removeIf([&](const SpaceWorlds::World &world) { return !members.holds(canon, world.book); });
    for (const SpaceWorlds::World &world : std::as_const(worlds)) {
        if (!world.scope.isEmpty())
            members.worlds[world.scope].insert(canon.book(world.book));
        if (world.file)
            members.files.insert(canon.book(world.book));
    }
    return worlds;
}

// Whether a relation was learned where the owner has read, or in no book of the space.
[[nodiscard]] bool learned(const ReducedEdge &edge, const Canon &canon, const Members &members) {
    return !edge.hasLearned || members.met(canon, edge.bookGid, edge.learned);
}

// The things of the graph its books hold: met in one of them, learned of
// there, related there, a book's own, or related to one; questions apart.
struct GraphOfBooks {
    QSet<QString> things; // alive, of the library's own graph, or of a world given to the space
    QHash<QString, QString> kinds;
    QSet<QString> bookNodes; // the books' own things
    QSet<QString> questions; // things that are a question asked
    QSet<QString> met;
};

// The things of the library's own graph and of the worlds given to the
// space, and of those which its books hold themselves (a book's own thing,
// one met in a book).
void readThings(const ReducedState &state, const Canon &canon, const Members &members, GraphOfBooks &graph) {
    for (auto node = state.nodes.cbegin(); node != state.nodes.cend(); ++node) {
        const QString kind = node->fields.value(u"kind"_s).value;
        const QString scope = node->fields.value(u"scope"_s).value;
        // A tag is the owner's own word for a book, which the space leaves out.
        if (!alive(*node) || !(scope.isEmpty() || members.worlds.contains(scope)) || kind == "tag"_L1)
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
    BookPlace at;
};

[[nodiscard]] Place placeIn(const QString &json, const QString &book = {}) {
    const QJsonObject object = QJsonDocument::fromJson(json.toUtf8()).object();
    return {.book = book.isEmpty() ? object.value("book").toString() : book, .at = BookPlace::fromJson(object)};
}

[[nodiscard]] bool metAt(const Members &members, const Canon &canon, const Place &place) {
    return members.met(canon, place.book, place.at);
}

// A book's files are said when the space carries its books' files, and of a
// book its plugin made when the plugin gave the space its file with its
// world, whatever the space's switch (#107).
[[nodiscard]] bool bookFieldIn(const SpaceOperation &op, const PublishedSpace::Parts &parts, bool fileGiven) {
    return among(identityFields, op.field) || op.field == "same_as"_L1 || (parts.ratings && op.field == "rating"_L1)
           || (parts.reading && among(readingFields, op.field)) || ((parts.files || fileGiven) && op.field == "formats"_L1);
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
        return members.books.contains(canon.book(op.id)) && bookFieldIn(op, parts, members.files.contains(canon.book(op.id)));
    if (op.entity == "collection"_L1)
        return members.collections.contains(op.id)
               && (among(collectionFields, op.field)
                   || (op.field.startsWith("book:"_L1) && !members.withheld.contains(canon.book(op.field.mid(5)))));
    if (op.entity == "reading_log"_L1)
        return parts.reading && members.days.contains(op.id) && op.field == "range"_L1;
    if (op.entity == "annotation"_L1)
        return SpaceAnnotations::goes(op, members.annotations);
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
    if (op.entity == "book"_L1 && op.field == "formats"_L1)
        op.value = PublishedSpace::filesSaid(op.value);
    if (op.entity == "annotation"_L1 && op.field == "book"_L1)
        op.value = canon.book(op.value);
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
// A reading day the same, so that its removal names no day.
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
    const auto collection = [&space](const QString &home) { return PublishedSpace::collectionIdIn(space, home); };
    for (SpaceOperation &op : ops) {
        op.device = alias.value(op.device);
        op.author = alias.value(op.author);
        // A reading day by an id of this space too: at home it is the book
        // and the day, which its removal would still say once the day is gone.
        if (op.entity == "reading_log"_L1)
            op.id = u"d:"_s + hashed(space, op.id);
        if (op.entity != "collection"_L1)
            continue;
        op.id = collection(op.id);
        if (op.field == "parent"_L1 && !op.value.isEmpty())
            op.value = collection(op.value);
    }
}

} // namespace

QString PublishedSpace::filesSaid(const QString &formats) {
    QJsonArray said;
    for (const QJsonValue file : QJsonDocument::fromJson(formats.toUtf8()).array())
        said << QJsonObject{{"format", file["format"]}, {"hash", file["hash"]}, {"size", file["size"]}};
    return QString::fromUtf8(QJsonDocument(said).toJson(QJsonDocument::Compact));
}

QString PublishedSpace::collectionIdIn(const QString &space, const QString &homeGid) { return u"c:"_s + hashed(space, homeGid); }

QString PublishedSpace::idFor(const QString &person, const QString &collectionGlobalId) {
    return u"published:"_s + hashed(person, collectionGlobalId);
}

QString PublishedSpace::spaceFor(const Library &library, const QString &person, const QString &collectionGlobalId) {
    const QString made = idFor(person, collectionGlobalId);
    QSqlQuery query = ChangeLog::query(library,
                                       "SELECT s.id FROM spaces AS s JOIN published AS p ON p.space = s.id WHERE s.kind = 'published' "
                                       "AND s.owner = ? AND p.collection = ? ORDER BY s.id = ? DESC, s.id LIMIT 1",
                                       {person, collectionGlobalId, made});
    return query.next() ? query.value(0).toString() : made;
}

std::expected<QList<SpaceOperation>, QString> PublishedSpace::operations(const Library &library, qint64 collectionId, const Parts &parts,
                                                                         const QString &space) {
    const std::optional<Collection> root = collectionOf(library, collectionId);
    if (!root)
        return std::unexpected(u"There is no collection %1 to publish."_s.arg(collectionId));
    // The owner's own log: what the space says is taken from there, and of
    // each world given to it the log of its own the world is kept in.
    const QList<SpaceWorlds::World> worlds = SpaceWorlds::granted(library, space, parts.graph);
    const QStringList scopes = SpaceWorlds::scopesOf(worlds);
    QList<SpaceOperation> personal = SpaceReplay::operations(library);
    personal.removeIf([&scopes](const SpaceOperation &op) { return !op.space.startsWith("personal:"_L1) && !scopes.contains(op.space); });
    const Canon canon{.books = SpaceReducer::sameAs(personal), .things = SpaceReducer::sameAs(personal, "node"_L1)};
    const ReducedState state = SpaceReducer::reduce(personal);
    Members members;
    collectTree(library, *root, canon, members);
    const QList<SpaceWorlds::World> given = collectWorlds(worlds, canon, members);
    collectGraph(state, canon, parts, members);
    if (parts.reading)
        collectDays(state, canon, members);
    if (parts.annotations)
        members.annotations = SpaceAnnotations::choose(state, [&](const QString &book) { return members.holds(canon, book); });

    QList<SpaceOperation> chosen;
    for (const SpaceOperation &op : std::as_const(personal)) {
        if (!belongs(op, members, canon, parts))
            continue;
        if (std::optional<SpaceOperation> taken = published(op, members, canon)) {
            taken->space = space;
            chosen << *taken;
        }
    }
    chosen += SpaceWorlds::said(given, space, Spaces::deviceId(library));
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
    const QString space = spaceFor(library, owner.person, home);
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
    // A collection no longer at home is no longer in the space: all it held
    // there is taken back from those who follow it.
    QList<SpaceOperation> now;
    if (const qint64 collectionId = Spaces::localId(library, u"collection"_s, record->collection); collectionOf(library, collectionId)) {
        auto ops = operations(library, collectionId, Parts::fromNames(record->parts), space);
        if (!ops)
            return std::unexpected(ops.error());
        now = std::move(*ops);
    }
    // Written by the owner, whichever of their computers it was, and
    // unsigned: what was signed at home was signed as it is there (its
    // space, device and number), which none of these is (ADR 0001, People
    // and keys).
    const qint64 appended = PublishedLog::follow(library, space, now, person);
    return Publication{.space = space, .appended = appended, .operations = PublishedLog::operations(library, space).size()};
}
