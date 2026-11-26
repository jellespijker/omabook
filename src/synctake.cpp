#include "synctake.h"

#include <QHash>
#include <QImageReader>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <compare>
#include <functional>
#include <optional>
#include <tuple>

#include "bookidentity.h"
#include "booklinks.h"
#include "changelog.h"
#include "graph.h"
#include "hlc.h"
#include "replication.h"
#include "spacereducer.h"
#include "spaces.h"
#include "strayidentifiers.h"
#include "synccopies.h"
#include "syncmanifest.h"
#include "syncrenames.h"
#include "synctakeannotations.h"
#include "synctakecollections.h"
#include "synctakegraph.h"

using namespace Qt::StringLiterals;

namespace {

void note(int &count, QStringList &titles, const QString &title) {
    ++count;
    titles << title;
}

// One take in progress: the log as each side's device wrote it, and the other
// side's manifest books (for their covers), all by the id a book stands for.
struct Taking {
    Library &library;           // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    const SyncTake::Peer &peer; // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    SyncSummary &summary;       // NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members): short-lived stack context
    QHash<QString, QString> canonical;
    ReducedState mine;
    ReducedState theirs;
    ReducedState theirsByName; // the same, each book by the id its own operations name, links not followed
    ReducedState merged;       // both sides' operations, as the log's rules merge them
    QHash<QString, QJsonObject> listed;
    QTemporaryDir scratch;
    QSet<qint64> gone;      // the books here the other side removed
    QSet<QString> weak;     // the books paired on less than their global id (BookLinks::weaklyPaired)
    StrayIdentifiers stray; // the identifiers books have of one kept apart

