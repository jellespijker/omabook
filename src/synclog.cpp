#include "synclog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <algorithm>
#include <span>
#include <tuple>
#include <utility>

#include "booklinks.h"
#include "bookpairing.h"
#include "compaction.h"
#include "keptops.h"
#include "log.h"
#include "pairing.h"
#include "plugindata.h"
#include "replication.h"
#include "spacecarry.h"
#include "spacereplay.h"
#include "spaces.h"
#include "strayidentifiers.h"
#include "synctake.h"

namespace {

[[nodiscard]] QStringList titlesOf(const Library &library, const QList<qint64> &books) {
    QStringList titles;
    for (const qint64 id : books)
        if (const std::optional<Book> book = library.book(id))
            titles << book->meta.title;
    return titles;
}

// The books whose place taking `incoming` changes, or with `dryRun` would,
// each place a dry run would move in `plan` (#187), with where in the log's
// order it and here's are: never by their wall-clock times, which a computer
// whose clock is behind gives a place read later (#211).
[[nodiscard]] QList<qint64> takePlaces(Library &library, const QList<SpaceOperation> &incoming, bool dryRun, SyncPlan &plan) {
    if (!dryRun)
        return SpaceReplay::takePlaces(library, incoming);
    QList<qint64> books;
    for (const Replication::PlaceChange &change : Replication::placeChanges(library, incoming)) {
        books << change.book;
        plan.places << SyncPlan::Place{.title = titlesOf(library, {change.book}).value(0), .before = change.was, .after = change.stamp};
    }
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
// Only its strong keys no other book there has ("pair_keys"): an Omabook
// before 0.2.2 names none, and its books without a file are paired by their
// id alone (#158).
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
            for (const auto &key : book.value("pair_keys").toArray())
                keys << key.toString();
        if (const QString gid = book.value("gid").toString(); !gid.isEmpty())
            names << Replication::BookNames{.gid = gid, .hashes = hashes, .keys = keys, .title = book.value("pair_title").toString()};
    }
    return names;
}

// The links a dry run would make, each as [the id here, theirs], for the other
// side to take as made: a dry run writes none of them, and both sides then
// take as if both were made (#158).
[[nodiscard]] QJsonArray linksJson(const BookPairing::Pairs &pairs) {
    QJsonArray links;
    for (const auto &[mine, theirs] : pairs.links)
        links << QJsonArray{mine, theirs};
    return links;
}

// After every operation here and of `incoming`, as a link a sync makes is
// once its clock has seen theirs (SyncManifest::observeManifest).
[[nodiscard]] qint64 afterAll(const Library &library, const QList<SpaceOperation> &incoming) {
    qint64 last = Spaces::lastHlc(library);
    for (const SpaceOperation &op : incoming)
        last = std::max(last, op.hlc);
    return last + 1;
}

// The links a dry run would make, here's and those `links` of theirs, as operations made now.
[[nodiscard]] QList<SpaceOperation> wouldLink(const Library &library, const BookPairing::Pairs &mine, const QJsonArray &links,
                                              const QList<SpaceOperation> &incoming) {
    BookPairing::Pairs pairs = mine;
    for (const auto &link : links)
        pairs.links << std::pair{link.toArray().at(1).toString(), link.toArray().at(0).toString()};
    return BookPairing::asOperations(library, pairs, afterAll(library, incoming));
}

[[nodiscard]] QJsonArray gidsOf(const Library &library) {
    QJsonArray gids;
    for (const Book &book : library.books())
        gids << Spaces::bookId(library, book.id);
    return gids;
}

// Here, at the start of a sync: what pairing the books of their `manifest`
// with this side's would link, the books kept apart named in `summary`; and,
// but in a dry run, paired with them and linked.
[[nodiscard]] BookPairing::Pairs pairWith(const Library &library, const QJsonObject &manifest, const QSet<QString> &theirGids,
                                          SyncSummary &summary) {
    const BookPairing::Pairs paired = Replication::pairs(library, namesIn(manifest.value("books").toArray()), theirGids);
    summary.titles.apart << paired.apart;
    summary.titles.apart.removeDuplicates();
    if (!summary.dryRun) {
        Spaces::pair(library, manifest.value("space").toString(), manifest.value("device").toString(), manifest.value("name").toString());
        BookPairing::record(library, paired);
    }
    return paired;
}

