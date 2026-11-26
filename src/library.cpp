#include "library.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <ranges>

#include "addinghold.h"
#include "booksides.h"
#include "changelog.h"
#include "formats.h"
#include "graph.h"
#include "hlc.h"
#include "librarybatch.h"
#include "librarymigrations.h"
#include "log.h"
#include "readingformat.h"
#include "safename.h"
#include "spaces.h"
#include "statements.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString folderFor(const BookMeta &meta, qint64 id) {
    return SafeName::component(meta.authors.value(0), "Unknown") + "/" + SafeName::component(meta.title, "Untitled")
           + QString(" (%1)").arg(id);
}

[[nodiscard]] Book bookFrom(const QSqlQuery &query) {
    Book book;
    book.id = query.value("id").toLongLong();
    book.meta.title = query.value("title").toString();
    book.meta.authors = BookMeta::splitAuthors(query.value("authors").toString());
    book.meta.series = query.value("series").toString();
    book.meta.seriesIndex = query.value("series_index").toDouble();
    book.meta.publisher = query.value("publisher").toString();
    book.meta.language = query.value("language").toString();
    book.meta.isbn = query.value("isbn").toString();
    book.meta.published = query.value("published").toString();
    book.meta.description = query.value("description").toString();
    book.meta.tags = BookMeta::splitTags(query.value("tags").toString());
    book.path = query.value("path").toString();
    book.added = query.value("added").toLongLong();
    book.hasCover = query.value("has_cover").toBool();
    book.percent = query.value("percent").toDouble();
    book.lastRead = query.value("updated").toLongLong();
    book.paper = query.value("paper").toBool();
    book.shelf = query.value("shelf").toString();
    book.rating = query.value("rating").toInt();
    book.finished = query.value("finished").toString();
    book.identifiers = QJsonDocument::fromJson(query.value("identifiers").toString().toUtf8()).object();
    book.detailsAt = query.value("details_at").toLongLong();
    book.readingAt = query.value("reading_at").toLongLong();
    return book;
}

// The details as the books table keeps them, to tell whether a change
// changes anything.
[[nodiscard]] QVariantList storedDetails(const BookMeta &meta) {
    return {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
            meta.isbn, meta.published, meta.description, meta.tags.join(", ")};
}

[[nodiscard]] QString compact(const QJsonObject &object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

constexpr auto selectBooks = "SELECT books.*, progress.percent, progress.updated FROM books "
                             "LEFT JOIN progress ON progress.book_id = books.id"_L1;

// Says why something didn't work, to a caller that asked, and is the "no" to return.
bool failed(QString *error, const QString &why) {
    if (error)
        *error = why;
    return false;
}

// A merge takes this file of the other book into the one kept when it is a format
// that one lacks. An empty file is nothing to keep; it goes to the trash with its folder.
[[nodiscard]] std::expected<void, Failure> taken(Library &library, const Book &kept, const FormatFile &file) {
    if (kept.formats.contains(file.format) || file.size == 0)
        return {};
    return library.addFormat(kept.id, file.path);
}

} // namespace

QString Book::status() const {
    if (!shelf.isEmpty())
        return shelf;
    return percent >= 98 ? "read" : percent > 0 ? "reading" : QString();
}

QString Book::held() const {
    return !formats.isEmpty() ? (paper ? "both" : "digital") : paper ? "paper" : "none";
}

bool Collection::Part::holds(const Book &book) const {
    if (excluded.contains(book.id))
        return false;
    return included.contains(book.id) || (!rule.trimmed().isEmpty() && book.matches(rule));
}

bool Collection::contains(const Book &book) const {
    // Its own choice by hand wins; then its own rule, and what the
    // collections inside it hold.
    if (excluded.contains(book.id))
        return false;
    return Part{rule, included, excluded}.holds(book)
           || std::ranges::any_of(within, [&book](const Part &part) { return part.holds(book); });
}

QList<Book> Collection::of(const QList<Book> &books) const {
    QList<Book> in;
    for (const Book &book : books)
        if (contains(book))
            in << book;
    return in;
}

QJsonObject Collection::toJson() const {
    const auto ids = [](const QSet<qint64> &set) {
        QList<qint64> sorted(set.begin(), set.end());
        std::ranges::sort(sorted);
        QJsonArray array;
        for (const qint64 id : sorted)
            array << id;
        return array;
    };
    return {{"id", id}, {"name", name}, {"path", path}, {"parent", parent}, {"rule", rule}, {"included", ids(included)},
            {"excluded", ids(excluded)}, {"shelf", shelf}, {"format", format}};
}

QJsonObject Book::toJson() const {
    QJsonObject object = meta.toJson();
    object.insert("id", id);
    object.insert("formats", QJsonArray::fromStringList(formats));
    object.insert("added", QDateTime::fromSecsSinceEpoch(added).toString(Qt::ISODate));
    object.insert("percent_read", percent);
    object.insert("held", held());
    object.insert("status", status());
    if (rating > 0)
        object.insert("rating", rating);
    if (!finished.isEmpty())
        object.insert("finished", finished);
    object.insert("identifiers", identifiers);
    object.insert("generated", identifiers.value("generated").toString()); // the plugin that made it, as plugin events say (#128)
    object.insert("details_at", detailsAt);
    object.insert("reading_at", readingAt);
    return object;
}

Library::Library(const QString &root)
    : m_root(root.isEmpty() ? QString() : QDir(root).absolutePath()), m_connection("omabook-" + QUuid::createUuid().toString(QUuid::Id128)),
      m_statements(std::make_unique<Statements>(m_connection)) {}

