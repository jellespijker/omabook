#include "collectionshare.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <cstdio>
#include <print>
#include <span>
#include <tuple>

#include "changelog.h"
#include "githublink.h"
#include "person.h"
#include "spacelinks.h"
#include "spacereducer.h"
#include "spaces.h"
#include "spacesnapshot.h"

using namespace Qt::StringLiterals;

namespace {

using PublishedSpace::Parts;

void say(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = std::fflush(stream); // Nothing better to do when the terminal is gone.
}

int fail(const QString &why) {
    say(stderr, why);
    return 1;
}

// What a space holds, by kind.
struct Counts {
    int collections = 0, books = 0, things = 0, facts = 0, relations = 0, questions = 0, ratings = 0, places = 0, days = 0;
};

template <typename Thing> [[nodiscard]] bool alive(const Thing &thing) { return !SpaceReducer::isRemoved(thing.alive, thing.removed); }

[[nodiscard]] bool has(const ReducedBook &book, const QString &field) {
    const QString value = book.fields.value(field).value;
    return !value.isEmpty() && value != "0"_L1;
}

void countBooks(const ReducedState &state, Counts &counts) {
    for (const ReducedBook &book : state.books) {
        if (!alive(book))
            continue;
        ++counts.books;
        counts.ratings += has(book, u"rating"_s) ? 1 : 0;
        counts.places += has(book, u"progress"_s) || has(book, u"shelf"_s) || has(book, u"finished"_s) ? 1 : 0;
    }
}

void countGraph(const ReducedState &state, Counts &counts) {
    for (const ReducedNode &node : state.nodes) {
        const QString kind = node.fields.value(u"kind"_s).value;
        if (!alive(node) || kind == "book"_L1) // a book's own thing is the book
            continue;
        ++(kind == "question"_L1 ? counts.questions : counts.things);
    }
    counts.facts = static_cast<int>(std::ranges::count_if(state.facts, [](const ReducedFact &fact) { return alive(fact); }));
    counts.relations = static_cast<int>(std::ranges::count_if(state.edges, [](const ReducedEdge &edge) { return alive(edge); }));
}

[[nodiscard]] Counts countsOf(std::span<const SpaceOperation> ops) {
    const ReducedState state = SpaceReducer::reduce(ops);
    Counts counts;
    counts.collections = static_cast<int>(std::ranges::count_if(state.collections, [](const ReducedCollection &c) { return alive(c); }));
    countBooks(state, counts);
    countGraph(state, counts);
    counts.days = static_cast<int>(std::ranges::count_if(state.days, [](const ReducedDay &day) { return day.valid; }));
    return counts;
}

[[nodiscard]] QJsonObject countsJson(const Counts &counts) {
    return {{"collections", counts.collections}, {"books", counts.books},
            {"things", counts.things},           {"facts", counts.facts},
            {"relations", counts.relations},     {"questions", counts.questions},
            {"ratings", counts.ratings},         {"reading_places", counts.places},
            {"reading_days", counts.days}};
}

[[nodiscard]] QJsonArray leftOut(const Parts &parts, const Counts &every, int files) {
    QJsonArray out{QJsonObject{{"part", "files"}, {"count", files}, {"never", true}}};
    if (!parts.graph)
        out << QJsonObject{{"part", "graph"},
                           {"things", every.things},
                           {"facts", every.facts},
                           {"relations", every.relations},
                           {"switch", "--with-graph"}};
    if (!parts.questions)
        out << QJsonObject{{"part", "questions"}, {"count", every.questions}, {"switch", "--with questions"}};
    if (!parts.ratings)
        out << QJsonObject{{"part", "ratings"}, {"count", every.ratings}, {"switch", "--with ratings"}};
    if (!parts.reading)
        out << QJsonObject{{"part", "progress"}, {"places", every.places}, {"days", every.days}, {"switch", "--with progress"}};
    return out;
}

[[nodiscard]] std::expected<Parts, QString> partsFrom(const QCommandLineParser &parser) {
    Parts parts{.graph = parser.isSet(u"with-graph"_s)};
    for (const QString &value : parser.values(u"with"_s))
        for (const QString &given : value.split(u',', Qt::SkipEmptyParts)) {
            const QString part = given.trimmed().toLower();
            if (part == "questions"_L1)
                parts.questions = true;
            else if (part == "ratings"_L1)
                parts.ratings = true;
            else if (part == "progress"_L1 || part == "reading"_L1)
                parts.reading = true;
            else if (part == "graph"_L1)
                parts.graph = true;
            else
                return std::unexpected(u"--with takes questions, ratings and progress, and \"%1\" is none of them."_s.arg(given));
        }
    return parts;
}

void printPreview(const QJsonObject &shown) {
    const QJsonObject in = shown.value("in").toObject();
    const auto n = [&in](const char *key) { return in.value(QLatin1StringView(key)).toInt(); };
    const QStringList parts = shown.value("parts").toVariant().toStringList();
    say(stdout, u"Sharing %1. Goes in:"_s.arg(shown.value("collection").toString()));
    say(stdout, u"  %1 collections: their names, rules and how their shelves show"_s.arg(n("collections")));
    say(stdout, u"  %1 books: title, authors, series, ISBN and their names elsewhere"_s.arg(n("books")));
    if (parts.contains("graph"_L1))
        say(stdout,
            u"  the graph: %1 things, %2 facts, %3 relations; only what you have read, nothing past your place in a book"_s.arg(n("things"))
                .arg(n("facts"))
                .arg(n("relations")));
    if (parts.contains("questions"_L1))
        say(stdout, u"  %1 questions you asked"_s.arg(n("questions")));
    if (parts.contains("ratings"_L1))
        say(stdout, u"  %1 ratings"_s.arg(n("ratings")));
    if (parts.contains("reading"_L1))
        say(stdout, u"  your reading: %1 places, %2 days"_s.arg(n("reading_places")).arg(n("reading_days")));
    say(stdout, u"Left out:"_s);
    for (const QJsonValue value : shown.value("left_out").toArray()) {
        const QJsonObject part = value.toObject();
        const QString name = part.value("part").toString();
        const auto of = [&part](const char *key) { return QString::number(part.value(QLatin1StringView(key)).toInt()); };
        if (name == "files"_L1)
            say(stdout, u"  the books' files (%1): never shared"_s.arg(of("count")));
        else if (name == "graph"_L1)
            say(stdout, u"  the graph (%1 things, %2 facts, %3 relations): --with-graph"_s.arg(of("things"), of("facts"), of("relations")));
        else if (name == "progress"_L1)
            say(stdout, u"  your reading (%1 places, %2 days): --with progress"_s.arg(of("places"), of("days")));
        else
            say(stdout, u"  %1 (%2): %3"_s.arg(name, of("count"), part.value("switch").toString()));
    }
}

// The name the owner goes by in the space: as given, else as they gave it before.
[[nodiscard]] std::expected<SpaceSnapshot::Owner, QString> ownerOf(const Library &library, const QCommandLineParser &parser,
                                                                   const Collection &collection) {
    // The user's person, who signs what this library writes (Library::person).
    const Person *person = library.person();
    if (person == nullptr)
        return std::unexpected(u"There is no person to publish the space as; omabook identity says why."_s);
    SpaceSnapshot::Owner owner{.name = parser.value(u"as"_s).trimmed(), .person = person->id()};
    if (owner.name.isEmpty()) {
        const QString space = PublishedSpace::idFor(owner.person, Spaces::collectionId(library, collection.id));
        QSqlQuery named = ChangeLog::query(library, "SELECT name FROM space_members WHERE space = ? AND person = ?", {space, owner.person});
        owner.name = named.next() ? named.value(0).toString() : QString();
    }
    if (owner.name.isEmpty())
        return std::unexpected(u"Say what name you go by in the space with --as <name>: it is what those you share it with see of you."_s);
    return owner;
}

[[nodiscard]] std::expected<QJsonObject, QString> publish(const Library &library, const QCommandLineParser &parser,
                                                          const Collection &collection, const Parts &parts) {
    const auto owner = ownerOf(library, parser, collection);
    if (!owner)
        return std::unexpected(owner.error());
    const auto published = PublishedSpace::publish(library, collection.id, parts, *owner);
    if (!published)
        return std::unexpected(published.error());
    QJsonObject done{{"space", published->space},
                     {"owner", owner->name},
                     {"person", owner->person},
                     {"operations", published->operations},
                     {"new", published->appended}};
    if (const QString file = parser.value(u"output"_s); !file.isEmpty()) {
        const auto snapshot = PublishedSpace::snapshot(library, collection.id, parts, *owner);
        if (!snapshot)
            return std::unexpected(snapshot.error());
        if (const auto saved = SpaceSnapshotFile::save(*snapshot, file); !saved)
            return std::unexpected(saved.error());
        done.insert("output", file);
        done.insert("snapshot_operations", snapshot->operations.size());
    }
    if (parser.isSet(u"gist"_s)) {
        GitHubLink github;
        const auto sent = CollectionShare::send(library, published->space, github, parser.isSet(u"public"_s));
        if (!sent)
            return std::unexpected(sent.error());
        done.insert("sent", *sent);
    }
    return done;
}

void printPublished(const QJsonObject &done) {
    say(stdout, u"Published as the space %1, by %2: %3 operations in its log, %4 of them new."_s.arg(
                    done.value("space").toString(), done.value("owner").toString(), QString::number(done.value("operations").toInteger()),
                    QString::number(done.value("new").toInteger())));
    if (done.contains("output"))
        say(stdout, u"Wrote %1 (%2 operations): give it to whoever you share the collection with."_s.arg(
                        done.value("output").toString(), QString::number(done.value("snapshot_operations").toInteger())));
    if (const QJsonObject sent = done.value("sent").toObject(); !sent.isEmpty()) {
        const QString link = sent.value("address").toString();
        const QString kind = sent.value("public").toBool() ? u"public"_s : u"secret"_s;
        say(stdout,
            sent.value("created").toBool()
                ? u"Sent to a %1 gist (%2 operations): %3\nWhoever has the link can import it, omabook collections import %3, and "
                  "follow what you change; sharing it again with --gist updates this gist."_s.arg(kind)
                      .arg(sent.value("operations").toInteger())
                      .arg(link)
                : u"Brought the %1 gist up to date (%2 operations): %3"_s.arg(kind).arg(sent.value("operations").toInteger()).arg(link));
        if (const QString note = sent.value("note").toString(); !note.isEmpty())
            say(stdout, note);
    }
    if (!done.contains("output") && !done.contains("sent"))
        say(stdout, u"Give it to others as a file with --output <file>%1, or as a link with --gist."_s.arg(
                        QLatin1StringView(SpaceSnapshotFile::suffix)));
}

} // namespace

