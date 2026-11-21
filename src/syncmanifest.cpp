#include "syncmanifest.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <initializer_list>

#include "changelog.h"
#include "collections.h"
#include "graphsync.h"
#include "hlc.h"
#include "spacereplay.h"
#include "spaces.h"
#include "synclog.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] qint64 maxOf(const QHash<QString, qint64> &hlcs, std::initializer_list<QLatin1StringView> keys, qint64 fallback = 0) {
    qint64 best = fallback;
    for (const QLatin1StringView key : keys)
        best = std::max(best, hlcs.value(QString(key)));
    return best;
}

[[nodiscard]] QJsonObject bookFieldHlcJson(const Book &book, const QHash<QString, qint64> &hlcs) {
    if (book.detailsAt == 0)
        return {};
    const qint64 base = Hlc::normalize(book.detailsAt);
    const auto field = [&](QLatin1StringView key) { return hlcs.value(QString(key), base); };
    return {{"title", field("title"_L1)},
            {"authors", field("authors"_L1)},
            {"series", std::max(field("series"_L1), field("series_index"_L1))},
            {"publisher", field("publisher"_L1)},
            {"language", field("language"_L1)},
            {"isbn", field("isbn"_L1)},
            {"published", field("published"_L1)},
            {"description", field("description"_L1)},
            {"tags", field("tags"_L1)},
            {"cover", std::max(field("has_cover"_L1), field("cover_hash"_L1))}};
}

[[nodiscard]] QJsonObject toJson(const Progress &progress, qint64 hlc) {
    return {{"section", progress.place.section},
            {"fraction", progress.place.fraction},
            {"percent", progress.percent},
            {"updated", progress.updated},
            {"hlc", hlc}};
}

[[nodiscard]] qint64 stampedOrNormalizedHlc(qint64 seconds, qint64 loggedHlc) noexcept {
    if (seconds == 0)
        return 0;
    return Hlc::seconds(loggedHlc) == seconds ? loggedHlc : Hlc::normalize(seconds);
}

void decideField(SyncManifest::MergedBookDetails &out, qint64 myHlc, qint64 theirHlc, bool current, SyncOptions::Details choice,
                 const std::function<void()> &take) {
    if (std::is_eq(Hlc::compare(myHlc, theirHlc))) {
        if (choice == SyncOptions::Details::Mine) {
            if (current) {
                out.sendFields = true;
                out.sendHlc = std::max(out.sendHlc, myHlc);
            }
        } else if (choice == SyncOptions::Details::Theirs) {
            take();
            out.takeFields = true;
            out.takeHlc = std::max(out.takeHlc, theirHlc);
        } else {
            out.undecided = true;
        }
        return;
    }
    if (std::is_gt(Hlc::compare(theirHlc, myHlc))) {
        take();
        out.takeFields = true;
        out.takeHlc = std::max(out.takeHlc, theirHlc);
    } else if (current) {
        out.sendFields = true;
        out.sendHlc = std::max(out.sendHlc, myHlc);
    }
}

[[nodiscard]] qint64 fieldHlc(qint64 baseHlc, const QJsonObject &fieldObj, QLatin1StringView key) {
    return baseHlc == 0 ? 0 : Hlc::normalize(fieldObj.value(key).toInteger(baseHlc));
}

[[nodiscard]] bool coversDiffer(const Library &library, qint64 bookId, const QJsonObject &theirs) {
    const QString theirCover = theirs.value("cover_hash").toString();
    const QString myCoverFile = library.coverPath(bookId);
    const QString myCover = myCoverFile.isEmpty() ? QString() : Library::hashFile(myCoverFile);
    return theirCover != myCover;
}