Library::~Library() {
    if (QSqlDatabase::contains(m_connection)) {
        LibraryMigrations::closing(*this);
        QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

QString Library::databasePath() const { return databaseIn(m_root); }

QString Library::databaseIn(const QString &root) { return root + "/omabook.db"; }

QString Library::backupFolder() const { return m_root + "/.backups"; }

int Library::formatVersion() const {
    QSqlQuery version = query("PRAGMA user_version");
    return version.next() ? version.value(0).toInt() : 0;
}

bool Library::exec(const QString &sql, const QVariantList &values) const {
    // Outside a batch, a write waits its turn as a batch does (librarybatch.h).
    std::optional<WriteTurn> turn;
    if (!QFileInfo::exists(databasePath()) || (m_batches == 0 && !turn.emplace(databasePath(), m_connection).had()))
        return false;
    if (!m_statements->run(sql, values))
        return false;
    if (m_batches == 0)
        ChangeLog::afterWrite(*this);
    return true;
}

QSqlQuery Library::query(const QString &sql, const QVariantList &values) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(sql);
    // As they are: a null string asks for NULL, which finds nothing.
    for (const QVariant &value : values)
        query.addBindValue(value);
    query.exec();
    return query;
}

std::expected<qint64, QString> Library::insert(const QString &sql, const QVariantList &values) const {
    std::optional<WriteTurn> turn; // as exec's
    if (!QFileInfo::exists(databasePath()) || (m_batches == 0 && !turn.emplace(databasePath(), m_connection).had()))
        return std::unexpected(whyNotWritten(databasePath()).text());
    const auto id = m_statements->run(sql, values);
    if (id && m_batches == 0)
        ChangeLog::afterWrite(*this);
    return id;
}

QString Library::bookFolder(qint64 id) const {
    QSqlQuery folder = query("SELECT path FROM books WHERE id = ?", {id});
    return folder.next() ? m_root + "/" + folder.value(0).toString() : QString();
}

Library::Added Library::add(const QString &file, const std::optional<BookMeta> &known) {
    const QFileInfo info(file);
    if (!info.isFile() || !info.isReadable())
        return {.error = Failure(Failure::Code::NoReadableFile, {file}).text()};
    if (!Formats::isBook(file))
        return {.error = Failure(Failure::Code::NotABookFormat, {info.fileName()}).text()};
    const QString hash = hashFile(file);
    // Held until the book's file is recorded: another adding it waits, then finds it (#228).
    const auto held = AddingHold::files(m_root, {hash});
    if (!held)
        return {.error = held.error().text()};
    if (const qint64 existing = bookWithHash(hash))
        return {.id = existing, .duplicate = true, .error = {}};
    // Added on purpose, so no longer something that was thrown away.
    exec("DELETE FROM removed WHERE hash = ?", {hash});

    BookMeta meta = known ? *known : Formats::readMeta(file);
    if (meta.title.isEmpty())
        meta.title = Formats::metaFromFileName(file).title;
    std::optional<ChangeLog::Scope> guard(std::in_place);
    // The new row is seen by others only once its book is held, so none gives it another file meanwhile.
    std::optional<Batch> making(std::in_place, *this);
    if (!making->begun())
        return {.error = Failure(Failure::Code::LibraryBusy).text()};
    const auto id = insert("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                           "description, tags, added, details_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
                            meta.isbn, meta.published, meta.description, meta.tags.join(", "),
                            Spaces::wallSeconds(*this), Spaces::wallSeconds(*this)});
    if (!id)
        return {.error = Failure(Failure::Code::LibraryUnwritable, {id.error()}).text()};
    const QString folder = folderFor(meta, *id);
    exec("UPDATE books SET path = ? WHERE id = ?", {folder, *id});
    // Nobody else can be waiting for a row not yet seen; without the hold the add still goes on.
    const auto filling = AddingHold::book(m_root, *id, std::chrono::seconds(5));
    making.reset();
    guard.reset();

    if (const auto put = addFormat(*id, file); !put) {
        const ChangeLog::Scope cleanupGuard;
        exec("DELETE FROM books WHERE id = ?", {*id});
        QDir(m_root + "/" + folder).removeRecursively();
        return {.error = put.error().text()};
    }
    if (!meta.cover.isNull())
        setCover(*id, meta.cover);
    return {.id = *id, .duplicate = false, .error = {}};
}

Library::Added Library::addEntry(const BookMeta &meta, bool paper) {
    if (meta.title.trimmed().isEmpty())
        return {.error = Failure(Failure::Code::BookNeedsTitle).text()};
    const Batch batch(*this);
    const auto id = insert("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                           "description, tags, added, paper, details_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
                            meta.isbn, meta.published, meta.description, meta.tags.join(", "),
                            Spaces::wallSeconds(*this), paper, Spaces::wallSeconds(*this)});
    if (!id)
        return {.error = Failure(Failure::Code::LibraryUnwritable, {id.error()}).text()};
    // The folder is made when there is something to put in it: a cover, or
    // a file the book is given later.
    exec("UPDATE books SET path = ? WHERE id = ?", {folderFor(meta, *id), *id});
    if (!meta.cover.isNull())
        setCover(*id, meta.cover);
    Spaces::ensureBookId(*this, *id);
    return {.id = *id, .duplicate = false, .error = {}};
}

