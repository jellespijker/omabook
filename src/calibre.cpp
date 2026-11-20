#include "calibre.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <algorithm>
#include <array>

#include "htmlclean.h"

using namespace Qt::StringLiterals;

namespace {

// Closes and unregisters the connection when the import is done, however it ends.
class Connection {
public:
    explicit Connection(const QString &file) : m_name("calibre-" + QUuid::createUuid().toString(QUuid::Id128)) {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", m_name);
        database.setDatabaseName(file);
        // Calibre may be running; reading never gets in its way.
        database.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        m_open = database.open();
        if (!m_open)
            m_error = database.lastError().text();
    }
    ~Connection() {
        QSqlDatabase::database(m_name, false).close();
        QSqlDatabase::removeDatabase(m_name);
    }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return m_open; }
    [[nodiscard]] QString error() const { return m_error; }
    [[nodiscard]] QSqlQuery query(const QString &sql) const {
        QSqlQuery query(QSqlDatabase::database(m_name));
        query.exec(sql);
        return query;
    }

private:
    QString m_name, m_error;
    bool m_open = false;
};

// The values linked to each book through one of Calibre's link tables, in
// the order Calibre keeps them.
[[nodiscard]] QHash<qint64, QStringList> linked(const Connection &calibre, const QString &sql) {
    QHash<qint64, QStringList> values;
    for (QSqlQuery query = calibre.query(sql); query.next();)
        values[query.value(0).toLongLong()] << query.value(1).toString();
    return values;
}

// "date_read", "Date Read" and "#dateread" are one name.
[[nodiscard]] QString plainName(const QString &name) {
    QString plain;
    for (const QChar letter : name.toLower())
        if (letter.isLetterOrNumber())
            plain += letter;
    return plain;
}

// The chapter file a place in Calibre's viewer is in. Calibre writes a place
// as "epubcfi(/8/2/4...)", whose first step is twice the file's number in
// reading order, counted from one.
[[nodiscard]] int sectionOfCfi(const QString &cfi) {
    static const QRegularExpression first("^(?:epubcfi\\()?/(\\d+)");
    const auto match = first.match(cfi);
    return match.hasMatch() ? std::max(0, match.captured(1).toInt() / 2 - 1) : 0;
}

// Calibre writes "no date" as the year 101, and dates with their time.
[[nodiscard]] QString dayOf(const QString &stamp) {
    const QString day = stamp.left(10);
    static const QRegularExpression date("^\\d{4}-\\d{2}-\\d{2}$");
    return date.match(day).hasMatch() && !day.startsWith("0101") ? day : QString();
}

// Of two shelves a book is on at once, the one that says most about now:
// a book being read again is being read.
[[nodiscard]] QString strongerShelf(const QString &a, const QString &b) {
    // From the weakest to the strongest; a shelf that isn't one of these is weaker than any.
    static constexpr std::array order{""_L1, "to-read"_L1, "read"_L1, "reading"_L1};
    const auto strength = [](const QString &shelf) {
        const auto found = std::ranges::find(order, shelf);
        return found != order.end() ? found - order.begin() : -1;
    };
    return strength(a) >= strength(b) ? a : b;
}