// Where no time says which cover is newer, two files of a cover are no reason
// to call a book undecided: the same picture saved on two computers (as a sync
// before the log saved what it fetched) is seldom the same bytes (#59). As
// before times were kept, only the words are compared then, and the cover goes
// with words settled by `--details mine` or `--details theirs`.
void mergeCoverField(SyncManifest::MergedBookDetails &out, qint64 myCoverAt, qint64 theirCoverAt, SyncOptions::Details choice) {
    const bool settled = out.takeFields || out.sendFields;
    if (std::is_eq(Hlc::compare(myCoverAt, theirCoverAt)) && (choice == SyncOptions::Details::Undecided || !settled))
        return;
    decideField(out, myCoverAt, theirCoverAt, true, choice, [&] { out.takeCover = true; });
    if (!out.takeCover && out.sendFields && std::is_gteq(Hlc::compare(myCoverAt, theirCoverAt)))
        out.sendCover = true;
}

// A book as an Omabook that speaks the log needs it: what names it (its global
// id, its entry keys), its files and cover to fetch. Its details, its reading,
// its place and its removal are the log's. The keys are a book's with a file
// too, as an older Omabook makes them of its details: a book without a file
// there that has one of them is this one, never sent here nor removed there (#58).
QJsonObject logPeerBook(const Library &library, const Book &book, const QJsonArray &files) {
    const QString cover = library.coverPath(book.id);
    return {{"id", book.id},  {"gid", Spaces::bookId(library, book.id)},
            {"files", files}, {"keys", QJsonArray::fromStringList(Library::entryKeys(book))},
            {"cover", cover}, {"cover_hash", cover.isEmpty() ? QString() : Library::hashFile(cover)}};
}

// A book as an Omabook from before the log needs it: everything of it.
QJsonObject olderPeerBook(const Library &library, const Book &book, const QJsonArray &files) {
    QJsonObject object = book.toJson();
    object.insert("files", files);
    const QString cover = library.coverPath(book.id);
    object.insert("cover", cover);
    object.insert("cover_hash", cover.isEmpty() ? QString() : Library::hashFile(cover));
    object.insert("added_at", book.added);
    const QHash<QString, qint64> fieldHlcs = SpaceReplay::bookFieldHlcs(library, book.id);
    const Progress prog = library.progress(book.id);
    object.insert("progress", toJson(prog, stampedOrNormalizedHlc(prog.updated, fieldHlcs.value(u"progress"_s))));
    object.insert("shelf", book.shelf);
    object.insert("paper", book.paper);
    const qint64 detailsHlc =
        book.detailsAt == 0 ? 0
                            : maxOf(fieldHlcs,
                                    {"title"_L1, "authors"_L1, "series"_L1, "series_index"_L1, "publisher"_L1, "language"_L1, "isbn"_L1,
                                     "published"_L1, "description"_L1, "tags"_L1, "identifiers"_L1, "has_cover"_L1, "cover_hash"_L1},
                                    Hlc::normalize(book.detailsAt));
    const qint64 readingHlc =
        book.readingAt == 0 ? 0 : maxOf(fieldHlcs, {"shelf"_L1, "rating"_L1, "finished"_L1, "paper"_L1}, Hlc::normalize(book.readingAt));
    object.insert("details_hlc", detailsHlc);
    object.insert("reading_hlc", readingHlc);
    object.insert("field_hlc", bookFieldHlcJson(book, fieldHlcs));
    return object;
}

// When each book or name was removed here, for an older Omabook to decide whether to remove it there.
QJsonArray removals(const Library &library) {
    QJsonArray removals;
    for (const Library::Removal &removal : library.removals()) {
        const qint64 loggedHlc = SpaceReplay::fieldHlc(library, u"book"_s, Spaces::removalGlobalId(removal.key), u"removed"_s);
        const qint64 hlc = Hlc::seconds(loggedHlc) == removal.at ? loggedHlc : Hlc::normalize(removal.at);
        removals << QJsonObject{{"key", removal.key}, {"at", removal.at}, {"hlc", hlc}};
    }
    return removals;
}

} // namespace