bool Library::setReading(qint64 id, const QString &shelf, int rating, const QString &finished, std::optional<qint64> at) {
    static constexpr std::array shelves{""_L1, "to-read"_L1, "reading"_L1, "read"_L1};
    if (!std::ranges::contains(shelves, shelf))
        return false;
    const int stars = std::clamp(rating, 0, 5);
    // Stamped only when something changes: a sync that says the same again
    // doesn't make the book look read since.
    return exec("UPDATE books SET shelf = ?, rating = ?, finished = ?, reading_at = CASE WHEN shelf IS ? AND rating = ? "
                "AND finished IS ? THEN reading_at ELSE ? END WHERE id = ?",
                {shelf, stars, finished, shelf, stars, finished, at ? Hlc::seconds(*at) : Spaces::wallSeconds(*this), id});
}

bool Library::setPaper(qint64 id, bool paper, std::optional<qint64> at) {
    return exec("UPDATE books SET paper = ?, reading_at = CASE WHEN paper = ? THEN reading_at ELSE ? END WHERE id = ?",
                {paper, paper, at ? Hlc::seconds(*at) : Spaces::wallSeconds(*this), id});
}

bool Library::setDetailsAt(qint64 id, qint64 at) { return ChangeLog::setDetailsAt(*this, id, at); }

QJsonObject Library::bookFormat(qint64 id) const {
    QSqlQuery found = query("SELECT options FROM book_format WHERE book_id = ?", {id});
    return found.next() ? ReadingFormat::fitting(QJsonDocument::fromJson(found.value(0).toString().toUtf8()).object()) : QJsonObject();
}

bool Library::setBookFormat(qint64 id, const QJsonObject &options) {
    const QJsonObject kept = ReadingFormat::fitting(options);
    if (kept.isEmpty())
        return exec("DELETE FROM book_format WHERE book_id = ?", {id});
    if (!book(id))
        return false;
    // Unchanged, it isn't written: the reader hears of every write.
    if (kept == bookFormat(id))
        return true;
    return exec("INSERT OR REPLACE INTO book_format (book_id, options) VALUES (?, ?)",
                {id, QString::fromUtf8(QJsonDocument(kept).toJson(QJsonDocument::Compact))});
}

bool Library::setIdentifier(qint64 id, const QString &scheme, const QString &value, std::optional<qint64> at) {
    const auto found = book(id);
    if (!found)
        return false;
    QJsonObject identifiers = found->identifiers;
    if (value.isEmpty())
        identifiers.remove(scheme);
    else
        identifiers.insert(scheme, value);
    if (identifiers == found->identifiers)
        return true;
    const Batch batch(*this);
    if (!exec("UPDATE books SET identifiers = ?, details_at = ? WHERE id = ?",
              {compact(identifiers), at ? Hlc::seconds(*at) : Spaces::wallSeconds(*this), id}))
        return false;
    if (at)
        ChangeLog::setDetailsAt(*this, id, *at);
    return true;
}

qint64 Library::bookWithIdentifier(const QString &scheme, const QString &value) const {
    if (value.isEmpty())
        return 0;
    const QList<Book> all = books();
    const auto called = std::ranges::find_if(all, [&scheme, &value](const Book &book) { return book.identifiers.value(scheme).toString() == value; });
    return called != all.end() ? called->id : 0;
}

std::expected<void, QString> Library::remove(qint64 id, std::optional<qint64> at) {
    const QString folder = bookFolder(id);
    if (folder.isEmpty())
        return std::unexpected(QString("There is no book %1.").arg(id));
    const Batch batch(*this);
    const QList<FormatFile> formats = formatFiles(id);
    // A file another book holds too (the one a merge kept took it over)
    // is still in the library, so a sync mustn't take it for removed.
    // So is the file one came in as, before details were written into it (#74).
    for (const FormatFile &format : formats)
        for (const QString &hash : {format.hash, format.original})
            if (!hash.isEmpty() && !query("SELECT 1 FROM formats WHERE (hash = ? OR original = ?) AND book_id <> ?", {hash, hash, id}).next())
                exec("INSERT OR REPLACE INTO removed (hash, removed_at) VALUES (?, ?)", {hash, at.value_or(Spaces::wallSeconds(*this))});
    // A book with no file is remembered by what names it instead.
    if (const std::optional<Book> gone = formats.isEmpty() ? book(id) : std::nullopt)
        for (const QString &key : entryKeys(*gone))
            exec("INSERT OR REPLACE INTO removed (hash, removed_at) VALUES (?, ?)", {key, at.value_or(Spaces::wallSeconds(*this))});
    // To the trash, where a mistake can be undone; a filesystem with no
    // trash gets the folder removed, since the user asked for it to go.
    if (QFileInfo::exists(folder) && !QFile::moveToTrash(folder) && !QDir(folder).removeRecursively())
        return std::unexpected("The book's folder can't be removed: " + folder);
    QDir().rmdir(QFileInfo(folder).absolutePath()); // the author's folder, when it is now empty
    return insert("DELETE FROM books WHERE id = ?", {id}).transform([](qint64) {}); // what the database said, when it failed
}

QList<Book> Library::books(const QString &query) const {
    QHash<qint64, QStringList> formats;
    for (QSqlQuery format = this->query("SELECT book_id, format FROM formats"); format.next();)
        formats[format.value(0).toLongLong()] << format.value(1).toString();

    QList<Book> books;
    for (QSqlQuery found = this->query(QString(selectBooks) + " ORDER BY books.id"); found.next();) {
        Book book = bookFrom(found);
        book.formats = Formats::byPreference(formats.value(book.id));
        if (query.isEmpty() || book.matches(query))
            books << book;
    }
    return books;
}

