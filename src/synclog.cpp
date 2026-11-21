#include "synclog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <span>
#include <tuple>
#include <utility>

#include "compaction.h"
#include "keptops.h"
#include "log.h"
#include "pairing.h"
#include "plugindata.h"
#include "replication.h"
#include "spacecarry.h"
#include "spacereplay.h"
#include "spaces.h"
#include "synctake.h"

namespace {

[[nodiscard]] QStringList titlesOf(const Library &library, const QList<qint64> &books) {
    QStringList titles;
    for (const qint64 id : books)
        if (const std::optional<Book> book = library.book(id))
            titles << book->meta.title;
    return titles;
}

// The books whose place taking `incoming` changes, or with `dryRun` would.
[[nodiscard]] QList<qint64> takePlaces(Library &library, const QList<SpaceOperation> &incoming, bool dryRun) {
    if (!dryRun)
        return SpaceReplay::takePlaces(library, incoming);
    QList<qint64> books;
    for (const Replication::PlaceChange &change : Replication::placeChanges(library, incoming))
        books << change.book;
    return books;
}

// The last of each space and device either vector holds.
[[nodiscard]] Replication::Vector joined(Replication::Vector one, const Replication::Vector &other) {
    for (auto space = other.cbegin(); space != other.cend(); ++space)
        for (auto device = space->cbegin(); device != space->cend(); ++device)
            one[space.key()][device.key()] = std::max(one.value(space.key()).value(device.key()), device.value());
    return one;
}

// The other side's books as its manifest names them, for this side to link
// to its own (Replication::link): the side asked links the books the one
// that asks sends it, so a book without a file there (an entry from
// Goodreads) is found here by its keys whichever side starts the sync (#154).
[[nodiscard]] QList<Replication::BookNames> namesIn(const QJsonArray &books) {
    QList<Replication::BookNames> names;
    for (const auto &value : books) {
        const QJsonObject book = value.toObject();
        QStringList hashes;
        QStringList keys;
        for (const auto &file : book.value("files").toArray())
            for (const char *name : {"hash", "original"})
                if (const QString hash = file.toObject().value(QLatin1StringView(name)).toString(); !hash.isEmpty())
                    hashes << hash;
        if (hashes.isEmpty())
            for (const auto &key : book.value("keys").toArray())
                keys << key.toString();
        if (const QString gid = book.value("gid").toString(); !gid.isEmpty())
            names << Replication::BookNames{.gid = gid, .hashes = hashes, .keys = keys};
    }
    return names;
}

[[nodiscard]] QSet<QString> heldIn(const QJsonArray &names) {
    QSet<QString> held;
    for (const auto &name : names)
        held.insert(name.toString());
    return held;
}

// The operations of `incoming` the computer that `theirs` (its manifest or
// request) introduces may give, and those it may not counted in `summary` and
// said there as `refusal` (with the count and why) says it, for whoever
// started the sync (Replication::admit).
//
// A dry run pairs nothing, but a sync pairs with another personal space before
// it takes anything (Spaces::pair), and from then on its device is one of
// one's own computers: so a dry run admits from such a peer what a sync
// would, as from this computer, and says no refusal a sync wouldn't make.
[[nodiscard]] QList<SpaceOperation> admitted(const Library &library, const QList<SpaceOperation> &incoming, const QJsonObject &theirs,
                                             const QString &refusal, SyncSummary &summary) {
    const QString peer = theirs.value("device").toString();
    const bool wouldPair = summary.dryRun && theirs.value("space").toString().startsWith(QLatin1StringView("personal:")) && !peer.isEmpty();
    Replication::Admitted taken = Replication::admit(library, incoming, wouldPair ? Spaces::deviceId(library) : peer);
    if (taken.refused > 0) {
        summary.refused += taken.refused;
        summary.problems << refusal.arg(taken.refused)
                                .arg("they are not as their author signed them, are unsigned and not from one of your own computers, "
                                     "or are a plugin's data over its limits");
        qCWarning(lcSync).noquote() << taken.refused << "operations refused from" << peer;
    }
    return std::move(taken.ops);
}

void takePerson(Library &library, const QJsonObject &theirs, SyncSummary &summary) {
    if (const auto taken = Pairing::take(library, theirs); !taken)
        summary.problems << "This computer could not take on the person it is paired with: " + taken.error();
}

// Takes what one's other computer carried of the spaces one is in (#51), and says what was refused.
void takeCarried(Library &library, const QJsonObject &theirs, SyncSummary &summary) {
    const SpaceCarry::Taken taken = SpaceCarry::take(library, theirs);
    summary.refused += static_cast<int>(taken.refused);
    summary.problems << taken.problems;
    if (taken.spaces + taken.operations + taken.subscriptions + taken.dropped > 0)
        qCInfo(lcSync).noquote() << "carried here:" << taken.spaces << "shared spaces," << taken.keys << "keys," << taken.operations
                                 << "of their operations," << taken.subscriptions << "subscriptions;" << taken.dropped
                                 << "subscriptions given up elsewhere dropped";
}

// The manifest, as the sync goes on with it: both now hold the same
// operations, so what this side holds, they do.
QJsonObject holding(const Library &library, QJsonObject manifest) {
    manifest.insert("vector", Replication::toJson(Replication::vector(library)));
    if (manifest.contains("annotations"))
        manifest.insert("annotations", Replication::toJson(Replication::annotationVector(library)));
    return manifest;
}

// What this side carries to the other, whose introduction is `theirs`, in
// the exchange that carries (#51): only once the books have moved, so what
// is filed on arrival finds them there.
QJsonObject carried(Library &library, const QJsonObject &theirs, bool carry) {
    if (!carry)
        return {};
    QJsonObject offer = SpaceCarry::offer(library, theirs);
    offer.insert("carrying", true);
    return offer;
}

// What the other side, whose manifest or request is `theirs`, lacks of the
// log. Of the bookmarks and highlights (#66), what it says it holds of them,
// apart: an Omabook from before they travelled took none, though it holds
// what came after them. One that says nothing of them leaves them out of
// what it takes in (Replication::operationsFrom).
//
// A plugin's data (#76) goes only to a side that says it takes it: an
// Omabook from before leaves it out of what it takes in, and would be sent
// all of it again at every sync.
[[nodiscard]] QList<SpaceOperation> lackingThere(const Library &library, const QJsonObject &theirs) {
    const Replication::Vector known = Replication::vectorFrom(theirs.value("vector").toObject());
    QList<SpaceOperation> lacking =
        theirs.contains("annotations") ? Replication::since(library, known, Replication::vectorFrom(theirs.value("annotations").toObject()))
                                       : Replication::since(library, known);
    if (theirs.value("plugin_data").toInt() < 1)
        lacking.removeIf([](const SpaceOperation &op) { return PluginData::isPluginSpace(op.space); });
    return lacking;
}

QJsonObject with(QJsonObject object, const QJsonObject &more) {
    for (auto it = more.begin(); it != more.end(); ++it)
        object.insert(it.key(), it.value());
    return object;
}

// What the other side took of this one's, as its own summary says it took it.
void countThere(SyncSummary &summary, const QJsonObject &there) {
    const QJsonObject titles = there.value("named").toObject();
    const auto add = [&](int &count, QStringList &named, const char *key) {
        count += there.value(QLatin1StringView(key)).toInt();
        for (const auto &title : titles.value(QLatin1StringView(key)).toArray())
            named << title.toString();
    };
    add(summary.detailsSent, summary.titles.detailsSent, "details_taken");
    add(summary.readingSent, summary.titles.readingSent, "reading_taken");
    add(summary.removedThere, summary.titles.removedThere, "removed_here");
    summary.collectionsSent += there.value("collections_taken").toInt();
    summary.graphSent += there.value("graph_taken").toInt();
    summary.annotationsSent += there.value("annotations_taken").toInt();
    for (const auto &problem : there.value("problems").toArray())
        summary.problems << problem.toString();
}

// Takes in what was kept until this Omabook knew its kind (KeptOps), as a
// sync takes in what comes from each computer that wrote it: its places,
// then what the log says of the books, collections, bookmarks and graph.
// Nothing is removed here by it: every book here is still held there, as
// far as it can say.
void takeKept(Library &library, std::span<const SpaceOperation> ops) {
    SyncSummary summary;
    std::ignore = SpaceReplay::takePlaces(library, ops);
    const QSet<QString> held = SyncTake::heldBy(library);
    QSet<QString> devices;
    for (const SpaceOperation &op : ops)
        devices.insert(op.device);
    for (const QString &device : std::as_const(devices))
        std::ignore = SyncTake::take(
            library, ops,
            {.device = device, .details = SyncOptions::Details::Undecided, .dryRun = false, .remote = nullptr, .books = {}, .held = held},
            summary);
}

// Given as the program starts: the library layer sets a library up, and
// takes in there what it kept, by this.
[[maybe_unused]] const bool takesKept = KeptOps::takeWith(takeKept);

} // namespace