// The identifiers books here have of a book kept apart from them, removed
// (StrayIdentifiers, #164), or with `dryRun` only named, as the log `ops` says.
void unstray(Library &library, std::span<const SpaceOperation> ops, bool dryRun, SyncSummary &summary) {
    const StrayIdentifiers::Removed said = StrayIdentifiers::remove(library, ops, dryRun);
    summary.titles.identifiersRemoved << said.removed;
    summary.titles.identifiersRemoved.removeDuplicates();
    summary.titles.identifiersKept << said.doubtful;
    summary.titles.identifiersKept.removeDuplicates();
}

// The links the person asked to drop (SyncOptions::unlink, #160), each
// recorded here as not one book before the log goes, so it goes with it; a
// dry run only names them. And then, before anything else of the sync
// touches the two, what either has of the other goes (#164).
void unlink(Library &library, const SyncOptions &options, SyncSummary &summary) {
    for (const QString &title : options.unlink)
        summary.titles.unlinked << BookLinks::unlink(library, Replication::operations(library), title, options.dryRun);
    summary.titles.unlinked.removeDuplicates();
    if (!options.dryRun)
        unstray(library, Replication::operations(library), false, summary);
}

// Before what came in is compacted: the links it brought that are not of one
// book stay dropped (BookLinks::record), and are named among the books kept
// apart; and before anything is taken in, what books here have of a book kept
// apart goes. A dry run, of the log and `incoming`, only names that.
void keepApart(Library &library, const QList<SpaceOperation> &incoming, bool dryRun, SyncSummary &summary) {
    if (dryRun) {
        unstray(library, Replication::operations(library) + incoming, true, summary);
        return;
    }
    summary.titles.apart << BookLinks::record(library, Replication::operations(library));
    summary.titles.apart.removeDuplicates();
    unstray(library, Replication::operations(library), false, summary);
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
    add(summary.renamedThere, summary.titles.renamedThere, "renamed_here");
    summary.renamesHeld += there.value("renames_held").toInt();
    for (const auto &title : titles.value("apart").toArray())
        summary.titles.apart << title.toString();
    summary.titles.apart.removeDuplicates();
    for (const auto &[key, named] : {std::pair{"identifiers_removed", &summary.titles.identifiersRemoved},
                                     std::pair{"identifiers_kept", &summary.titles.identifiersKept}}) {
        for (const auto &identifier : titles.value(QLatin1StringView(key)).toArray())
            *named << identifier.toString();
        named->removeDuplicates();
    }
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
        std::ignore = SyncTake::take(library, ops,
                                     {.device = device,
                                      .details = SyncOptions::Details::Undecided,
                                      .acceptRenames = false,
                                      .dryRun = false,
                                      .remote = nullptr,
                                      .books = {},
                                      .held = held},
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
    unlink(library, options, summary);
    const QJsonArray books = manifest.value("books").toArray();
    const QString theirDevice = manifest.value("device").toString();
    QSet<QString> theirGids;
    for (const auto &book : books)
        theirGids.insert(book.toObject().value("gid").toString());
    const BookPairing::Pairs paired = pairWith(library, manifest, theirGids, summary);
    const QList<SpaceOperation> lacking = lackingThere(library, manifest);
    // A dry run links nothing: it says the links it would make, and takes
    // them as made, as the other side does. Nor does it pair: the person's key
    // goes with it only in a sync, when theirs is to become this one's (pairing.h).
    const QJsonObject request = with({{"space", Spaces::personalSpace(library)},
                                      {"device", Spaces::deviceId(library)},
                                      {"vector", Replication::toJson(Replication::vector(library))},
                                      {"annotations", Replication::toJson(Replication::annotationVector(library))},
                                      {"plugin_data", 1},
                                      {"ops", Replication::toJson(lacking, manifest.value("signs").toInt() >= 1)},
                                      {"books", Replication::toJson(Replication::unlinked(library, theirGids))},
                                      {"gids", gidsOf(library)},
                                      {"would_link", dryRun ? linksJson(paired) : QJsonArray()},
                                      {"accept_renames", options.acceptRenames},
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
    QList<SpaceOperation> incoming =
        KeptOps::keep(library,
                      admitted(library, Replication::operationsFrom(answer->value("ops").toArray()), manifest,
                               "%1 operations from " + remote.host() + " were refused and not taken in: %2.", summary),
                      dryRun);
    // They hold what they held, and now what was sent them.
    if (!dryRun)
        Compaction::rememberSeen(library, theirDevice,
                                 joined(Replication::vectorFrom(manifest.value("vector").toObject()),
                                        Replication::vectorFrom(request.value("vector").toObject())));
    const QList<qint64> taken = takePlaces(library, incoming, dryRun, summary.plans.here);
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
    if (dryRun)
        incoming << wouldLink(library, paired, answer->value("would_link").toArray(), incoming);
    keepApart(library, incoming, dryRun, summary);
    SyncTake::Taken described = SyncTake::take(library, incoming,
                                               {.device = theirDevice,
                                                .details = options.details,
                                                .acceptRenames = options.acceptRenames,
                                                .dryRun = dryRun,
                                                .remote = &remote,
                                                .books = books,
                                                .held = SyncTake::heldBy(books)},
                                               summary);
    if (dryRun) // what the other side's dry run would change of its books, for the guard to check (#187)
        summary.plans.there = SyncPlan::fromJson(answer->value("plan").toObject());
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
    QSet<QString> theirGids;
    for (const auto &gid : request.value("gids").toArray())
        theirGids.insert(gid.toString());
    const BookPairing::Pairs paired = Replication::pairs(library, Replication::booksFrom(request.value("books").toArray()), theirGids);
    if (!dryRun)
        Spaces::pair(library, request.value("space").toString(), theirDevice, request.value("name").toString());
    SyncSummary taken;
    taken.dryRun = dryRun;
    taken.titles.apart << paired.apart;
    QList<SpaceOperation> incoming = KeptOps::keep(library,
                                                   admitted(library, Replication::operationsFrom(request.value("ops").toArray()), request,
                                                            "%1 operations sent from here were refused there and not taken in: %2.", taken),
                                                   dryRun);
    // What they held when they asked: whether the answer reaches them, this side can't know.
    if (!dryRun)
        Compaction::rememberSeen(library, theirDevice, Replication::vectorFrom(request.value("vector").toObject()));
    // Linked once their operations are here, and before their places are taken: the links come
    // after those operations in time, as the other side's do once it saw this side's manifest, for
    // a link is judged by the titles before it (BookLinks).
    if (!dryRun) {
        Spaces::observeHlc(library, afterAll(library, incoming));
        BookPairing::record(library, paired);
    }
    const QList<qint64> changed = takePlaces(library, incoming, dryRun, taken.plans.here);
    if (dryRun)
        incoming << wouldLink(library, paired, request.value("would_link").toArray(), incoming);
    keepApart(library, incoming, dryRun, taken);
    // The side asked has no books of the other's to describe: it never fetches any.
    std::ignore = SyncTake::take(library, incoming,
                                 {.device = theirDevice,
                                  .details = SyncTake::detailsFrom(request.value("details").toString()),
                                  .acceptRenames = request.value("accept_renames").toBool(),
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
         {"would_link", dryRun ? linksJson(paired) : QJsonArray()},
         {"plan", dryRun ? taken.plans.here.toJson() : QJsonObject()},
         {"named", QJsonObject{{"details_taken", QJsonArray::fromStringList(taken.titles.detailsTaken)},
                               {"reading_taken", QJsonArray::fromStringList(taken.titles.readingTaken)},
                               {"removed_here", QJsonArray::fromStringList(taken.titles.removedHere)},
                               {"renamed_here", QJsonArray::fromStringList(taken.titles.renamedHere)},
                               {"apart", QJsonArray::fromStringList(taken.titles.apart)},
                               {"identifiers_removed", QJsonArray::fromStringList(taken.titles.identifiersRemoved)},
                               {"identifiers_kept", QJsonArray::fromStringList(taken.titles.identifiersKept)}}}},
        // Once this side is who it is to be: the person a pairing just made it may be theirs (#51).
        with(pairing, with(SpaceCarry::introduction(library), carried(library, request, request.value("carrying").toBool() && !dryRun))));
}
