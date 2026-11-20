#include "library.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <array>
#include <functional>
#include <atomic>
#include <ranges>
#include <span>

#include "booksides.h"
#include "changelog.h"
#include "formats.h"
#include "graph.h"
#include "hlc.h"
#include "librarymigrations.h"
#include "log.h"
#include "readingformat.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// A name that is safe as one path component on any filesystem a library
// might be synced to.
[[nodiscard]] QString fileName(QString text, const QString &fallback) {
    static constexpr QStringView unsafe = u"/\\:*?\"<>|";
    for (QChar &c : text)
        if (c.unicode() < 32 || unsafe.contains(c))
            c = ' ';
    text = text.simplified();
    while (text.startsWith('.'))
        text.remove(0, 1);
    text = text.left(80).trimmed();
    while (text.endsWith('.'))
        text.chop(1);
    return text.isEmpty() ? fallback : text;
}

[[nodiscard]] QString folderFor(const BookMeta &meta, qint64 id) {
    return fileName(meta.authors.value(0), "Unknown") + "/" + fileName(meta.title, "Untitled")
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

// A null QString binds as SQL NULL, which the NOT NULL columns refuse; a book
// with no series has the empty string for one.
[[nodiscard]] QVariant bindable(const QVariant &value) {
    return value.typeId() == QMetaType::QString && value.toString().isNull() ? QVariant(QString("")) : value;
}

// A statement with its values in place, ready to run.
[[nodiscard]] QSqlQuery statement(const QString &connection, const QString &sql, const QVariantList &values) {
    QSqlQuery statement(QSqlDatabase::database(connection));
    statement.prepare(sql);
    for (const QVariant &value : values)
        statement.addBindValue(bindable(value));
    return statement;
}

constexpr auto selectBooks = "SELECT books.*, progress.percent, progress.updated FROM books "
                             "LEFT JOIN progress ON progress.book_id = books.id"_L1;

// Says why something didn't work, to a caller that asked, and is the "no" to return.
bool failed(QString *error, const QString &why) {
    if (error)
        *error = why;
    return false;
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

namespace {

// The fields a search can look in one at a time, as "key:value".
constexpr std::array searchKeys{"author"_L1, "title"_L1, "series"_L1, "tag"_L1, "genre"_L1, "format"_L1, "lang"_L1,
                                "publisher"_L1, "about"_L1, "status"_L1, "is"_L1, "rating"_L1, "knows"_L1};

// The words of a search, a quoted phrase as one: `tag:"science fiction"`
// is one word, and so is `"the hobbit"`.
[[nodiscard]] QStringList searchWords(const QString &query, bool *open = nullptr) {
    QStringList words;
    QString word;
    bool quoted = false;
    for (const QChar c : query) {
        if (c == '"') {
            quoted = !quoted;
        } else if (c.isSpace() && !quoted) {
            if (!word.isEmpty())
                words << std::exchange(word, {});
        } else {
            word += c;
        }
    }
    if (!word.isEmpty())
        words << word;
    if (open)
        *open = quoted;
    return words;
}

// What tags say a book is a story, and what tags say it is about the world.
// A tag that is both ("Historical Fiction", "Science Fiction") is a story.
constexpr std::array storyWords{"fiction"_L1, "novel"_L1, "fantasy"_L1, "sci-fi"_L1, "mystery"_L1, "mysteries"_L1,
                                "thriller"_L1, "romance"_L1, "horror"_L1, "crime"_L1, "adventure"_L1, "short stories"_L1,
                                "fairy tale"_L1, "dystopia"_L1, "fables"_L1, "graphic novel"_L1, "detective"_L1};
constexpr std::array worldWords{"nonfiction"_L1, "non-fiction"_L1, "history"_L1, "biography"_L1, "memoir"_L1,
                                "science"_L1, "programming"_L1, "computer"_L1, "software"_L1, "technology"_L1,
                                "philosophy"_L1, "business"_L1, "economics"_L1, "reference"_L1, "textbook"_L1,
                                "self-help"_L1, "psychology"_L1, "mathematics"_L1, "politics"_L1, "essays"_L1,
                                "travel"_L1, "cooking"_L1, "health"_L1, "education"_L1, "engineering"_L1,
                                "management"_L1, "design"_L1, "language"_L1, "religion"_L1, "law"_L1,
                                "nature"_L1, "cookbook"_L1, "parenting"_L1, "medicine"_L1};

[[nodiscard]] bool tagHas(const QString &tag, std::span<const QLatin1StringView> words) {
    return std::ranges::any_of(words, [&tag](QLatin1StringView word) { return tag.contains(word, Qt::CaseInsensitive); });
}

[[nodiscard]] bool storyTag(const QString &tag) {
    return tagHas(tag, storyWords) && !tag.contains("nonfiction"_L1, Qt::CaseInsensitive)
           && !tag.contains("non-fiction"_L1, Qt::CaseInsensitive);
}

[[nodiscard]] bool genreOf(const QStringList &tags, const QString &genre) {
    const QString wanted = genre.toLower().remove(' ');
    if (wanted == "fiction"_L1)
        return std::ranges::any_of(tags, storyTag);
    if (wanted == "nonfiction"_L1 || wanted == "non-fiction"_L1)
        return std::ranges::any_of(tags, [](const QString &tag) { return tagHas(tag, worldWords) && !storyTag(tag); });
    return std::ranges::any_of(tags, [&genre](const QString &tag) { return tag.contains(genre, Qt::CaseInsensitive); });
}

} // namespace

bool Book::matches(const QString &query) const { return matches(query, {}); }

bool Book::matches(const QString &query, const Knows &knows) const {
    const QString everything = meta.title + "\n" + meta.authorText() + "\n" + meta.series + "\n" + meta.tags.join("\n");
    const auto found = [this, &everything, &knows](const QString &word) {
        const QString key = word.section(':', 0, 0).toLower(), value = word.section(':', 1);
        if (word.contains(':')) {
            if (key == "author") return meta.authorText().contains(value, Qt::CaseInsensitive);
            if (key == "title") return meta.title.contains(value, Qt::CaseInsensitive);
            if (key == "series") return meta.series.contains(value, Qt::CaseInsensitive);
            if (key == "tag") return meta.tags.join("\n").contains(value, Qt::CaseInsensitive);
            if (key == "genre") return genreOf(meta.tags, value);
            if (key == "format") return formats.contains(value.toLower());
            if (key == "lang") return meta.language.startsWith(value, Qt::CaseInsensitive);
            if (key == "publisher") return meta.publisher.contains(value, Qt::CaseInsensitive);
            if (key == "about") return meta.description.contains(value, Qt::CaseInsensitive);
            if (key == "status")
                return value.toLower() == "unread" ? status() != "read" && status() != "reading" : status() == value.toLower();
            if (key == "is")
                return value.toLower() == "digital" ? !formats.isEmpty() : value.toLower() == "paper" ? paper
                       : value.toLower() == "wishlist" && held() == "none";
            if (key == "rating") return rating >= value.toInt() && rating > 0;
            if (key == "knows") return knows && knows(*this, value, true);
        }
        return everything.contains(word, Qt::CaseInsensitive) || (knows && knows(*this, word, false));
    };
    // A word with a minus in front is one the book must not match.
    const auto holds = [&found](const QString &word) {
        return word.size() > 1 && word.startsWith('-') ? !found(word.mid(1)) : found(word);
    };
    // Words joined by OR are alternatives: any one group of words, each of
    // which matches, is enough.
    QList<QStringList> groups{{}};
    for (const QString &word : searchWords(query)) {
        if (word == "OR"_L1)
            groups.append(QStringList{});
        else
            groups.last() << word;
    }
    groups.removeIf([](const QStringList &group) { return group.isEmpty(); });
    return groups.isEmpty()
           || std::ranges::any_of(groups, [&holds](const QStringList &group) { return std::ranges::all_of(group, holds); });
}

QString Book::searchProblem(const QString &query) {
    bool open = false;
    const QStringList words = searchWords(query, &open);
    if (open)
        return "A quote is left open; close it with another \".";
    static const QRegularExpression keyed(u"^-?([A-Za-z]+):"_s);
    for (const QString &word : words) {
        const QRegularExpressionMatch match = keyed.match(word);
        const QString key = match.captured(1).toLower();
        // What is known of a book grows as it is read, and a rule holds the
        // same books for everyone who opens the library.
        if (match.hasMatch() && key == "knows"_L1)
            return "A collection's rule goes by the books' own details; knows: is for the search, where what you have read counts.";
        if (match.hasMatch() && std::ranges::none_of(searchKeys, [&key](QLatin1StringView known) { return key == known; })) {
            QStringList keys;
            for (const QLatin1StringView known : searchKeys)
                if (known != "knows"_L1)
                    keys << known + ':';
            return QString("\"%1:\" isn't a field a search looks in; it can be %2.").arg(match.captured(1), keys.join(", "));
        }
    }
    return {};
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
    if (!identifiers.isEmpty())
        object.insert("identifiers", identifiers);
    object.insert("details_at", detailsAt);
    object.insert("reading_at", readingAt);
    return object;
}

QString Library::hashFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

Library::Library(const QString &root)
    : m_root(QDir(root).absolutePath()), m_connection("omabook-" + QUuid::createUuid().toString(QUuid::Id128)) {}

Library::~Library() {
    if (QSqlDatabase::contains(m_connection)) {
        QSqlDatabase::database(m_connection, false).close();
        QSqlDatabase::removeDatabase(m_connection);
    }
}

Library::Batch::Batch(const Library &library, Lock lock) : m_library(library) {
    if (m_library.m_batches++ == 0)
        m_begun = lock == Lock::AtOnce ? QSqlQuery(QSqlDatabase::database(m_library.m_connection)).exec("BEGIN IMMEDIATE")
                                       : QSqlDatabase::database(m_library.m_connection).transaction();
}

Library::Batch::~Batch() {
    if (m_library.m_batches == 1)
        ChangeLog::afterWrite(m_library);
    if (--m_library.m_batches == 0)
        QSqlDatabase::database(m_library.m_connection).commit();
}

QString Library::databasePath() const { return m_root + "/omabook.db"; }

QString Library::backupFolder() const { return m_root + "/.backups"; }

int Library::formatVersion() const {
    QSqlQuery version = query("PRAGMA user_version");
    return version.next() ? version.value(0).toInt() : 0;
}

bool Library::exec(const QString &sql, const QVariantList &values) const {
    if (!statement(m_connection, sql, values).exec())
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
    QSqlQuery insert = statement(m_connection, sql, values);
    if (!insert.exec())
        return std::unexpected(insert.lastError().text());
    const qint64 id = insert.lastInsertId().toLongLong();
    if (m_batches == 0)
        ChangeLog::afterWrite(*this);
    return id;
}

bool Library::open(QString *error) {
    const auto fail = [this, error](const QString &why) {
        qCWarning(lcLibrary).noquote() << m_root << "can't be opened:" << why;
        return failed(error, why);
    };
    if (!QDir().mkpath(m_root))
        return fail("The library folder can't be created: " + m_root);
    QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", m_connection);
    database.setDatabaseName(databasePath());
    // The window and a command can write at once; the later one waits.
    database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=10000");
    if (!database.open())
        return fail("The library can't be opened: " + database.lastError().text());
    const auto setUp = LibraryMigrations::setUp(*this);
    return setUp ? true : fail(setUp.error());
}

QString Library::bookFolder(qint64 id) const {
    QSqlQuery folder = query("SELECT path FROM books WHERE id = ?", {id});
    return folder.next() ? m_root + "/" + folder.value(0).toString() : QString();
}

Library::Added Library::add(const QString &file, const std::optional<BookMeta> &known) {
    const QFileInfo info(file);
    if (!info.isFile() || !info.isReadable())
        return {.error = "There is no readable file at " + file};
    if (!Formats::isBook(file))
        return {.error = info.fileName() + " is not a book format Omabook knows."};
    const QString hash = hashFile(file);
    if (const qint64 existing = bookWithHash(hash))
        return {.id = existing, .duplicate = true, .error = {}};
    // Added on purpose, so no longer something that was thrown away.
    exec("DELETE FROM removed WHERE hash = ?", {hash});

    BookMeta meta = known ? *known : Formats::readMeta(file);
    if (meta.title.isEmpty())
        meta.title = Formats::metaFromFileName(file).title;
    std::optional<ChangeLog::Scope> guard(std::in_place);
    const auto id = insert("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                           "description, tags, added, details_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
                            meta.isbn, meta.published, meta.description, meta.tags.join(", "),
                            Spaces::wallSeconds(*this), Spaces::wallSeconds(*this)});
    if (!id)
        return {.error = "The library can't be written: " + id.error()};
    const QString folder = folderFor(meta, *id);
    exec("UPDATE books SET path = ? WHERE id = ?", {folder, *id});
    guard.reset();

    if (QString error; !addFormat(*id, file, &error)) {
        const ChangeLog::Scope cleanupGuard;
        exec("DELETE FROM books WHERE id = ?", {*id});
        QDir(m_root + "/" + folder).removeRecursively();
        return {.error = error};
    }
    if (!meta.cover.isNull())
        setCover(*id, meta.cover);
    return {.id = *id, .duplicate = false, .error = {}};
}

bool Library::addFormat(qint64 id, const QString &file, QString *error) {
    const auto fail = [error](const QString &why) { return failed(error, why); };
    const auto book = this->book(id);
    if (!book)
        return fail(QString("There is no book %1.").arg(id));
    const QString format = Formats::extension(file);
    if (!Formats::isBook(file))
        return fail(QFileInfo(file).fileName() + " is not a book format Omabook knows.");
    const QString folder = m_root + "/" + book->path;
    if (!QDir().mkpath(folder))
        return fail("The book's folder can't be created: " + folder);

    const QString name = fileName(book->meta.title, "Untitled")
                         + (book->meta.authors.isEmpty() ? QString() : " - " + fileName(book->meta.authors.first(), "Unknown"))
                         + "." + format;
    const QString target = folder + "/" + name;
    const QString hash = hashFile(file);
    // The very file is this book's already: there is nothing to copy.
    const QList<FormatFile> held = formatFiles(id);
    if (std::ranges::any_of(held, [&](const FormatFile &one) { return one.format == format && one.hash == hash; }))
        return true;
    // Copied beside its place and renamed, so a copy cut short never looks
    // like a book. The name is this copy's own, so two imports into one
    // library at once (the window and a command, say) don't trip over each
    // other's.
    static std::atomic<int> copies = 0;
    const QString partial = QString("%1.%2-%3.part").arg(target).arg(QCoreApplication::applicationPid()).arg(++copies);
    const auto copied = [&] {
        QFile::remove(partial);
        if (QFile::copy(file, partial) && QFileInfo(partial).size() == QFileInfo(file).size()) {
            const QString old = formatPath(id, format);
            if (!old.isEmpty() && old != target)
                QFile::remove(old);
            QFile::remove(target);
            if (QFile::rename(partial, target))
                return true;
        }
        QFile::remove(partial);
        // Another import put the very same file there in the meantime.
        return QFileInfo::exists(target) && hashFile(target) == hash;
    };
    if (!copied())
        return fail(QString("%1 can't be copied into the library, to %2").arg(file, target));
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (!exec("INSERT OR REPLACE INTO formats (book_id, format, file, size, hash) VALUES (?, ?, ?, ?, ?)",
              {id, format, name, QFileInfo(target).size(), hash}))
        return fail("The library can't be written.");
    Spaces::ensureBookId(*this, id);
    return true;
}

Library::Added Library::addEntry(const BookMeta &meta, bool paper) {
    if (meta.title.trimmed().isEmpty())
        return {.error = "A book needs a title."};
    const Batch batch(*this);
    const auto id = insert("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                           "description, tags, added, paper, details_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
                            meta.isbn, meta.published, meta.description, meta.tags.join(", "),
                            Spaces::wallSeconds(*this), paper, Spaces::wallSeconds(*this)});
    if (!id)
        return {.error = "The library can't be written: " + id.error()};
    // The folder is made when there is something to put in it: a cover, or
    // a file the book is given later.
    exec("UPDATE books SET path = ? WHERE id = ?", {folderFor(meta, *id), *id});
    if (!meta.cover.isNull())
        setCover(*id, meta.cover);
    Spaces::ensureBookId(*this, *id);
    return {.id = *id, .duplicate = false, .error = {}};
}