std::optional<Book> Library::book(qint64 id) const {
    QSqlQuery found = query(QString(selectBooks) + " WHERE books.id = ?", {id});
    if (!found.next())
        return std::nullopt;
    Book book = bookFrom(found);
    for (QSqlQuery format = query("SELECT format FROM formats WHERE book_id = ?", {id}); format.next();)
        book.formats << format.value(0).toString();
    book.formats = Formats::byPreference(book.formats);
    return book;
}

bool Library::setMeta(qint64 id, const BookMeta &meta, QString *error, std::optional<qint64> at) {
    const auto before = book(id);
    if (!before)
        return failed(error, QString("There is no book %1.").arg(id));
    {
        const Batch batch(*this);
        BookMeta kept = meta;
        if (kept.title.isEmpty())
            kept.title = before->meta.title;
        QVariantList values = storedDetails(kept);
        // Stamped when something changed, or with the time it is given.
        const bool changed = values != storedDetails(before->meta);
        const qint64 stamp = changed ? Spaces::wallSeconds(*this) : before->detailsAt;
        values << (at ? Hlc::seconds(*at) : stamp) << id;
        if (!exec("UPDATE books SET title = ?, authors = ?, series = ?, series_index = ?, publisher = ?, language = ?, "
                  "isbn = ?, published = ?, description = ?, tags = ?, details_at = ? WHERE id = ?",
                  values))
            return failed(error, "The library can't be written.");
        // The folder is named after the author and title, so it follows them.
        // Where it can't be moved it keeps its old name, which still works.
        const QString folder = folderFor(meta.title.isEmpty() ? before->meta : meta, id);
        if (folder != before->path && QFileInfo::exists(m_root + "/" + before->path)
            && QDir().mkpath(QFileInfo(m_root + "/" + folder).absolutePath())
            && QDir().rename(m_root + "/" + before->path, m_root + "/" + folder)) {
            exec("UPDATE books SET path = ? WHERE id = ?", {folder, id});
            QDir().rmdir(QFileInfo(m_root + "/" + before->path).absolutePath());
        }
        if (!meta.cover.isNull())
            setCover(id, meta.cover, at);
        if (at)
            ChangeLog::setDetailsAt(*this, id, *at);
    }
    return true;
}

std::optional<qint64> Library::removedAt(const QString &hash) const {
    QSqlQuery found = query("SELECT removed_at FROM removed WHERE hash = ?", {hash});
    return found.next() ? std::optional<qint64>(found.value(0).toLongLong()) : std::nullopt;
}

QList<Library::Removal> Library::removals() const {
    QList<Removal> removals;
    for (QSqlQuery found = query("SELECT hash, removed_at FROM removed ORDER BY hash"); found.next();)
        removals.append({.key = found.value(0).toString(), .at = found.value(1).toLongLong()});
    return removals;
}

QStringList Library::removedHashes() const {
    QStringList hashes;
    for (QSqlQuery found = query("SELECT hash FROM removed"); found.next();)
        hashes << found.value(0).toString();
    return hashes;
}

Progress Library::progress(qint64 id) const {
    QSqlQuery found = query("SELECT section, fraction, percent, updated FROM progress WHERE book_id = ?", {id});
    if (!found.next())
        return {};
    return {.place = {.section = found.value(0).toInt(), .fraction = found.value(1).toDouble()},
            .percent = found.value(2).toDouble(),
            .updated = found.value(3).toLongLong()};
}

void Library::setProgress(qint64 id, const Progress &progress) { ChangeLog::setProgress(*this, id, progress); }

bool Library::wasRemoved(const QString &hash) const { return query("SELECT 1 FROM removed WHERE hash = ?", {hash}).next(); }

Recap Library::notes(qint64 id, const QString &kind) const {
    QSqlQuery found = query("SELECT section, fraction, text, updated FROM notes WHERE book_id = ? AND kind = ?", {id, kind});
    if (!found.next())
        return {};
    return {.place = {.section = found.value(0).toInt(), .fraction = found.value(1).toDouble()},
            .text = found.value(2).toString(),
            .updated = found.value(3).toLongLong()};
}

QList<Book> Library::earlierInSeries(qint64 id) const {
    const std::optional<Book> current = book(id);
    const QString series = current ? current->meta.series.trimmed() : QString();
    if (series.isEmpty())
        return {};
    // Only those on its side of the library (BookSides): a series is not
    // carried from one top collection into another.
    const BookSides sides = BookSides::of(*this);
    const auto comesBefore = [id, &current, &series, &sides](const Book &other) {
        return other.id != id && other.meta.seriesIndex < current->meta.seriesIndex
               && other.meta.series.trimmed().compare(series, Qt::CaseInsensitive) == 0 && sides.together(id, other.id);
    };
    const QList<Book> all = books();
    QList<Book> earlier = all | std::views::filter(comesBefore) | std::ranges::to<QList<Book>>();
    std::ranges::sort(earlier, {}, [](const Book &book) { return book.meta.seriesIndex; });
    return earlier;
}

QList<SeriesNotes> Library::seriesNotes(qint64 id, const QString &kind) const {
    QList<SeriesNotes> found;
    for (const Book &other : earlierInSeries(id)) {
        QString text = notes(other.id, kind).text;
        for (const QString &fallback : {QString("recap"), QString("cast")})
            if (text.isEmpty())
                text = notes(other.id, fallback).text;
        const bool blurb = text.isEmpty();
        if (blurb)
            text = notes(other.id, "blurb").text;
        if (text.isEmpty())
            text = other.meta.description;
        if (!text.trimmed().isEmpty())
            found.append({.title = other.meta.title, .text = text, .blurb = blurb});
    }
    return found;
}

