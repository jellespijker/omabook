#include "synctake.h"

#include <QHash>
#include <QImageReader>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <tuple>

#include "graph.h"
#include "hlc.h"
#include "readingformat.h"
#include "replication.h"
#include "spacereducer.h"
#include "spaces.h"
#include "syncmanifest.h"

using namespace Qt::StringLiterals;

namespace {

// "generated" (which plugin made a book) never travels: only that plugin
// marks a book as its own, and the mark is what lets it fork and branch a
// story, which must never reach a book by a human author. A sync neither
// sends it nor takes it in.
bool travels(const QString &scheme) { return scheme.compare(QStringLiteral("generated"), Qt::CaseInsensitive) != 0; }

// The names elsewhere they have of a book and this side lacks. Names that came
// by sync keep the details' time, so they are no reason to send the details back.
bool fillNames(Library &library, const Book &mine, const QJsonObject &identifiers, bool dryRun) {
    bool changed = false;
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        if (travels(it.key()) && !mine.identifiers.contains(it.key())) {
            changed = true;
            if (!dryRun)
                library.setIdentifier(mine.id, it.key(), it.value().toVariant().toString(), SyncManifest::myDetailsHlc(library, mine));
        }
    return changed;
}

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
    ReducedState merged; // both sides' operations, as the log's rules merge them
    QHash<QString, QJsonObject> listed;
    QTemporaryDir scratch;

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
    const QString folder = QString::fromUtf8(taking.peer.remote->run("mktemp -d").output).trimmed();
    if (cover.isEmpty() || folder.isEmpty())
        return;
    const QString there = folder + u"/cover.jpg"_s;
    if (!taking.peer.remote->send(cover, there)
        || !taking.peer.remote
                ->omabook(
                    {"set", QString::number(listed.value("id").toInteger()), "--cover", there, "--details-at", QString::number(settled)})
                .ok())
        taking.summary.problems << mine.meta.title + ": its cover could not be given to " + taking.peer.remote->host() + ".";
    taking.peer.remote->run("rm -rf " + Remote::quote(folder));
}

// Of a book both sides hold: the details field by field by each field's time,
// as both sides decide alike, then what is said of its reading.
bool takeDetails(Taking &taking, const Book &here, const QJsonObject &theirs, const QJsonObject &listed) {
    const QString gid = taking.standsFor(Spaces::bookId(taking.library, here.id));
    const QJsonObject mine = Replication::bookView(taking.mine.books.value(gid));
    const bool covers = !listed.isEmpty() && coverHash(taking.library, here.id) != theirs.value("cover_hash").toString();
    const SyncManifest::MergedBookDetails merged = SyncManifest::decideDetails(here, mine, theirs, true, taking.peer.details, covers);
    // The details keep the time they had: the later of the two books' own details times.
    const qint64 settled = std::max(Hlc::normalize(here.detailsAt), Hlc::normalize(theirs.value("details_at").toInteger()));
    const QString &title = here.meta.title;
    if (merged.undecided && !merged.takeFields && !merged.sendFields)
        note(taking.summary.detailsUndecided, taking.summary.titles.detailsUndecided, title);
    if (merged.sendCover && !taking.peer.dryRun)
        giveCover(taking, here, listed, settled);
    if (!merged.takeFields)
        return false;
    BookMeta meta = merged.meta;
    if (merged.takeCover && !taking.peer.dryRun)
        meta.cover = fetchCover(taking, listed);
    QString error;
    if (!taking.peer.dryRun && !taking.library.setMeta(here.id, meta, &error, settled)) {
        taking.summary.problems << title + ": " + error;
        return false;
    }
    note(taking.summary.detailsTaken, taking.summary.titles.detailsTaken, title);
    return true;
}

void takeBook(Taking &taking, const Book &here) {
    const QString gid = taking.standsFor(Spaces::bookId(taking.library, here.id));
    const std::optional<QJsonObject> theirs = theirView(taking, gid);
    if (!theirs)
        return;
    const bool details = takeDetails(taking, here, *theirs, taking.listed.value(gid));
    const bool reading = SyncTake::fillReading(taking.library, here.id, *theirs, taking.peer.dryRun);
    if (reading)
        note(taking.summary.readingTaken, taking.summary.titles.readingTaken, here.meta.title);
    // The book's node in the knowledge graph says the same as the library.
    if ((details || reading) && !taking.peer.dryRun)
        Graph(taking.library).bookNode(here.id);
}

[[nodiscard]] QString fieldOf(const ReducedCollection &collection, const QString &field) { return collection.fields.value(field).value; }

[[nodiscard]] QJsonObject objectOf(const QString &text) { return QJsonDocument::fromJson(text.toUtf8()).object(); }

// A collection as the log says it is now, written over `row` when there is
// one, its books by the id they stand for here.
[[nodiscard]] Collection collectionFrom(const Taking &taking, const ReducedCollection &reduced, const Collection *row,
                                        const QHash<QString, qint64> &parents, const QHash<QString, qint64> &books) {
    Collection collection;
    collection.id = row ? row->id : 0;
    collection.name = fieldOf(reduced, u"name"_s);
    collection.parent = parents.value(fieldOf(reduced, u"parent"_s));
    collection.rule = fieldOf(reduced, u"rule"_s).trimmed();
    collection.shelf = objectOf(fieldOf(reduced, u"shelf"_s));
    collection.format = ReadingFormat::fitting(objectOf(fieldOf(reduced, u"format"_s)));
    collection.removed = SpaceReducer::isRemoved(reduced.alive, reduced.removed);
    collection.updated = std::max(reduced.alive.hlc, reduced.removed.hlc);
    // A removed one keeps only its name and place, as removing it leaves it.
    if (collection.removed) {
        collection.rule.clear();
        collection.shelf = {};
        collection.format = {};
        return collection;
    }
    for (auto book = reduced.books.cbegin(); book != reduced.books.cend(); ++book)
        if (const qint64 id = books.value(taking.standsFor(book.key())); id > 0 && book->value == "in"_L1)
            collection.included.insert(id);
        else if (id > 0 && book->value == "out"_L1)
            collection.excluded.insert(id);
    return collection;
}