void CollectionShare::addOptions(QCommandLineParser &parser) {
    parser.addOption({u"with-graph"_s, u"share: with what the knowledge graph knows of its books, up to where you have read."_s});
    parser.addOption({u"with"_s, u"share: also these of yours: questions, ratings, progress."_s, u"parts"_s});
    parser.addOption({u"output"_s, u"share: write the space's snapshot to this file."_s, u"file"_s});
    parser.addOption({u"gist"_s, u"share: send the space's snapshot to a gist, through GitHub's command line gh, and print its link; "
                                 "the same gist each time."_s});
    parser.addOption({u"public"_s, u"share --gist: a public gist, which anyone may find, rather than a secret one."_s});
}

std::expected<QJsonObject, QString> CollectionShare::send(const Library &library, const QString &space, SpaceTransport &transport,
                                                          bool open) {
    const auto snapshot = PublishedSpace::snapshotOf(library, space);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    const auto before = SpaceLinks::of(library, space, SpaceLinks::Role::Sent, transport.name());
    const auto sent = transport.publish(*snapshot, {.previous = before ? before->address : QString(), .open = open});
    if (!sent)
        return std::unexpected(sent.error());
    SpaceLinks::remember(library, SpaceLinks::Role::Sent, {.space = space, .transport = transport.name(), .address = sent->address});
    return QJsonObject{{"address", sent->address}, {"transport", transport.name()},
                       {"created", sent->created}, {"public", sent->open},
                       {"note", sent->note},       {"operations", snapshot->operations.size()}};
}