    [[nodiscard]] QString standsFor(const QString &gid) const { return canonical.value(gid, gid); }
};

// Their book as the log says their device holds it, or nothing when it holds
// none of it (never had it, or removed it).
[[nodiscard]] std::optional<QJsonObject> theirView(const Taking &taking, const QString &gid) {
    const auto their = taking.theirs.books.constFind(gid);
    if (their == taking.theirs.books.cend() || !their->fields.contains(u"title"_s) || SpaceReducer::isRemoved(their->alive, their->removed))
        return std::nullopt;
    return Replication::bookView(*their);
}

[[nodiscard]] QString coverHash(const Library &library, qint64 id) {
    const QString file = library.coverPath(id);
    return file.isEmpty() ? QString() : Library::hashFile(file);
}

// Their cover, fetched from where their manifest says it is.
[[nodiscard]] QImage fetchCover(const Taking &taking, const QJsonObject &listed) {
    const QString there = listed.value("cover").toString();
    const QString here = taking.scratch.filePath(u"cover-"_s + QString::number(listed.value("id").toInteger()) + u".jpg"_s);
    return !there.isEmpty() && taking.peer.remote->fetch(there, here) ? QImageReader(here).read() : QImage();
}

// This side's cover, given to them with the time the details settled at.
void giveCover(const Taking &taking, const Book &mine, const QJsonObject &listed, qint64 settled) {
    const QString cover = taking.library.coverPath(mine.id);
    const QString folder = taking.peer.remote->makeFolder();
    if (cover.isEmpty() || folder.isEmpty())
        return;
    const QString there = folder + u"/cover.jpg"_s;
    if (!taking.peer.remote->send(cover, there)
        || !taking.peer.remote
                ->omabook(
                    {"set", QString::number(listed.value("id").toInteger()), "--cover", there, "--details-at", QString::number(settled)})
                .ok())
        taking.summary.problems << mine.meta.title + ": its cover could not be given to " + taking.peer.remote->host() + ".";
    taking.peer.remote->dropFolder(folder);
}

// The time a book's details keep: the later of the two books' own details
// times, to the clock's counter as each side's log says it. In seconds alone,
// a field taken from a second the clock ticked in more than once would be
// older here than there, and taken again at every sync (#106).
[[nodiscard]] qint64 settledAt(const QList<Book> &copies, const QJsonObject &mine, const QJsonObject &theirs) {
    qint64 settled = std::max(SyncManifest::theirDetailsHlc(mine), SyncManifest::theirDetailsHlc(theirs));
    for (const Book &copy : copies)
        settled = std::max(settled, Hlc::normalize(copy.detailsAt));
    return settled;
}

// Whether the book `gid` stands for is paired on less than its global id (#158).
[[nodiscard]] bool pairedWeakly(const Taking &taking, const QString &gid) {
    return taking.weak.contains(gid) || !BookIdentity::strongId(gid);
}

// Gives the copies here the details decided, as `behind` says which: every
// copy what comes from there; and, of two, the one whose words the log says
// otherwise (the other copy's are newer) the book's as the log says them,
// once that is decided (#154). Whether any was given them.
bool giveCopies(Taking &taking, const QList<Book> &copies, const SyncManifest::MergedBookDetails &merged, const QJsonObject &listed,
                qint64 settled, const std::function<bool(const Book &)> &behind) {
    if (std::ranges::none_of(copies, behind))
        return false;
    const QString &title = copies.first().meta.title;
    BookMeta meta = merged.meta;
    if (merged.takeCover && !taking.peer.dryRun && taking.peer.remote != nullptr)
        meta.cover = fetchCover(taking, listed);
    const bool weak = pairedWeakly(taking, taking.standsFor(Spaces::bookId(taking.library, copies.first().id)));
    for (const Book &copy : copies) {
        if (behind(copy))
            taking.summary.plans.here.details(taking.library, copy, merged.meta, merged.takeCover, weak);
        // Without a cover to write and with its words, a copy is given nothing:
        // on the side asked, the cover comes from the side that reaches it (#168).
        const bool writes = behind(copy) && (!meta.cover.isNull() || !SyncCopies::sameWords(copy.meta, meta));
        QString error;
        if (writes && !taking.peer.dryRun && !taking.library.setMeta(copy.id, meta, &error, settled)) {
            taking.summary.problems << title + ": " + error;
            return false;
        }
    }
    note(taking.summary.detailsTaken, taking.summary.titles.detailsTaken, title);
    return true;
}

// Of two covers, one side's and none: whatever the times say, the side
// without one takes it, as no picture can be taken. Both sides decide this
// alike, whichever starts the sync, and once given it settles (#168).
void preferACover(SyncManifest::MergedBookDetails &merged, bool mine, bool theirs) {
    if (mine == theirs)
        return;
    merged.takeCover = theirs;
    merged.sendCover = mine;
    merged.takeFields = merged.takeFields || theirs;
    merged.sendFields = merged.sendFields || mine;
}

// Whether the copies here of the book `gid` stands for are given its details
// as the log says them, `mine` titled so, once they are decided (#154): never
// copies an identifier folds together, which may be two books (#158, #164).
[[nodiscard]] bool settlesCopies(const Taking &taking, const QList<Book> &copies, const QString &gid,
                                 const SyncManifest::MergedBookDetails &merged, const QString &mine, qint64 settled) {
    return copies.size() > 1 && settled > 0 && !merged.undecided && !mine.isEmpty() && !pairedWeakly(taking, gid);
}

// Of a book both sides hold: the details field by field by each field's time,
// as both sides decide alike, then what is said of its reading. `copies` are
// the books here that are this one book to the log, the one that speaks for
// it first (SyncCopies::of); `listed` is the copy their manifest speaks for it by.
bool takeDetails(Taking &taking, const QList<Book> &copies, const QJsonObject &theirs, const QJsonObject &listed) {
    const Book &here = copies.first();
    const QString gid = taking.standsFor(Spaces::bookId(taking.library, here.id));
    const QJsonObject mine = Replication::bookView(taking.mine.books.value(gid));
    // The covers compared are the pictures of the copies that speak for the
    // book on each side, so both sides compare the same two (#154): theirs as
    // their manifest lists it, else, on the side asked, as their log says it,
    // so the side asked decides the cover as the side that asks does (#168).
    const QString myCover = coverHash(taking.library, here.id);
    const QString theirCover = (listed.isEmpty() ? theirs : listed).value("cover_hash").toString();
    const bool covers = myCover != theirCover;
    SyncManifest::MergedBookDetails merged = SyncManifest::decideDetails(here, mine, theirs, true, taking.peer.details, covers);
    if (covers)
        preferACover(merged, !myCover.isEmpty(), !theirCover.isEmpty());
    if (pairedWeakly(taking, gid))
        SyncRenames::hold(here, theirs, {.details = taking.peer.details, .accept = taking.peer.acceptRenames}, merged, taking.summary);
    const qint64 settled = settledAt(copies, mine, theirs);
    if (merged.undecided && !merged.takeFields && !merged.sendFields)
        note(taking.summary.detailsUndecided, taking.summary.titles.detailsUndecided, here.meta.title);
    if (merged.sendCover && !taking.peer.dryRun && taking.peer.remote != nullptr)
        giveCover(taking, here, listed, settled);
    const bool asLogged = settlesCopies(taking, copies, gid, merged, mine.value("title").toString(), settled);
    // A copy that has the details decided already, and no cover to take, is
    // given nothing and counted for nothing: the logs may say them otherwise
    // (an older copy's id the log folds into this book), but a write of what
    // it holds changes nothing, and the next sync would decide the same (#160).
    return giveCopies(taking, copies, merged, listed, settled, [&](const Book &copy) {
        const bool behind = !SyncCopies::sameWords(copy.meta, merged.meta);
        return (merged.takeFields && (merged.takeCover || behind)) || (asLogged && behind);
    });
}

// When the other side removed what names this book here (`name`: a file's
// hash, or an entry key), as the operations their device wrote say; none
// when they hold it, or never had it. What they removed is what they removed
// by that name, as the manifest sync decides it: never a tombstone of
// another copy that a same-as link put beside it (a duplicate removed there,
// #58).
[[nodiscard]] std::optional<qint64> removedThere(const Taking &taking, const QString &name) {
    if (taking.peer.held.contains(name))
        return std::nullopt;
    const auto book = taking.theirsByName.books.constFind(Spaces::removalGlobalId(name));
    if (book == taking.theirsByName.books.cend() || !SpaceReducer::isRemoved(book->alive, book->removed))
        return std::nullopt;
    return SpaceReducer::fieldHlc(*book, u"removed"_s);
}

// The books here that the other side removed, all of each, as the manifest
// sync decides it: removed here too, at the time they were removed there,
// unless they changed here since; and either way never given back to them.
void takeRemovals(Taking &taking, const QList<Book> &books) {
    for (const Book &here : books) {
        QStringList hashes;
        for (const FormatFile &file : taking.library.formatFiles(here.id))
            hashes << file.hash;
        const std::optional<qint64> when = SyncManifest::removedWhen(hashes, hashes.isEmpty() ? Library::entryKeys(here) : QStringList(),
                                                                     [&](const QString &name) { return removedThere(taking, name); });
        if (!when)
            continue;
        taking.gone.insert(here.id);
        if (!std::is_lteq(Hlc::compare(SyncManifest::lastChangeHlc(taking.library, here), *when)))
            continue;
        taking.summary.plans.here.removal(taking.library, here);
        const auto removed = taking.peer.dryRun ? std::expected<void, QString>() : taking.library.remove(here.id, Hlc::seconds(*when));
        if (removed)
            note(taking.summary.removedHere, taking.summary.titles.removedHere, here.meta.title);
        else
            taking.summary.problems << here.meta.title + " could not be removed here: " + removed.error();
    }
}

// The identifiers a copy here, of the book `standsFor`, takes none of:
// a book's kept apart from it, and those whose book can't be told (#166).
[[nodiscard]] QSet<QString> strayIn(const Taking &taking, const Book &copy, const QString &standsFor) {
    const QString gid = Spaces::bookId(taking.library, copy.id);
    return taking.stray.of(copy, gid, standsFor) | taking.stray.doubtful(copy, gid, standsFor);
}

// The fields of a book's reading their device said later than this one's,
// of a book paired by its global id: a field's latest wins (ADR 0001), of one
// book (ADR 0009, #213). Never an empty one: no stars are written whenever a
// shelf is, so a clear can't be told from saying nothing.
[[nodiscard]] QSet<QString> newerReading(const Taking &taking, const QString &gid) {
    QSet<QString> newer;
    const ReducedBook mine = taking.mine.books.value(gid);
    const ReducedBook theirs = taking.theirs.books.value(gid);
    for (const QString &field : {u"shelf"_s, u"rating"_s, u"finished"_s})
        if (const QString said = theirs.fields.value(field).value;
            !pairedWeakly(taking, gid) && said != ""_L1 && said != "0"_L1
            && SpaceReducer::fieldHlc(theirs, field) > SpaceReducer::fieldHlc(mine, field))
            newer.insert(field);
    return newer;
}

// One book both sides hold, decided once for all its copies here, and counted once.
void takeBook(Taking &taking, const QList<Book> &copies) {
    const Book &here = copies.first();
    const QString gid = taking.standsFor(Spaces::bookId(taking.library, here.id));
    const std::optional<QJsonObject> theirs = theirView(taking, gid);
    if (!theirs)
        return;
    const bool details = takeDetails(taking, copies, *theirs, taking.listed.value(gid));
    const QSet<QString> newer = newerReading(taking, gid);
    bool reading = false;
    for (const Book &copy : copies)
        reading = SyncTake::fillReading(taking.library, copy.id, *theirs, taking.peer.dryRun,
                                        {.take = !pairedWeakly(taking, gid), .stray = strayIn(taking, copy, gid), .newerReading = newer},
                                        &taking.summary.plans.here)
                  || reading;
    if (reading)
        note(taking.summary.readingTaken, taking.summary.titles.readingTaken, here.meta.title);
    // The book's node in the knowledge graph says the same as the library.
    if ((details || reading) && !taking.peer.dryRun)
        for (const Book &copy : copies)
            Graph(taking.library).bookNode(copy.id);
}

// The name each thing of the graph was given, by its id (before same-as links fold them).
[[nodiscard]] QHash<QString, QString> thingNames(std::span<const SpaceOperation> ops) {
    QHash<QString, std::pair<qint64, QString>> latest;
    for (const SpaceOperation &op : ops)
        if (op.entity == "node"_L1 && op.field == "name"_L1 && op.hlc >= latest.value(op.id).first)
            latest.insert(op.id, {op.hlc, op.value});
    QHash<QString, QString> names;
    for (auto it = latest.cbegin(); it != latest.cend(); ++it)
        names.insert(it.key(), it->second);
    return names;
}

// The books here by the id they stand for.
[[nodiscard]] QHash<QString, qint64> booksHere(const Taking &taking) {
    QHash<QString, qint64> books;
    for (const Book &book : taking.library.books())
        books.insert(taking.standsFor(Spaces::bookId(taking.library, book.id)), book.id);
    return books;
}

// A book here that this computer's own log says is removed: a same-as link
// put it beside a copy removed here, by the id that copy had (a duplicate
// without a file, #58), and the copy's tombstone stood for it too. It is
// here, so the log says so again, at once, of each id this computer removed
// it by: for every computer to take, and before that tombstone sends the
// book's details nowhere or removes another computer's copy.
[[nodiscard]] bool keepHeld(const Library &library, const QList<SpaceOperation> &ops) {
    const QString device = Spaces::deviceId(library);
    const QHash<QString, QString> canonical = SpaceReducer::sameAs(ops);
    const ReducedState mine = SpaceReducer::reduce(ops, device);
    QSet<QString> removedHeld;
    for (const Book &book : library.books()) {
        const QString gid = Spaces::bookId(library, book.id);
        const auto reduced = mine.books.constFind(canonical.value(gid, gid));
        if (reduced != mine.books.cend() && SpaceReducer::isRemoved(reduced->alive, reduced->removed))
            removedHeld.insert(reduced.key());
    }
    if (removedHeld.isEmpty())
        return false;
    const ReducedState byName = SpaceReducer::reduceByName(ops, device);
    const Library::Batch batch(library);
    for (auto book = byName.books.cbegin(); book != byName.books.cend(); ++book)
        if (removedHeld.contains(canonical.value(book.key(), book.key())) && SpaceReducer::isRemoved(book->alive, book->removed))
            ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                        .entity = u"book"_s,
                                        .id = book.key(),
                                        .field = u"removed"_s,
                                        .value = u"0"_s,
                                        .hlc = std::nullopt});
    return true;
}

} // namespace