// What the columns people add to Calibre say about each book's reading.
// A column Calibre keeps "normalized" holds its values once, in a table of
// their own, with a link table beside it; the others hold them per book.
void readCustomColumns(const Connection &calibre, QHash<qint64, Calibre::Reading> &readings) {
    QSqlQuery columns = calibre.query("SELECT id, label, name, datatype, normalized FROM custom_columns WHERE mark_for_delete = 0");
    while (columns.next()) {
        const QString role = Calibre::columnRole(columns.value(1).toString(), columns.value(2).toString(), columns.value(3).toString());
        if (role.isEmpty())
            continue;
        const QString table = "custom_column_" + QString::number(columns.value(0).toLongLong());
        const QString sql = columns.value(4).toBool()
                                ? QString("SELECT l.book, c.value FROM books_%1_link l JOIN %1 c ON c.id = l.value").arg(table)
                                : QString("SELECT book, value FROM %1").arg(table);
        for (QSqlQuery values = calibre.query(sql); values.next();) {
            Calibre::Reading &reading = readings[values.value(0).toLongLong()];
            const QVariant value = values.value(1);
            if (role == "shelf") {
                reading.shelf = strongerShelf(reading.shelf, Calibre::shelfFor(value.toString()));
            } else if (role == "read") {
                if (value.toBool())
                    reading.shelf = strongerShelf(reading.shelf, "read");
            } else if (role == "finished") {
                if (const QString day = dayOf(value.toString()); !day.isEmpty()) {
                    reading.finished = day;
                    reading.shelf = strongerShelf(reading.shelf, "read");
                }
            } else if (role == "progress") {
                // A percentage kept by hand has no time: it only fills in
                // where Calibre's viewer has no place of its own.
                if (reading.readAt == 0)
                    reading.percent = std::clamp(value.toDouble(), 0.0, 100.0);
            } else if (role == "rating" && reading.rating == 0) {
                // Calibre's own stars, read before these, come first.
                reading.rating = columns.value(3).toString() == "rating" ? Calibre::starsFor(value.toInt())
                                                                         : std::clamp(value.toInt(), 0, 5);
            }
        }
    }
}

// Highlights, their notes and bookmarks from Calibre's viewer, each a small
// JSON document. A highlight with a note is a highlight and a margin note on
// the same words, as Omabook keeps them; each has a name of its own, so the
// note of a highlight brought in before notes were comes along the next time.
void readNotes(const Connection &calibre, QHash<qint64, Calibre::Reading> &readings) {
    QSqlQuery notes = calibre.query("SELECT book, annot_id, annot_type, annot_data, timestamp FROM annotations ORDER BY timestamp");
    while (notes.next()) {
        const QJsonObject data = QJsonDocument::fromJson(notes.value(3).toString().toUtf8()).object();
        const QString kind = notes.value(2).toString();
        // A highlight taken away again stays in Calibre's table, marked.
        if (data.value("removed").toBool() || (kind != "highlight" && kind != "bookmark"))
            continue;
        QList<Calibre::Note> &kept = readings[notes.value(0).toLongLong()].notes;
        const QString id = notes.value(1).toString();
        const auto created = static_cast<qint64>(notes.value(4).toDouble());
        if (kind == "bookmark") {
            kept.append({.id = "bookmark:" + id,
                         .kind = kind,
                         .section = sectionOfCfi(data.value("pos").toString()),
                         .text = data.value("title").toString(),
                         .note = {},
                         .created = created,
                         .color = {}});
            continue;
        }
        const Calibre::Note highlight{.id = "highlight:" + id,
                                      .kind = kind,
                                      .section = data.value("spine_index").toInt(),
                                      .text = data.value("highlighted_text").toString(),
                                      .note = {},
                                      .created = created,
                                      .color = Calibre::colorOf(data.value("style").toObject())};
        kept.append(highlight);
        if (const QString note = data.value("notes").toString().trimmed(); !note.isEmpty())
            kept.append({.id = "margin-note:" + id,
                         .kind = "margin-note",
                         .section = highlight.section,
                         .text = highlight.text,
                         .note = note,
                         .created = created,
                         .color = {}});
    }
}

[[nodiscard]] QHash<qint64, Calibre::Reading> readReadings(const Connection &calibre) {
    QHash<qint64, Calibre::Reading> readings;
    for (QSqlQuery stars = calibre.query("SELECT l.book, r.rating FROM books_ratings_link l JOIN ratings r ON r.id = l.rating"); stars.next();)
        readings[stars.value(0).toLongLong()].rating = Calibre::starsFor(stars.value(1).toInt());
    // One place per reader and device; the one read last is where the book is.
    for (QSqlQuery places = calibre.query("SELECT book, cfi, epoch, pos_frac FROM last_read_positions ORDER BY epoch"); places.next();) {
        Calibre::Reading &reading = readings[places.value(0).toLongLong()];
        reading.section = sectionOfCfi(places.value(1).toString());
        reading.readAt = qint64(places.value(2).toDouble());
        reading.percent = std::clamp(places.value(3).toDouble() * 100, 0.0, 100.0);
    }
    readCustomColumns(calibre, readings);
    readNotes(calibre, readings);
    for (Calibre::Reading &reading : readings) {
        // A book read to its end in Calibre was read, whatever page Omabook
        // later opens it at.
        if (reading.shelf.isEmpty() && reading.percent >= 98)
            reading.shelf = "read";
    }
    return readings;
}

} // namespace