bool Library::setReading(qint64 id, const QString &shelf, int rating, const QString &finished) {
    static constexpr std::array shelves{""_L1, "to-read"_L1, "reading"_L1, "read"_L1};
    if (!std::ranges::contains(shelves, shelf))
        return false;
    const int stars = std::clamp(rating, 0, 5);
    // Stamped only when something changes: a sync that says the same again
    // doesn't make the book look read since.
    return exec("UPDATE books SET shelf = ?, rating = ?, finished = ?, reading_at = CASE WHEN shelf IS ? AND rating = ? "
                "AND finished IS ? THEN reading_at ELSE ? END WHERE id = ?",
                {shelf, stars, finished, shelf, stars, finished, Spaces::wallSeconds(*this), id});
}

bool Library::setPaper(qint64 id, bool paper) {
    return exec("UPDATE books SET paper = ?, reading_at = CASE WHEN paper = ? THEN reading_at ELSE ? END WHERE id = ?",
                {paper, paper, Spaces::wallSeconds(*this), id});
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

QStringList Library::entryKeys(const QJsonObject &identifiers, const QString &isbn, const QString &title,
                               const QStringList &authors) {
    static const QRegularExpression other(R"([^\p{L}\p{N}]+)");
    const auto fold = [](QString text) { return text.toCaseFolded().remove(other); };
    QStringList keys;
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        if (const QString value = it.value().toVariant().toString().trimmed(); !value.isEmpty())
            keys << "entry:" + it.key().toLower() + "=" + value.toLower();
    if (const QString number = QString(isbn).remove(other).toLower(); !number.isEmpty())
        keys << "entry:isbn=" + number;
    if (const QString name = fold(title); !name.isEmpty()) {
        QStringList folded;
        for (const QString &author : authors)
            folded << fold(author);
        folded.sort();
        keys << "entry:title=" + name + "|" + folded.join(',');
    }
    keys.removeDuplicates();
    return keys;
}

QStringList Library::entryKeys(const Book &book) {
    return entryKeys(book.identifiers, book.meta.isbn, book.meta.title, book.meta.authors);
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
    for (const FormatFile &format : formats)
        if (!query("SELECT 1 FROM formats WHERE hash = ? AND book_id <> ?", {format.hash, id}).next())
            exec("INSERT OR REPLACE INTO removed (hash, removed_at) VALUES (?, ?)", {format.hash, at.value_or(Spaces::wallSeconds(*this))});
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

bool Library::setCover(qint64 id, const QImage &cover, std::optional<qint64> at) {
    const QString folder = bookFolder(id);
    if (folder.isEmpty() || cover.isNull() || !QDir().mkpath(folder))
        return false;
    // Large enough for a sharp tile on a 4K screen, small enough to load a
    // shelf of them at once.
    const QImage scaled = cover.height() > 900 ? cover.scaledToHeight(900, Qt::SmoothTransformation) : cover;
    const QString file = folder + "/cover.jpg";
    const QString before = hashFile(file);
    if (!scaled.convertToFormat(QImage::Format_RGB32).save(file, "JPG", 88))
        return false;
    // The same picture again is no change to the details.
    if (at || hashFile(file) != before)
        return exec("UPDATE books SET has_cover = 1, details_at = ? WHERE id = ?",
                    {at ? Hlc::seconds(*at) : Spaces::wallSeconds(*this), id});
    return exec("UPDATE books SET has_cover = 1 WHERE id = ?", {id});
}

QString Library::coverPath(qint64 id) const {
    const QString path = bookFolder(id) + "/cover.jpg";
    return QFileInfo::exists(path) ? path : QString();
}

QString Library::formatPath(qint64 id, const QString &format) const {
    QHash<QString, QString> files;
    for (QSqlQuery found = query("SELECT format, file FROM formats WHERE book_id = ?", {id}); found.next();)
        files.insert(found.value(0).toString(), found.value(1).toString());
    const QString chosen = format.isEmpty() ? Formats::byPreference(files.keys()).value(0) : format.toLower();
    return files.contains(chosen) ? bookFolder(id) + "/" + files.value(chosen) : QString();
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
    return {.section = found.value(0).toInt(),
            .fraction = found.value(1).toDouble(),
            .percent = found.value(2).toDouble(),
            .updated = found.value(3).toLongLong()};
}

void Library::setProgress(qint64 id, const Progress &progress) { ChangeLog::setProgress(*this, id, progress); }

QList<FormatFile> Library::formatFiles(qint64 id) const {
    const QString folder = bookFolder(id);
    QList<FormatFile> files;
    for (QSqlQuery found = query("SELECT format, file, hash, size FROM formats WHERE book_id = ?", {id}); found.next();)
        files.append({.format = found.value(0).toString(),
                      .path = folder + "/" + found.value(1).toString(),
                      .hash = found.value(2).toString(),
                      .size = found.value(3).toLongLong()});
    return files;
}

qint64 Library::bookWithHash(const QString &hash) const {
    QSqlQuery found = query("SELECT book_id FROM formats WHERE hash = ?", {hash});
    return found.next() ? found.value(0).toLongLong() : 0;
}

bool Library::wasRemoved(const QString &hash) const { return query("SELECT 1 FROM removed WHERE hash = ?", {hash}).next(); }

Recap Library::notes(qint64 id, const QString &kind) const {
    QSqlQuery found = query("SELECT section, fraction, text, updated FROM notes WHERE book_id = ? AND kind = ?", {id, kind});
    if (!found.next())
        return {};
    return {.section = found.value(0).toInt(),
            .fraction = found.value(1).toDouble(),
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
         {id, kind, notes.section, notes.fraction, notes.text, QDateTime::currentSecsSinceEpoch()});
}

bool Library::merge(qint64 keep, qint64 other, QString *error) {
    const auto kept = book(keep), folded = book(other);
    if (!kept || !folded || keep == other)
        return failed(error, "There are no two such books to merge.");
    for (const FormatFile &file : formatFiles(other))
        if (!kept->formats.contains(file.format) && !addFormat(keep, file.path, error))
            return false;
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