SyncTake::Taken SyncTake::take(Library &library, std::span<const SpaceOperation> incoming, const Peer &peer, SyncSummary &summary) {
    summary.plans.here.said = true;
    QList<SpaceOperation> ops = Replication::operations(library);
    if (peer.dryRun)
        ops.append(QList<SpaceOperation>(incoming.begin(), incoming.end()));
    else if (keepHeld(library, ops))
        ops = Replication::operations(library);
    // No link folds different books into one, as a wrong one made before did (#154).
    ops = BookLinks::trusted(ops);
    Taking taking{.library = library,
                  .peer = peer,
                  .summary = summary,
                  .canonical = SpaceReducer::sameAs(ops),
                  .mine = SpaceReducer::reduce(ops, Spaces::deviceId(library)),
                  .theirs = SpaceReducer::reduce(ops, peer.device),
                  .theirsByName = SpaceReducer::reduceByName(ops, peer.device),
                  .merged = SpaceReducer::reduce(ops),
                  .listed = {},
                  .scratch = {},
                  .gone = {},
                  .weak = BookLinks::weaklyPaired(ops),
                  .stray = StrayIdentifiers(ops)};
    for (const auto &value : peer.books) {
        const QJsonObject book = value.toObject();
        const QString gid = taking.standsFor(book.value("gid").toString());
        if (const auto listed = taking.listed.constFind(gid); listed == taking.listed.cend() || SyncCopies::speaksBefore(book, *listed))
            taking.listed.insert(gid, book);
    }
    const QList<Book> books = library.books();
    takeRemovals(taking, books);
    for (QList<Book> copies : SyncCopies::of(library, books, taking.canonical)) {
        copies.removeIf([&taking](const Book &book) { return taking.gone.contains(book.id); });
        if (!copies.isEmpty())
            takeBook(taking, copies);
    }
    summary.collectionsTaken +=
        SyncTakeCollections::take(library, taking.merged.collections, booksHere(taking), taking.canonical, peer.dryRun);
    summary.annotationsTaken += SyncTakeAnnotations::take(library, taking.merged.annotations, booksHere(taking), taking.canonical,
                                                          peer.dryRun, summary.plans.here.annotationsMoved);
    // A dry run leaves the graph be, as it always did.
    if (!peer.dryRun)
        summary.graphTaken += SyncTakeGraph::take(
            library, taking.merged, booksHere(taking), SpaceReducer::sameAs(ops, "node"_L1),
            {.theirs = QSet<QString>(taking.theirs.nodes.keyBegin(), taking.theirs.nodes.keyEnd()), .names = thingNames(ops)});
    // Their books, each as the log says they hold it, for what this side lacks.
    Taken taken{.books = {}, .gone = taking.gone};
    for (const auto &value : peer.books) {
        QJsonObject book = value.toObject();
        if (const std::optional<QJsonObject> view = theirView(taking, taking.standsFor(book.value("gid").toString())))
            for (auto field = view->begin(); field != view->end(); ++field)
                book.insert(field.key(), field.value());
        taken.books << book;
    }
    return taken;
}