QString Calibre::columnRole(const QString &label, const QString &name, const QString &datatype) {
    static const QSet<QString> shelves{"shelf", "shelves", "status", "readstatus", "readingstatus", "readingstate", "bookshelf",
                                       "bookshelves", "goodreadsshelf", "goodreadsshelves", "grshelf", "grshelves",
                                       "exclusiveshelf",
                                       // What the Goodreads Sync plugin proposes for the column shelves are kept in.
                                       "goodreadstags"};
    static const QSet<QString> read{"read", "isread", "hasread", "finished", "completed", "done", "gelezen"};
    static const QSet<QString> finished{"dateread", "readdate", "readon", "finished", "finishedon", "datefinished", "completed",
                                        "datecompleted", "lastread", "grdateread", "goodreadsdateread"};
    static const QSet<QString> progress{"progress", "readingprogress", "percentread", "readpercent", "grprogress",
                                        "goodreadsprogress",
                                        // Goodreads Sync's own name for it.
                                        "goodreadsreadingprogress"};
    // Not "grrating": that is what the Goodreads metadata plugin calls the
    // column for what all of Goodreads' readers rate a book, not the
    // reader's own stars ("goodreadsrating" is Goodreads Sync's for those).
    static const QSet<QString> ratings{"myrating", "rating", "userrating", "stars", "goodreadsrating", "grmyrating",
                                       "goodreadsmyrating"};
    const std::array names{plainName(label), plainName(name)};
    const auto among = [&names](const QSet<QString> &known) {
        return std::ranges::any_of(names, [&known](const QString &name) { return known.contains(name); });
    };
    const bool words = datatype == "text" || datatype == "enumeration";
    if (words && among(shelves))
        return "shelf";
    // "Read: Yes" kept as words rather than as a tick.
    if (words && among(read))
        return "shelf";
    if (datatype == "bool" && among(read))
        return "read";
    if (datatype == "datetime" && among(finished))
        return "finished";
    if ((datatype == "int" || datatype == "float") && among(progress))
        return "progress";
    // A float called "rating" is a site's average, not the reader's own.
    if ((datatype == "rating" || datatype == "int") && among(ratings))
        return "rating";
    return {};
}

QString Calibre::shelfFor(const QString &value) {
    static const QHash<QString, QString> shelves{
        {"read", "read"}, {"yes", "read"}, {"true", "read"}, {"finished", "read"}, {"done", "read"}, {"completed", "read"},
        {"gelezen", "read"},
        {"reading", "reading"}, {"currentlyreading", "reading"}, {"inprogress", "reading"}, {"started", "reading"},
        {"toread", "to-read"}, {"wanttoread", "to-read"}, {"tbr", "to-read"}, {"queued", "to-read"}, {"next", "to-read"},
        {"wishlist", "to-read"},
    };
    return shelves.value(plainName(value));
}

QString Calibre::colorOf(const QJsonObject &style) {
    // Calibre's own colours by name; red is nearest Omabook's orange. A
    // colour of one's own, or a line rather than a colour, is the first.
    static const QHash<QString, QString> colors{{"yellow", "yellow"}, {"green", "green"}, {"blue", "blue"},
                                                {"purple", "purple"}, {"red", "orange"},  {"orange", "orange"}};
    if (style.value("kind").toString() != "color" || style.value("type").toString() != "builtin")
        return {};
    return colors.value(style.value("which").toString().toLower());
}

int Calibre::starsFor(int calibreRating) { return calibreRating <= 0 ? 0 : std::clamp((calibreRating + 1) / 2, 1, 5); }

bool Calibre::isLibrary(const QString &folder) { return QFileInfo(folder + "/metadata.db").isFile(); }

int Calibre::bookCount(const QString &folder) {
    const Connection calibre(folder + "/metadata.db");
    if (!calibre.isOpen())
        return -1;
    QSqlQuery count = calibre.query("SELECT count(*) FROM books");
    return count.next() ? count.value(0).toInt() : -1;
}