std::expected<QJsonObject, QString> CollectionShare::preview(const Library &library, const Collection &collection, const Parts &parts) {
    // A space id of no one's, since nothing is written: the counts are the same.
    const QString space = u"published:preview"_s;
    const auto chosen = PublishedSpace::operations(library, collection.id, parts, space);
    if (!chosen)
        return std::unexpected(chosen.error());
    const auto every =
        PublishedSpace::operations(library, collection.id, {.graph = true, .questions = true, .ratings = true, .reading = true}, space);
    if (!every)
        return std::unexpected(every.error());
    int files = 0;
    for (const Book &book : collection.of(library.books()))
        files += static_cast<int>(book.formats.size());
    return QJsonObject{{"collection", collection.path},
                       {"parts", QJsonArray::fromStringList(parts.names())},
                       {"in", countsJson(countsOf(*chosen))},
                       {"left_out", leftOut(parts, countsOf(*every), files)}};
}

int CollectionShare::run(const Library &library, const QCommandLineParser &parser, const QString &path, bool json) {
    if (path.trimmed().isEmpty())
        return fail(u"Name the collection to share: omabook collections share <path>."_s);
    const auto collection = library.collectionNamed(path);
    if (!collection)
        return fail(u"There is no collection called %1. omabook collections lists them."_s.arg(path));
    const auto parts = partsFrom(parser);
    if (!parts)
        return fail(parts.error());
    if (parser.isSet(u"public"_s) && !parser.isSet(u"gist"_s))
        return fail(u"--public says what kind of gist to share to, so it goes with --gist."_s);
    auto shown = preview(library, *collection, *parts);
    if (!shown)
        return fail(shown.error());
    const bool dryRun = parser.isSet(u"dry-run"_s);
    shown->insert("dry_run", dryRun);
    if (!json)
        printPreview(*shown);
    if (!dryRun) {
        const auto done = publish(library, parser, *collection, *parts);
        if (!done)
            return fail(done.error());
        shown->insert("published", *done);
        if (!json)
            printPublished(*done);
    } else if (!json) {
        say(stdout, u"Nothing was published or written (--dry-run)."_s);
    }
    if (json)
        say(stdout, QString::fromUtf8(QJsonDocument(*shown).toJson(QJsonDocument::Indented)).trimmed());
    return 0;
}