void Library::setNotes(qint64 id, const QString &kind, const Recap &notes) {
    exec("INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) VALUES (?, ?, ?, ?, ?, ?)",
         {id, kind, notes.place.section, notes.place.fraction, notes.text, QDateTime::currentSecsSinceEpoch()});
}

bool Library::merge(qint64 keep, qint64 other, QString *error) {
    const auto kept = book(keep), folded = book(other);
    if (!kept || !folded || keep == other)
        return failed(error, "There are no two such books to merge.");
    for (const FormatFile &file : formatFiles(other))
        if (const auto put = taken(*this, *kept, file); !put)
            return failed(error, put.error().text());
    if (!kept->hasCover && folded->hasCover)
        setCover(keep, QImage(coverPath(other)));
    {
        const Batch batch(*this);
        // What the book is called elsewhere: both books' names, the kept
        // book's where both have one. A site whose number only the other
        // book had would otherwise add the book again at the next sync.
        // `generated` says a plugin made the book, which it didn't make.
        QJsonObject identifiers = kept->identifiers;
        for (auto it = folded->identifiers.begin(); it != folded->identifiers.end(); ++it)
            if (it.key() != "generated"_L1 && identifiers.value(it.key()).toVariant().toString().isEmpty())
                identifiers.insert(it.key(), it.value());
        // What is said of the reading: the kept book's where it says
        // anything, and of two shelves the one further along.
        static constexpr std::array shelves{""_L1, "to-read"_L1, "reading"_L1, "read"_L1};
        const auto rank = [](const QString &shelf) { return std::ranges::find(shelves, shelf) - shelves.begin(); };
        const QString shelf = rank(folded->shelf) > rank(kept->shelf) && rank(folded->shelf) < qsizetype(shelves.size())
                                  ? folded->shelf : kept->shelf;
        const bool paper = kept->paper || folded->paper;
        const int rating = kept->rating > 0 ? kept->rating : folded->rating;
        const QString finished = kept->finished.isEmpty() ? folded->finished : kept->finished;
        // What changed is stamped, as any change of the details or the reading is.
        const qint64 now = Spaces::wallSeconds(*this);
        const bool readingChanged = paper != kept->paper || shelf != kept->shelf || rating != kept->rating || finished != kept->finished;
        if (!exec("UPDATE books SET identifiers = ?, paper = ?, shelf = ?, rating = ?, finished = ?, details_at = ?, reading_at = ? "
                  "WHERE id = ?",
                  {compact(identifiers), paper, shelf, rating, finished,
                   identifiers != kept->identifiers ? now : kept->detailsAt, readingChanged ? now : kept->readingAt, keep}))
            return failed(error, "The library can't be written.");

        // The reading place that is further along is the one that was read,
        // and the notes and places in the graph that go with it win where
        // both books have them.
        const bool furtherOther = progress(other).percent > progress(keep).percent;
        if (furtherOther)
            setProgress(keep, progress(other));
        const QVariantList both{keep, other};
        // Bookmarks and highlights, without the ones the kept book has too.
        exec("DELETE FROM annotations WHERE book_id = ? AND EXISTS (SELECT 1 FROM annotations AS mine WHERE mine.book_id = ? "
             "AND mine.kind = annotations.kind AND mine.section = annotations.section AND mine.fraction = annotations.fraction "
             "AND mine.text = annotations.text AND mine.note = annotations.note)", {other, keep});
        exec("UPDATE annotations SET book_id = ? WHERE book_id = ?", both);
        // The AI's notes.
        exec(QString("INSERT OR %1 INTO notes (book_id, kind, section, fraction, text, updated) "
                     "SELECT ?, kind, section, fraction, text, updated FROM notes WHERE book_id = ?")
                 .arg(furtherOther ? "REPLACE" : "IGNORE"), both);
        // How it is set on the page, where only the other book had a way of its own.
        exec("INSERT OR IGNORE INTO book_format (book_id, options) SELECT ?, options FROM book_format WHERE book_id = ?", both);
        // The collections it was put in or taken out of by hand, where the
        // kept book wasn't.
        exec("INSERT OR IGNORE INTO collection_books (collection_id, book_id, included) "
             "SELECT collection_id, ?, included FROM collection_books WHERE book_id = ?", both);
        // The knowledge graph: where things were met and facts learned, the
        // days it was read on, and the node that stands for the book.
        if (furtherOther)
            exec("DELETE FROM graph_seen WHERE book_id = ? AND node_id IN (SELECT node_id FROM graph_seen WHERE book_id = ?)", both);
        exec("UPDATE OR IGNORE graph_seen SET book_id = ? WHERE book_id = ?", both);
        exec("UPDATE graph_edges SET book_id = ? WHERE book_id = ?", both);
        exec("UPDATE graph_facts SET book_id = ? WHERE book_id = ?", both);
        exec("UPDATE reading_log SET from_percent = MIN(from_percent, (SELECT o.from_percent FROM reading_log AS o WHERE o.book_id = ? "
             "AND o.day = reading_log.day)), to_percent = MAX(to_percent, (SELECT o.to_percent FROM reading_log AS o WHERE "
             "o.book_id = ? AND o.day = reading_log.day)) WHERE book_id = ? AND day IN (SELECT day FROM reading_log WHERE book_id = ?)",
             {other, other, keep, other});
        exec("UPDATE OR IGNORE reading_log SET book_id = ? WHERE book_id = ?", both);
        // Both books' nodes have the book's title for a name, so they are
        // joined by their ids here rather than looked up by name.
        QList<qint64> foldedNodes;
        for (QSqlQuery nodes = query("SELECT id FROM graph_nodes WHERE book_id = ?", {other}); nodes.next();)
            foldedNodes << nodes.value(0).toLongLong();
        QSqlQuery mine = query("SELECT id FROM graph_nodes WHERE kind = 'book' AND book_id = ?", {keep});
        const qint64 keptNode = mine.next() ? mine.value(0).toLongLong() : 0;
        mine.finish();
        for (const qint64 node : std::as_const(foldedNodes)) {
            if (keptNode == 0) {
                exec("UPDATE graph_nodes SET book_id = ? WHERE id = ?", {keep, node});
                continue;
            }
            const QVariantList nodes{keptNode, node};
            exec("DELETE FROM graph_edges WHERE (source = ? AND target = ?) OR (source = ? AND target = ?)",
                 {keptNode, node, node, keptNode});
            exec("UPDATE OR IGNORE graph_edges SET source = ? WHERE source = ?", nodes);
            exec("UPDATE OR IGNORE graph_edges SET target = ? WHERE target = ?", nodes);
            exec("UPDATE OR IGNORE graph_seen SET node_id = ? WHERE node_id = ?", nodes);
            exec("UPDATE graph_facts SET node_id = ? WHERE node_id = ?", nodes);
            exec("UPDATE graph_nodes SET summary = (SELECT summary FROM graph_nodes WHERE id = ?) WHERE id = ? AND summary = ''",
                 {node, keptNode});
            // What is left of it (edges both nodes had) goes with it.
            exec("DELETE FROM graph_nodes WHERE id = ?", {node});
        }
    }
    if (const auto removed = remove(other); !removed)
        return failed(error, removed.error());
    // The node says what the book is now: its status, stars and formats.
    Graph(*this).bookNode(keep);
    return true;
}