QJsonObject libraryManifest(const Library &library, ManifestFor peer) {
    const bool log = peer == ManifestFor::LogPeer;
    ChangeLog::reconcile(library);
    QJsonArray books;
    for (const Book &book : library.books()) {
        QJsonArray files;
        for (const FormatFile &file : library.formatFiles(book.id)) {
            QJsonObject listed{{"format", file.format}, {"path", file.path}, {"hash", file.hash}, {"size", file.size}};
            // The hash it came in with, which the other side may still know it by (#74).
            if (!file.original.isEmpty())
                listed.insert("original", file.original);
            files << listed;
        }
        books << (log ? logPeerBook(library, book, files) : olderPeerBook(library, book, files));
    }
    QJsonObject manifest{
        {"version", manifestVersion}, {"hlc", Spaces::lastHlc(library)}, {"omabook", QStringLiteral(OMABOOK_VERSION)}, {"books", books}};
    // What travels by the log is left out for an Omabook that speaks it.
    if (!log) {
        manifest.insert("removed", QJsonArray::fromStringList(library.removedHashes()));
        manifest.insert("removals", removals(library));
        manifest.insert("collections", Collections::manifest(library));
        manifest.insert("graph", GraphSync::bundle(library));
    }
    if (log) {
        manifest.insert("log", 1);
        const QJsonObject introduction = SyncLog::introduction(library);
        for (auto it = introduction.begin(); it != introduction.end(); ++it)
            manifest.insert(it.key(), it.value());
    }
    return manifest;
}

QJsonObject SyncSummary::toJson() const {
    const auto list = [](const QStringList &titles) { return QJsonArray::fromStringList(titles); };
    QJsonObject result{{"received", received},
                       {"sent", sent},
                       {"places_taken", placesTaken},
                       {"places_sent", placesSent},
                       {"details_taken", detailsTaken},
                       {"details_sent", detailsSent},
                       {"reading_taken", readingTaken},
                       {"reading_sent", readingSent},
                       {"removed_here", removedHere},
                       {"removed_there", removedThere},
                       {"details_undecided", detailsUndecided},
                       {"collections_taken", collectionsTaken},
                       {"collections_sent", collectionsSent},
                       {"graph_taken", graphTaken},
                       {"graph_sent", graphSent},
                       {"annotations_taken", annotationsTaken},
                       {"annotations_sent", annotationsSent},
                       {"refused", refused},
                       {"plugin_files_taken", pluginFilesTaken},
                       {"plugin_files_sent", pluginFilesSent},
                       {"plugin_files_held", pluginFilesHeld},
                       {"dry_run", dryRun},
                       {"problems", list(problems)}};
    QJsonObject named{{"details_undecided", list(titles.detailsUndecided)}};
    if (dryRun)
        named = QJsonObject{{"received", list(titles.received)},
                            {"sent", list(titles.sent)},
                            {"places_taken", list(titles.placesTaken)},
                            {"places_sent", list(titles.placesSent)},
                            {"details_taken", list(titles.detailsTaken)},
                            {"details_sent", list(titles.detailsSent)},
                            {"reading_taken", list(titles.readingTaken)},
                            {"reading_sent", list(titles.readingSent)},
                            {"removed_here", list(titles.removedHere)},
                            {"removed_there", list(titles.removedThere)},
                            {"details_undecided", list(titles.detailsUndecided)}};
    result.insert("titles", named);
    return result;
}