QJsonObject SyncLog::introduction(const Library &library) {
    return with({{"space", Spaces::personalSpace(library)},
                 {"device", Spaces::deviceId(library)},
                 {"vector", Replication::toJson(Replication::vector(library))},
                 {"annotations", Replication::toJson(Replication::annotationVector(library))},
                 {"plugin_data", 1}},
                with(Pairing::introduction(library), SpaceCarry::introduction(library)));
}

SyncLog::Replicated SyncLog::replicate(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary,
                                       const SyncOptions &options, bool carry) {
    const bool dryRun = options.dryRun;
    const QJsonArray books = manifest.value("books").toArray();
    const QString theirDevice = manifest.value("device").toString();
    if (!dryRun) {
        Spaces::pair(library, manifest.value("space").toString(), theirDevice, manifest.value("name").toString());
        Replication::link(library, namesIn(books));
    }
    QSet<QString> theirGids;
    for (const auto &book : books)
        theirGids.insert(book.toObject().value("gid").toString());
    const QList<SpaceOperation> lacking = lackingThere(library, manifest);
    // A dry run links nothing: what is unlinked stays so until a sync. Nor
    // does it pair: the person's key goes with it only in a sync, when theirs
    // is to become this one's (pairing.h).
    const QJsonObject request = with({{"space", Spaces::personalSpace(library)},
                                      {"device", Spaces::deviceId(library)},
                                      {"vector", Replication::toJson(Replication::vector(library))},
                                      {"annotations", Replication::toJson(Replication::annotationVector(library))},
                                      {"plugin_data", 1},
                                      {"ops", Replication::toJson(lacking, manifest.value("signs").toInt() >= 1)},
                                      {"books", dryRun ? QJsonArray() : Replication::toJson(Replication::unlinked(library, theirGids))},
                                      {"details", SyncTake::otherSide(options.details)},
                                      {"held", QJsonArray::fromStringList(SyncTake::heldBy(library).values())},
                                      {"dry_run", dryRun}},
                                     with(with(Pairing::introduction(library), dryRun ? QJsonObject() : Pairing::keyFor(library, manifest)),
                                          // The spaces one is in, to another of one's own computers alone (#51).
                                          with(SpaceCarry::introduction(library), carried(library, manifest, carry && !dryRun))));
    const auto answer = remote.exchange(QJsonDocument(request), {"sync", "--replicate"});
    if (!answer) {
        summary.problems << "The change log could not be carried between here and " + remote.host() + ": " + answer.error();
        return {.manifest = holding(library, manifest), .gone = {}};
    }
    const QList<SpaceOperation> incoming =
        KeptOps::keep(library,
                      admitted(library, Replication::operationsFrom(answer->value("ops").toArray()), manifest,
                               "%1 operations from " + remote.host() + " were refused and not taken in: %2.", summary),
                      dryRun);
    // They hold what they held, and now what was sent them.
    if (!dryRun)
        Compaction::rememberSeen(library, theirDevice,
                                 joined(Replication::vectorFrom(manifest.value("vector").toObject()),
                                        Replication::vectorFrom(request.value("vector").toObject())));
    const QList<qint64> taken = takePlaces(library, incoming, dryRun);
    summary.placesTaken += static_cast<int>(taken.size());
    summary.titles.placesTaken << titlesOf(library, taken);
    summary.placesSent += answer->value("changed").toInt();
    for (const auto &title : answer->value("titles").toArray())
        summary.titles.placesSent << title.toString();
    // Each take finds every book still undecided, so the last one of a sync
    // says which they are: run again at its end (finish()), the same books
    // are not counted twice (#59).
    summary.detailsUndecided = 0;
    summary.titles.detailsUndecided.clear();
    SyncTake::Taken described = SyncTake::take(library, incoming,
                                               {.device = theirDevice,
                                                .details = options.details,
                                                .dryRun = dryRun,
                                                .remote = &remote,
                                                .books = books,
                                                .held = SyncTake::heldBy(books)},
                                               summary);
    QJsonObject there = answer->value("taken").toObject();
    there.insert("named", answer->value("named"));
    countThere(summary, there);
    if (!dryRun) {
        Compaction::compactWhenWorthIt(library);
        takePerson(library, *answer, summary);
        takeCarried(library, *answer, summary);
    }
    // A dry run shows the other side what it lacks, to say what it would take, and neither side keeps any of it.
    qCInfo(lcSync).noquote() << (dryRun ? "dry run of the log with" : "the log with") << remote.host() << ":" << lacking.size()
                             << (dryRun ? "operations would be sent," : "operations sent,") << incoming.size()
                             << (dryRun ? "would be taken" : "taken");
    QJsonObject now = holding(library, manifest);
    now.insert("books", described.books);
    // Who they are now, once a pairing has made both one person (#51).
    for (const char *said : {"carries", "carrier", "carry"})
        now.insert(QLatin1StringView(said), answer->value(QLatin1StringView(said)));
    return {.manifest = now, .gone = described.gone};
}