namespace {

// A collection's name: what it is called on the shelf, in Folio's sidebar and
// on the command line, so one line of ordinary length. "/" joins a path, so
// it is in no name.
[[nodiscard]] QString nameProblem(const QString &name) {
    if (name.isEmpty())
        return "A collection needs a name.";
    if (name.contains('/'))
        return "A collection's name can't have \"/\" in it; that is what puts one inside another: Studie/Python.";
    if (name.size() > 60)
        return "A collection's name can be 60 characters at most.";
    return {};
}

[[nodiscard]] QJsonObject objectFrom(const QVariant &text) {
    return QJsonDocument::fromJson(text.toString().toUtf8()).object();
}

[[nodiscard]] QStringList pathParts(const QString &path) {
    QStringList parts;
    for (const QString &part : path.split('/', Qt::SkipEmptyParts))
        if (!part.simplified().isEmpty())
            parts << part.simplified();
    return parts;
}

} // namespace

QList<Collection> Library::collections(bool withRemoved) const {
    QList<Collection> flat;
    QHash<qint64, qsizetype> at;
    for (QSqlQuery row = query("SELECT id, name, rule, shelf, format, updated, removed, parent FROM collections "
                               "ORDER BY position, id");
         row.next();) {
        Collection collection;
        collection.id = row.value(0).toLongLong();
        collection.name = row.value(1).toString();
        collection.rule = row.value(2).toString();
        collection.shelf = objectFrom(row.value(3));
        collection.format = objectFrom(row.value(4));
        collection.updated = row.value(5).toLongLong();
        collection.removed = row.value(6).toBool();
        collection.parent = row.value(7).toLongLong();
        at.insert(collection.id, flat.size());
        flat << collection;
    }
    for (QSqlQuery row = query("SELECT collection_id, book_id, included FROM collection_books"); row.next();) {
        const auto index = at.constFind(row.value(0).toLongLong());
        if (index == at.cend())
            continue;
        Collection &collection = flat[*index];
        (row.value(2).toBool() ? collection.included : collection.excluded).insert(row.value(1).toLongLong());
    }
    // One whose parent is gone (removed, or never synced here) is at the top.
    for (Collection &collection : flat)
        if (collection.parent && (!at.contains(collection.parent) || (flat.at(at.value(collection.parent)).removed && !collection.removed)))
            collection.parent = 0;
    // The tree, read from the top: each one, then those inside it.
    QList<Collection> tree;
    const std::function<void(qint64, const QString &, int, QSet<qint64> &)> visit =
        [&](qint64 parent, const QString &above, int depth, QSet<qint64> &seen) {
            for (const Collection &collection : std::as_const(flat)) {
                if (collection.parent != parent || seen.contains(collection.id))
                    continue;
                seen.insert(collection.id);
                Collection placed = collection;
                placed.path = above.isEmpty() ? collection.name : above + "/" + collection.name;
                placed.depth = depth;
                tree << placed;
                visit(collection.id, placed.path, depth + 1, seen);
            }
        };
    QSet<qint64> seen;
    visit(0, {}, 0, seen);
    // What each holds of those inside it, at every depth.
    for (qsizetype i = 0; i < tree.size(); ++i)
        for (qsizetype j = i + 1; j < tree.size() && tree.at(j).depth > tree.at(i).depth; ++j) {
            if (tree.at(j).removed)
                continue;
            tree[i].hasChildren = true;
            tree[i].within << Collection::Part{tree.at(j).rule, tree.at(j).included, tree.at(j).excluded};
        }
    if (!withRemoved)
        tree.removeIf([](const Collection &collection) { return collection.removed; });
    return tree;
}

std::optional<Collection> Library::collection(qint64 id) const {
    for (const Collection &collection : collections())
        if (collection.id == id)
            return collection;
    return std::nullopt;
}