namespace SyncManifest {

void observeManifest(const Library &library, const QJsonObject &manifest) {
    ChangeLog::reconcile(library);
    Spaces::observeHlc(library, manifest.value("hlc").toInteger());
    for (const auto &book : manifest.value("books").toArray()) {
        const QJsonObject obj = book.toObject();
        Spaces::observeHlc(library, theirDetailsHlc(obj));
        Spaces::observeHlc(library, theirProgressHlc(obj));
        Spaces::observeHlc(library, obj.value("reading_hlc").toInteger(obj.value("reading_at").toInteger()));
    }
    for (const auto &rem : manifest.value("removals").toArray()) {
        const QJsonObject obj = rem.toObject();
        Spaces::observeHlc(library, obj.value("hlc").toInteger(obj.value("at").toInteger()));
    }
}

qint64 theirDetailsHlc(const QJsonObject &theirs) {
    const qint64 plain = theirs.value("details_at").toInteger();
    const qint64 hlc = theirs.value("details_hlc").toInteger(plain);
    return plain == 0 && !theirs.contains("details_hlc") ? 0 : Hlc::normalize(hlc);
}

qint64 myDetailsHlc(const Library &library, const Book &book) {
    if (book.detailsAt == 0)
        return 0;
    const QHash<QString, qint64> fieldHlcs = SpaceReplay::bookFieldHlcs(library, book.id);
    return maxOf(fieldHlcs,
                 {"title"_L1, "authors"_L1, "series"_L1, "series_index"_L1, "publisher"_L1, "language"_L1, "isbn"_L1, "published"_L1,
                  "description"_L1, "tags"_L1, "identifiers"_L1, "has_cover"_L1, "cover_hash"_L1},
                 Hlc::normalize(book.detailsAt));
}

qint64 theirProgressHlc(const QJsonObject &theirs) {
    const QJsonObject prog = theirs.value("progress").toObject();
    const qint64 plain = prog.value("updated").toInteger();
    return plain == 0 ? 0 : Hlc::normalize(prog.value("hlc").toInteger(plain));
}

qint64 myProgressHlc(const Library &library, qint64 bookId, const Progress &place) {
    return stampedOrNormalizedHlc(place.updated, SpaceReplay::bookFieldHlcs(library, bookId).value(u"progress"_s));
}

qint64 lastChangeHlc(const Library &library, const Book &book) {
    const QHash<QString, qint64> fieldHlcs = SpaceReplay::bookFieldHlcs(library, book.id);
    const qint64 detailsHlc =
        book.detailsAt == 0
            ? 0
            : std::max(Hlc::normalize(book.detailsAt),
                       maxOf(fieldHlcs, {"title"_L1, "description"_L1, "tags"_L1, "publisher"_L1, "series"_L1, "authors"_L1, "isbn"_L1,
                                         "language"_L1, "published"_L1, "identifiers"_L1, "has_cover"_L1, "cover_hash"_L1}));
    const qint64 readingHlc = book.readingAt == 0 ? 0
                                                  : std::max(Hlc::normalize(book.readingAt),
                                                             maxOf(fieldHlcs, {"shelf"_L1, "rating"_L1, "finished"_L1, "paper"_L1}));
    const qint64 progHlc = stampedOrNormalizedHlc(book.lastRead, fieldHlcs.value(u"progress"_s));
    return std::max({Hlc::normalize(book.added), progHlc, detailsHlc, readingHlc});
}

qint64 lastChangeHlc(const QJsonObject &theirs) {
    if (theirs.contains("changed_hlc"))
        return Hlc::normalize(theirs.value("changed_hlc").toInteger());
    return std::max({Hlc::normalize(theirs.value("added_at").toInteger()), theirProgressHlc(theirs), theirDetailsHlc(theirs),
                     Hlc::normalize(theirs.value("reading_hlc").toInteger(theirs.value("reading_at").toInteger()))});
}

std::optional<qint64> localRemovalHlc(const Library &library, const QString &key) {
    const std::optional<qint64> at = library.removedAt(key);
    if (!at)
        return std::nullopt;
    const qint64 logged = SpaceReplay::fieldHlc(library, u"book"_s, Spaces::removalGlobalId(key), u"removed"_s);
    return Hlc::seconds(logged) == *at ? logged : Hlc::normalize(*at);
}

QHash<QString, qint64> remoteRemovalHlcs(const QJsonObject &manifest) {
    QHash<QString, qint64> removals;
    for (const auto &removal : manifest.value("removals").toArray()) {
        const QJsonObject obj = removal.toObject();
        const QString key = obj.value("key").toString();
        removals.insert(key, Hlc::normalize(obj.value("hlc").toInteger(obj.value("at").toInteger())));
    }
    return removals;
}

qint64 wireTimestamp(qint64 hlc, bool peerHasLog) noexcept { return peerHasLog ? Hlc::normalize(hlc) : Hlc::seconds(hlc); }

MergedBookDetails mergeBookDetails(const Library &library, const Book &here, const QJsonObject &theirs, bool current,
                                   SyncOptions::Details choice) {
    const QJsonObject mine{{"details_at", here.detailsAt},
                           {"field_hlc", bookFieldHlcJson(here, SpaceReplay::bookFieldHlcs(library, here.id))}};
    return decideDetails(here, mine, theirs, current, choice, current && coversDiffer(library, here.id, theirs));
}

MergedBookDetails decideDetails(const Book &here, const QJsonObject &mine, const QJsonObject &theirs, bool current,
                                SyncOptions::Details choice, bool covers) {
    MergedBookDetails out;
    // The details as this side's log says them, where it says them, as
    // theirs are as their log says them: both sides then compare alike, and
    // a book is undecided from both or from neither (#106). The book's own
    // details where the log has none of them (an older peer's manifest).
    out.meta = BookMeta::fromJson(mine.value("title").toString().isEmpty() ? here.meta.toJson() : mine);
    const BookMeta theirMeta = BookMeta::fromJson(theirs);
    const QJsonObject myFieldObj = mine.value("field_hlc").toObject();
    const QJsonObject theirFieldObj = theirs.value("field_hlc").toObject();
    const qint64 myBase = theirDetailsHlc(mine);
    const qint64 theirBase = theirDetailsHlc(theirs);
    const auto myAt = [&](QLatin1StringView key) { return fieldHlc(myBase, myFieldObj, key); };
    const auto theirAt = [&](QLatin1StringView key) { return fieldHlc(theirBase, theirFieldObj, key); };

    static constexpr std::array textFields{
        std::pair{"title"_L1, &BookMeta::title},         std::pair{"publisher"_L1, &BookMeta::publisher},
        std::pair{"language"_L1, &BookMeta::language},   std::pair{"isbn"_L1, &BookMeta::isbn},
        std::pair{"published"_L1, &BookMeta::published}, std::pair{"description"_L1, &BookMeta::description},
    };
    for (const auto &[key, member] : textFields)
        if (out.meta.*member != theirMeta.*member)
            decideField(out, myAt(key), theirAt(key), current, choice, [&] { out.meta.*member = theirMeta.*member; });
    static constexpr std::array listFields{
        std::pair{"authors"_L1, &BookMeta::authors},
        std::pair{"tags"_L1, &BookMeta::tags},
    };
    for (const auto &[key, member] : listFields)
        if (out.meta.*member != theirMeta.*member)
            decideField(out, myAt(key), theirAt(key), current, choice, [&] { out.meta.*member = theirMeta.*member; });
    if (out.meta.series != theirMeta.series || out.meta.seriesIndex != theirMeta.seriesIndex)
        decideField(out, myAt("series"_L1), theirAt("series"_L1), current, choice, [&] {
            out.meta.series = theirMeta.series;
            out.meta.seriesIndex = theirMeta.seriesIndex;
        });

    if (covers)
        mergeCoverField(out, myAt("cover"_L1), theirAt("cover"_L1), choice);
    return out;
}

Progress progressFrom(const QJsonObject &object) {
    Progress progress;
    progress.place = BookPlace::fromJson(object);
    progress.percent = object.value("percent").toDouble();
    progress.updated = object.value("updated").toInteger();
    return progress;
}

QStringList progressArguments(qint64 id, const Progress &progress) {
    return {"progress",   QString::number(id),
            "--section",  QString::number(progress.place.section),
            "--fraction", QString::number(progress.place.fraction, 'g', 12),
            "--percent",  QString::number(progress.percent, 'g', 12),
            "--updated",  QString::number(progress.updated)};
}

std::optional<qint64> removedWhen(const QStringList &hashes, const QStringList &keys,
                                  const std::function<std::optional<qint64>(const QString &)> &at) {
    std::optional<qint64> earliest;
    for (const QString &key : hashes.isEmpty() ? keys : hashes) {
        const std::optional<qint64> when = at(key);
        if (!when && !hashes.isEmpty())
            return std::nullopt;
        if (when)
            earliest = std::min(earliest.value_or(*when), *when);
    }
    return earliest;
}

} // namespace SyncManifest