void SyncLog::finish(Library &library, const Remote &remote, const QJsonObject &manifest, SyncSummary &summary,
                     const SyncOptions &options) {
    const bool carrying = SpaceCarry::worthCarrying(library, manifest);
    if (summary.received + summary.sent > 0 || carrying)
        std::ignore = replicate(library, remote, manifest, summary, options, carrying); // their books were taken in already
}

QJsonObject SyncLog::answer(Library &library, const QJsonObject &request) {
    const bool dryRun = request.value("dry_run").toBool();
    const QString theirDevice = request.value("device").toString();
    if (!dryRun) {
        Spaces::pair(library, request.value("space").toString(), theirDevice, request.value("name").toString());
        Replication::link(library, Replication::booksFrom(request.value("books").toArray()));
    }
    SyncSummary taken;
    taken.dryRun = dryRun;
    const QList<SpaceOperation> incoming =
        KeptOps::keep(library,
                      admitted(library, Replication::operationsFrom(request.value("ops").toArray()), request,
                               "%1 operations sent from here were refused there and not taken in: %2.", taken),
                      dryRun);
    // What they held when they asked: whether the answer reaches them, this side can't know.
    if (!dryRun)
        Compaction::rememberSeen(library, theirDevice, Replication::vectorFrom(request.value("vector").toObject()));
    const QList<qint64> changed = takePlaces(library, incoming, dryRun);
    // The side asked has no books of the other's to describe: it never fetches any.
    std::ignore = SyncTake::take(library, incoming,
                                 {.device = theirDevice,
                                  .details = SyncTake::detailsFrom(request.value("details").toString()),
                                  .dryRun = dryRun,
                                  .remote = nullptr,
                                  .books = {},
                                  .held = heldIn(request.value("held").toArray())},
                                 taken);
    // This side's key, when theirs is to be this side's person; else theirs is taken here.
    const QJsonObject pairing = with(Pairing::introduction(library), dryRun ? QJsonObject() : Pairing::keyFor(library, request));
    if (!dryRun) {
        Compaction::compactWhenWorthIt(library);
        takePerson(library, request, taken);
        takeCarried(library, request, taken);
    }
    // After taking theirs, so what was just written here goes back with the rest.
    const QList<SpaceOperation> lacking = lackingThere(library, request);
    return with(
        {{"ops", Replication::toJson(lacking, request.value("signs").toInt() >= 1)},
         {"changed", static_cast<int>(changed.size())},
         {"titles", QJsonArray::fromStringList(titlesOf(library, changed))},
         {"taken", taken.toJson()},
         {"named", QJsonObject{{"details_taken", QJsonArray::fromStringList(taken.titles.detailsTaken)},
                               {"reading_taken", QJsonArray::fromStringList(taken.titles.readingTaken)},
                               {"removed_here", QJsonArray::fromStringList(taken.titles.removedHere)}}}},
        // Once this side is who it is to be: the person a pairing just made it may be theirs (#51).
        with(pairing, with(SpaceCarry::introduction(library), carried(library, request, request.value("carrying").toBool() && !dryRun))));
}