QSet<QString> SyncTake::heldBy(const Library &library) {
    QSet<QString> held;
    // Of every book its entry keys too, a file or not: a book without a file
    // that has one of them elsewhere is this one, as the manifest sync finds
    // it, and was never removed here (#58).
    for (const Book &book : library.books()) {
        for (const FormatFile &file : library.formatFiles(book.id))
            held.insert(file.hash);
        // And the file one came in as, before details were written into it (#74).
        for (const FormatFile &file : library.formatFiles(book.id))
            if (!file.original.isEmpty())
                held.insert(file.original);
        for (const QString &key : Library::entryKeys(book))
            held.insert(key);
    }
    return held;
}

QSet<QString> SyncTake::heldBy(const QJsonArray &books) {
    QSet<QString> held;
    for (const auto &value : books) {
        for (const auto &file : value.toObject().value("files").toArray())
            held.insert(file.toObject().value("hash").toString());
        for (const auto &key : value.toObject().value("keys").toArray())
            held.insert(key.toString());
    }
    return held;
}

QString SyncTake::otherSide(SyncOptions::Details details) {
    switch (details) {
    case SyncOptions::Details::Mine:
        return u"theirs"_s;
    case SyncOptions::Details::Theirs:
        return u"mine"_s;
    case SyncOptions::Details::Undecided:
        break;
    }
    return {};
}

SyncOptions::Details SyncTake::detailsFrom(const QString &name) {
    if (name == "mine"_L1)
        return SyncOptions::Details::Mine;
    if (name == "theirs"_L1)
        return SyncOptions::Details::Theirs;
    return SyncOptions::Details::Undecided;
}