std::optional<Collection> Library::collectionNamed(const QString &name) const {
    const QString wanted = pathParts(name).join('/');
    const QList<Collection> all = collections();
    for (const Collection &collection : all)
        if (collection.path.compare(wanted, Qt::CaseInsensitive) == 0)
            return collection;
    // A name alone, where only one collection has it.
    std::optional<Collection> only;
    for (const Collection &collection : all)
        if (collection.name.compare(wanted, Qt::CaseInsensitive) == 0) {
            if (only)
                return std::nullopt;
            only = collection;
        }
    if (only)
        return only;
    bool number = false;
    const qint64 id = wanted.toLongLong(&number);
    for (const Collection &collection : all)
        if (number && collection.id == id)
            return collection;
    return std::nullopt;
}

std::expected<qint64, QString> Library::addCollection(const QString &name, const QString &rule, qint64 parent) {
    QStringList parts = pathParts(name);
    if (parts.isEmpty())
        return std::unexpected(QString("A collection needs a name."));
    if (const QString why = Book::searchProblem(rule); !why.isEmpty())
        return std::unexpected(why);
    if (parent && !collection(parent))
        return std::unexpected(QString("There is no collection %1 to put it in.").arg(parent));
    for (const QString &part : parts)
        if (const QString why = nameProblem(part); !why.isEmpty())
            return std::unexpected(why);
    const Batch batch(*this);
    // The collections the path goes through, made where they aren't there.
    const QString named = parts.takeLast();
    for (const QString &part : std::as_const(parts)) {
        QSqlQuery there = query("SELECT id FROM collections WHERE parent = ? AND name = ? COLLATE NOCASE AND removed = 0", {parent, part});
        if (there.next()) {
            parent = there.value(0).toLongLong();
            continue;
        }
        there.finish();
        const auto made = addCollection(part, {}, parent);
        if (!made)
            return made;
        parent = *made;
    }
    const qint64 now = Spaces::wallSeconds(*this);
    QSqlQuery last = query("SELECT COALESCE(MAX(position), 0) + 1 FROM collections WHERE removed = 0");
    const int position = last.next() ? last.value(0).toInt() : 1;
    last.finish();
    QSqlQuery same = query("SELECT id, removed FROM collections WHERE parent = ? AND name = ? COLLATE NOCASE", {parent, named});
    if (same.next()) {
        const qint64 id = same.value(0).toLongLong();
        const bool removed = same.value(1).toBool();
        same.finish();
        if (!removed)
            return std::unexpected(QString("There is a collection called %1 there already.").arg(named));
        // One that was removed comes back as new.
        exec("DELETE FROM collection_books WHERE collection_id = ?", {id});
        if (!exec("UPDATE collections SET name = ?, rule = ?, position = ?, shelf = '{}', format = '{}', updated = ?, removed = 0 "
                  "WHERE id = ?",
                  {named, rule.trimmed(), position, now, id}))
            return std::unexpected(QString("The library can't be written."));
        Spaces::ensureCollectionId(*this, id);
        return id;
    }
    same.finish();
    const auto id = insert("INSERT INTO collections (name, rule, position, updated, parent) VALUES (?, ?, ?, ?, ?)",
                           {named, rule.trimmed(), position, now, parent});
    if (!id)
        return std::unexpected("The library can't be written: " + id.error());
    Spaces::ensureCollectionId(*this, *id);
    return *id;
}

bool Library::renameCollection(qint64 id, const QString &name, QString *error) {
    const QString named = name.simplified();
    if (const QString why = nameProblem(named); !why.isEmpty())
        return failed(error, why);
    const auto now = collection(id);
    if (!now)
        return failed(error, QString("There is no collection %1.").arg(id));
    if (now->name == named)
        return true;
    QSqlQuery other = query("SELECT id, removed FROM collections WHERE parent = ? AND name = ? COLLATE NOCASE AND id <> ?",
                            {now->parent, named, id});
    if (other.next() && !other.value(1).toBool())
        return failed(error, QString("There is a collection called %1 there already.").arg(named));
    other.finish();
    // A removed one of that name gives the name up.
    exec("DELETE FROM collections WHERE removed = 1 AND parent = ? AND name = ? COLLATE NOCASE AND id <> ?", {now->parent, named, id});
    return exec("UPDATE collections SET name = ?, updated = ? WHERE id = ?", {named, Spaces::wallSeconds(*this), id})
           || failed(error, "The library can't be written.");
}

bool Library::setCollectionRule(qint64 id, const QString &rule, QString *error) {
    if (const QString why = Book::searchProblem(rule); !why.isEmpty())
        return failed(error, why);
    if (!collection(id))
        return failed(error, QString("There is no collection %1.").arg(id));
    return exec("UPDATE collections SET rule = ?, updated = ? WHERE id = ?", {rule.trimmed(), Spaces::wallSeconds(*this), id})
           || failed(error, "The library can't be written.");
}

bool Library::setCollectionParent(qint64 id, qint64 parent, QString *error) {
    const QList<Collection> all = collections();
    const auto moved = std::ranges::find(all, id, &Collection::id);
    if (moved == all.end())
        return failed(error, QString("There is no collection %1.").arg(id));
    if (moved->parent == parent)
        return true;
    if (parent) {
        const auto into = std::ranges::find(all, parent, &Collection::id);
        if (into == all.end())
            return failed(error, QString("There is no collection %1 to put it in.").arg(parent));
        // Not inside itself, nor inside one of those inside it.
        if (into->path.compare(moved->path, Qt::CaseInsensitive) == 0
            || into->path.startsWith(moved->path + "/", Qt::CaseInsensitive))
            return failed(error, QString("%1 can't go inside itself.").arg(moved->name));
    }
    if (std::ranges::any_of(all, [&](const Collection &c) {
            return c.parent == parent && c.id != id && c.name.compare(moved->name, Qt::CaseInsensitive) == 0;
        }))
        return failed(error, QString("There is a collection called %1 there already.").arg(moved->name));
    exec("DELETE FROM collections WHERE removed = 1 AND parent = ? AND name = ? COLLATE NOCASE", {parent, moved->name});
    return exec("UPDATE collections SET parent = ?, updated = ? WHERE id = ?", {parent, Spaces::wallSeconds(*this), id})
           || failed(error, "The library can't be written.");
}