Calibre::Books Calibre::read(const QString &folder) {
    Books result;
    const Connection calibre(folder + "/metadata.db");
    if (!calibre.isOpen())
        return {.books = {}, .error = "The Calibre library can't be read: " + calibre.error()};
    // Calibre writes a comma inside a name as a bar.
    auto authors = linked(calibre, "SELECT l.book, a.name FROM books_authors_link l JOIN authors a ON a.id = l.author ORDER BY l.id");
    for (QStringList &names : authors)
        for (QString &name : names)
            name.replace('|', ',');
    const auto series = linked(calibre, "SELECT l.book, s.name FROM books_series_link l JOIN series s ON s.id = l.series");
    const auto tags = linked(calibre, "SELECT l.book, t.name FROM books_tags_link l JOIN tags t ON t.id = l.tag ORDER BY l.id");
    const auto publishers = linked(calibre, "SELECT l.book, p.name FROM books_publishers_link l JOIN publishers p ON p.id = l.publisher");
    const auto languages = linked(calibre, "SELECT l.book, g.lang_code FROM books_languages_link l JOIN languages g ON g.id = l.lang_code ORDER BY l.item_order");
    const auto isbns = linked(calibre, "SELECT book, val FROM identifiers WHERE type = 'isbn'");
    QHash<qint64, QMap<QString, QString>> identifiers;
    for (QSqlQuery query = calibre.query("SELECT book, type, val FROM identifiers"); query.next();)
        if (!query.value(1).toString().isEmpty() && !query.value(2).toString().isEmpty())
            identifiers[query.value(0).toLongLong()].insert(query.value(1).toString().toLower(), query.value(2).toString());
    // Older libraries have no uuid; the query then simply finds nothing.
    QHash<qint64, QString> uuids;
    for (QSqlQuery query = calibre.query("SELECT id, uuid FROM books"); query.next();)
        uuids.insert(query.value(0).toLongLong(), query.value(1).toString());
    const QHash<qint64, Reading> readings = readReadings(calibre);
    const auto comments = linked(calibre, "SELECT book, text FROM comments");
    // "EPUB" and the file's name without its extension.
    QHash<qint64, QList<QPair<QString, QString>>> files;
    for (QSqlQuery query = calibre.query("SELECT book, format, name FROM data ORDER BY id"); query.next();)
        files[query.value(0).toLongLong()].append({query.value(1).toString(), query.value(2).toString()});

    QSqlQuery books = calibre.query("SELECT id, title, series_index, pubdate, path, has_cover FROM books ORDER BY id");
    if (books.lastError().isValid())
        return {.books = {}, .error = "This doesn't look like a Calibre library: " + books.lastError().text()};
    while (books.next()) {
        const qint64 id = books.value(0).toLongLong();
        const QString path = folder + "/" + books.value(4).toString();
        Book book;
        book.id = id;
        book.uuid = uuids.value(id);
        book.identifiers = identifiers.value(id);
        book.reading = readings.value(id);
        book.meta.title = books.value(1).toString();
        book.meta.authors = authors.value(id);
        // Calibre's name for "nobody".
        book.meta.authors.removeAll("Unknown");
        book.meta.series = series.value(id).value(0);
        book.meta.seriesIndex = book.meta.series.isEmpty() ? 0 : books.value(2).toDouble();
        book.meta.tags = tags.value(id);
        book.meta.publisher = publishers.value(id).value(0);
        book.meta.language = languages.value(id).value(0);
        book.meta.isbn = isbns.value(id).value(0);
        const QString comment = comments.value(id).value(0);
        book.meta.description = comment.contains('<') ? htmlToText(comment) : comment.trimmed();
        // "0101-01-01" is how Calibre writes "no date".
        const QString published = books.value(3).toString().left(10);
        if (!published.startsWith("0101"))
            book.meta.published = published;
        for (const auto &[format, name] : files.value(id)) {
            const QString file = path + "/" + name + "." + format.toLower();
            if (QFileInfo(file).isFile())
                book.files << file;
        }
        if (books.value(5).toBool() && QFileInfo(path + "/cover.jpg").isFile())
            book.cover = path + "/cover.jpg";
        result.books << book;
    }
    return result;
}