[[nodiscard]] bool sameCollection(const Collection &row, const Collection &log) {
    if (row.removed && log.removed)
        return true;
    return std::tie(row.name, row.parent, row.rule, row.removed, row.included, row.excluded)
               == std::tie(log.name, log.parent, log.rule, log.removed, log.included, log.excluded)
           && row.shelf == log.shelf && row.format == log.format;
}

// The collections as the log says them, both sides' operations merged by its
// rules (each field the latest; of the books in it by hand, an add over a
// concurrent remove): each that the log says otherwise than here is written
// as it says, a parent before what is in it. One removed that this side never
// had is not made.
void takeCollections(Taking &taking) {
    QHash<QString, qint64> books;
    for (const Book &book : taking.library.books())
        books.insert(taking.standsFor(Spaces::bookId(taking.library, book.id)), book.id);
    QHash<QString, Collection> rows;
    QHash<QString, qint64> ids;
    for (const Collection &collection : taking.library.collections(true)) {
        const QString gid = Spaces::collectionId(taking.library, collection.id);
        rows.insert(gid, collection);
        ids.insert(gid, collection.id);
    }
    QStringList order = taking.merged.collections.keys();
    std::ranges::stable_sort(order, {}, [](const QString &gid) { return gid.count(u'/'); });
    const Library::Batch batch(taking.library);
    for (const QString &gid : std::as_const(order)) {
        const ReducedCollection &reduced = taking.merged.collections[gid];
        const auto row = rows.constFind(gid);
        const Collection log = collectionFrom(taking, reduced, row == rows.cend() ? nullptr : &*row, ids, books);
        if ((row == rows.cend() && log.removed) || log.name.isEmpty() || (row != rows.cend() && sameCollection(*row, log)))
            continue;
        ++taking.summary.collectionsTaken;
        if (taking.peer.dryRun)
            continue;
        if (const std::optional<qint64> stored = taking.library.storeCollection(log)) {
            Spaces::setGlobalId(taking.library, u"collection"_s, *stored, gid);
            ids.insert(gid, *stored);
        }
    }
}

} // namespace

QJsonArray SyncTake::take(Library &library, std::span<const SpaceOperation> incoming, const Peer &peer, SyncSummary &summary) {
    QList<SpaceOperation> ops = Replication::operations(library);
    if (peer.dryRun)
        ops.append(QList<SpaceOperation>(incoming.begin(), incoming.end()));
    Taking taking{.library = library,
                  .peer = peer,
                  .summary = summary,
                  .canonical = SpaceReducer::sameAs(ops),
                  .mine = SpaceReducer::reduce(ops, Spaces::deviceId(library)),
                  .theirs = SpaceReducer::reduce(ops, peer.device),
                  .merged = SpaceReducer::reduce(ops),
                  .listed = {},
                  .scratch = {}};
    for (const auto &value : peer.books)
        taking.listed.insert(taking.standsFor(value.toObject().value("gid").toString()), value.toObject());
    for (const Book &here : library.books())
        takeBook(taking, here);
    takeCollections(taking);
    // Their books, each as the log says they hold it, for what this side lacks.
    QJsonArray described;
    for (const auto &value : peer.books) {
        QJsonObject book = value.toObject();
        if (const std::optional<QJsonObject> view = theirView(taking, taking.standsFor(book.value("gid").toString())))
            for (auto field = view->begin(); field != view->end(); ++field)
                book.insert(field.key(), field.value());
        described << book;
    }
    return described;
}

bool SyncTake::fillReading(Library &library, qint64 id, const QJsonObject &theirs, bool dryRun) {
    const std::optional<Book> mine = library.book(id);
    if (!mine)
        return false;
    bool changed = false;
    const QString shelf = mine->shelf.isEmpty() ? theirs.value("shelf").toString() : mine->shelf;
    const int rating = mine->rating > 0 ? mine->rating : theirs.value("rating").toInt();
    const QString finished = mine->finished.isEmpty() ? theirs.value("finished").toString() : mine->finished;
    if (shelf != mine->shelf || rating != mine->rating || finished != mine->finished) {
        changed = true;
        if (!dryRun)
            library.setReading(id, shelf, rating, finished);
    }
    if (!mine->paper && theirs.value("paper").toBool()) {
        changed = true;
        if (!dryRun)
            library.setPaper(id, true);
    }
    return fillNames(library, *mine, theirs.value("identifiers").toObject(), dryRun) || changed;
}

QStringList SyncTake::readingArguments(qint64 id, const Book &book, std::optional<qint64> detailsAt) {
    QStringList arguments{"set",      QString::number(id),          "--status", book.shelf.isEmpty() ? QStringLiteral("none") : book.shelf,
                          "--rating", QString::number(book.rating), "--paper",  book.paper ? QStringLiteral("yes") : QStringLiteral("no")};
    if (!book.finished.isEmpty())
        arguments << "--finished" << book.finished;
    for (auto it = book.identifiers.begin(); it != book.identifiers.end(); ++it)
        if (const QString value = it.value().toVariant().toString(); !value.isEmpty() && travels(it.key()))
            arguments << "--identifier" << it.key() + "=" + value;
    if (detailsAt)
        arguments << "--details-at" << QString::number(*detailsAt);
    return arguments;
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