bool Library::removeCollection(qint64 id) {
    const auto gone = collection(id);
    if (!gone)
        return false;
    const Batch batch(*this);
    const qint64 now = Spaces::wallSeconds(*this);
    // Those inside it move up to where it was.
    exec("UPDATE collections SET parent = ?, updated = ? WHERE parent = ? AND removed = 0", {gone->parent, now, id});
    exec("DELETE FROM collection_books WHERE collection_id = ?", {id});
    return exec("UPDATE collections SET removed = 1, rule = '', shelf = '{}', format = '{}', updated = ? WHERE id = ?", {now, id});
}

bool Library::putInCollection(qint64 id, qint64 bookId, bool in) {
    const auto collection = this->collection(id);
    const auto book = this->book(bookId);
    if (!collection || !book)
        return false;
    // What it would be without any choice made by hand for this book: its
    // rule, and what the collections inside it hold.
    Collection ruled = *collection;
    ruled.included.remove(bookId);
    ruled.excluded.remove(bookId);
    const Batch batch(*this);
    exec("UPDATE collections SET updated = ? WHERE id = ?", {Spaces::wallSeconds(*this), id});
    if (in == ruled.contains(*book))
        return exec("DELETE FROM collection_books WHERE collection_id = ? AND book_id = ?", {id, bookId});
    return exec("INSERT OR REPLACE INTO collection_books (collection_id, book_id, included) VALUES (?, ?, ?)", {id, bookId, in});
}

bool Library::followRule(qint64 id, qint64 bookId) {
    const Batch batch(*this);
    exec("UPDATE collections SET updated = ? WHERE id = ?", {Spaces::wallSeconds(*this), id});
    return exec("DELETE FROM collection_books WHERE collection_id = ? AND book_id = ?", {id, bookId});
}

bool Library::setCollectionShelf(qint64 id, const QJsonObject &shelf) {
    const auto now = collection(id);
    if (!now)
        return false;
    if (now->shelf == shelf)
        return true;
    return exec("UPDATE collections SET shelf = ?, updated = ? WHERE id = ?", {compact(shelf), Spaces::wallSeconds(*this), id});
}

bool Library::setCollectionFormat(qint64 id, const QJsonObject &options) {
    const auto now = collection(id);
    if (!now)
        return false;
    const QJsonObject kept = ReadingFormat::fitting(options);
    if (now->format == kept)
        return true;
    return exec("UPDATE collections SET format = ?, updated = ? WHERE id = ?", {compact(kept), Spaces::wallSeconds(*this), id});
}

std::optional<qint64> Library::storeCollection(const Collection &given) {
    const QString named = given.name.simplified();
    if (!nameProblem(named).isEmpty())
        return std::nullopt;
    const Batch batch(*this);
    const qint64 updatedSec = Hlc::seconds(given.updated);
    QSqlQuery same = query("SELECT id FROM collections WHERE parent = ? AND name = ? COLLATE NOCASE", {given.parent, named});
    qint64 id = given.id > 0 || !same.next() ? given.id : same.value(0).toLongLong();
    same.finish();
    if (id == 0) {
        QSqlQuery last = query("SELECT COALESCE(MAX(position), 0) + 1 FROM collections");
        const int position = last.next() ? last.value(0).toInt() : 1;
        last.finish();
        const auto made = insert("INSERT INTO collections (name, position, updated, parent) VALUES (?, ?, ?, ?)",
                                 {named, position, updatedSec, given.parent});
        if (!made)
            return std::nullopt;
        id = *made;
    }
    if (!exec("UPDATE collections SET name = ?, rule = ?, shelf = ?, format = ?, updated = ?, removed = ?, parent = ? WHERE id = ?",
              {named, given.rule.trimmed(), compact(given.shelf), compact(ReadingFormat::fitting(given.format)), updatedSec, given.removed,
               given.parent, id}))
        return std::nullopt;
    exec("DELETE FROM collection_books WHERE collection_id = ?", {id});
    for (const bool in : {true, false})
        for (const qint64 book : in ? given.included : given.excluded)
            exec("INSERT OR REPLACE INTO collection_books (collection_id, book_id, included) "
                 "SELECT ?, id, ? FROM books WHERE id = ?",
                 {id, in, book});
    Spaces::ensureCollectionId(*this, id);
    return id;
}

QList<Collection> Library::collectionsOf(const Book &book) const {
    QList<Collection> in = collections();
    in.removeIf([&book](const Collection &collection) { return !collection.contains(book); });
    return in;
}

std::optional<Collection> Library::formatCollection(qint64 bookId, qint64 preferred) const {
    const auto book = this->book(bookId);
    if (!book)
        return std::nullopt;
    const QList<Collection> in = collectionsOf(*book);
    for (const Collection &collection : in)
        if (collection.id == preferred && !collection.format.isEmpty())
            return collection;
    // The deepest first: "Studie/Python" says more of a book than "Studie".
    std::optional<Collection> best;
    for (const Collection &collection : in)
        if (!collection.format.isEmpty() && (!best || collection.depth > best->depth))
            best = collection;
    return best;
}
