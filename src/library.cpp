#include "library.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <array>
#include <ranges>

#include "formats.h"
#include "graph.h"
#include "log.h"
#include "readingformat.h"

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
    return book;
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

// A transaction that is rolled back unless it was committed, however the
// function that began it ends.
class Transaction {
public:
    explicit Transaction(QSqlDatabase &database) : m_database(database) { m_database.transaction(); }
    ~Transaction() {
        if (!m_committed)
            m_database.rollback();
    }
    Transaction(const Transaction &) = delete;
    Transaction &operator=(const Transaction &) = delete;

    void commit() {
        m_committed = true;
        m_database.commit();
    }

private:
    QSqlDatabase &m_database;
    bool m_committed = false;
};

} // namespace

QString Book::status() const {
    if (!shelf.isEmpty())
        return shelf;
    return percent >= 98 ? "read" : percent > 0 ? "reading" : QString();
}

QString Book::held() const {
    return !formats.isEmpty() ? (paper ? "both" : "digital") : paper ? "paper" : "none";
}

bool Book::matches(const QString &query) const {
    const QString everything = meta.title + "\n" + meta.authorText() + "\n" + meta.series + "\n" + meta.tags.join("\n");
    const auto found = [this, &everything](const QString &word) {
        const QString key = word.section(':', 0, 0).toLower(), value = word.section(':', 1);
        if (word.contains(':')) {
            if (key == "author") return meta.authorText().contains(value, Qt::CaseInsensitive);
            if (key == "title") return meta.title.contains(value, Qt::CaseInsensitive);
            if (key == "series") return meta.series.contains(value, Qt::CaseInsensitive);
            if (key == "tag") return meta.tags.join("\n").contains(value, Qt::CaseInsensitive);
            if (key == "format") return formats.contains(value.toLower());
            if (key == "lang") return meta.language.startsWith(value, Qt::CaseInsensitive);
            if (key == "status")
                return value.toLower() == "unread" ? status() != "read" && status() != "reading" : status() == value.toLower();
            if (key == "is")
                return value.toLower() == "digital" ? !formats.isEmpty() : value.toLower() == "paper" ? paper
                       : value.toLower() == "wishlist" && held() == "none";
            if (key == "rating") return rating >= value.toInt() && rating > 0;
        }
        return everything.contains(word, Qt::CaseInsensitive);
    };
    return std::ranges::all_of(query.split(' ', Qt::SkipEmptyParts), found);
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

Library::Changes Library::changes() const {
    Changes changes;
    for (QSqlQuery query = this->query("SELECT key, value FROM library_state"); query.next();) {
        const QString key = query.value(0).toString();
        (key == "books" ? changes.books : key == "graph" ? changes.graph : changes.format) = query.value(1).toLongLong();
    }
    return changes;
}

Library::Batch::Batch(const Library &library) : m_library(library) {
    if (m_library.m_batches++ == 0)
        QSqlDatabase::database(m_library.m_connection).transaction();
}

Library::Batch::~Batch() {
    if (--m_library.m_batches == 0)
        QSqlDatabase::database(m_library.m_connection).commit();
}

QString Library::databasePath() const { return m_root + "/omabook.db"; }

QString Library::backupFolder() const { return m_root + "/.backups"; }

int Library::formatVersion() const {
    QSqlQuery version = query("PRAGMA user_version");
    return version.next() ? version.value(0).toInt() : 0;
}

std::expected<void, QString> Library::backUp(int schema) const {
    const QString folder = backupFolder();
    const QString file = QString("%1/omabook-format-%2-%3.db")
                             .arg(folder).arg(schema).arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    if (!QDir().mkpath(folder))
        return std::unexpected(folder + " can't be made.");
    // SQLite writes the copy itself, whole and consistent, also while the
    // write-ahead log holds changes the main file doesn't have yet.
    QSqlQuery copy(QSqlDatabase::database(m_connection));
    QFile::remove(file);
    if (!copy.exec(QString("VACUUM INTO '%1'").arg(QString(file).replace('\'', "''"))))
        return std::unexpected(copy.lastError().text());
    // The last five are kept; older ones go.
    const QFileInfoList kept = QDir(folder).entryInfoList({"omabook-format-*.db"}, QDir::Files, QDir::Time);
    for (const QFileInfo &old : kept | std::views::drop(5))
        QFile::remove(old.filePath());
    return {};
}

bool Library::exec(const QString &sql, const QVariantList &values) const { return statement(m_connection, sql, values).exec(); }

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
    return insert.lastInsertId().toLongLong();
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
    // The write-ahead log needs memory shared between processes, which a
    // folder on another machine (sshfs, NFS, SMB) can't give; there the
    // older rollback journal is the one that works.
    static constexpr std::array<QByteArrayView, 9> remote{"fuse.sshfs", "sshfs", "nfs", "nfs4", "cifs",
                                                          "smb3", "smbfs", "9p", "fuse.rclone"};
    const QByteArray fileSystem = QStorageInfo(m_root).fileSystemType();
    const bool local = !std::ranges::contains(remote, QByteArrayView(fileSystem));
    exec(local ? "PRAGMA journal_mode = WAL" : "PRAGMA journal_mode = DELETE");
    exec("PRAGMA foreign_keys = ON");

    // Each entry takes the database from the version of its index to the next.
    static const QList<QStringList> migrations{
        {
            "CREATE TABLE books (id INTEGER PRIMARY KEY AUTOINCREMENT, title TEXT NOT NULL, authors TEXT NOT NULL DEFAULT '', "
            "series TEXT NOT NULL DEFAULT '', series_index REAL NOT NULL DEFAULT 0, publisher TEXT NOT NULL DEFAULT '', "
            "language TEXT NOT NULL DEFAULT '', isbn TEXT NOT NULL DEFAULT '', published TEXT NOT NULL DEFAULT '', "
            "description TEXT NOT NULL DEFAULT '', tags TEXT NOT NULL DEFAULT '', path TEXT NOT NULL DEFAULT '', "
            "added INTEGER NOT NULL, has_cover INTEGER NOT NULL DEFAULT 0)",
            "CREATE TABLE formats (book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, format TEXT NOT NULL, "
            "file TEXT NOT NULL, size INTEGER NOT NULL, hash TEXT NOT NULL, PRIMARY KEY (book_id, format))",
            "CREATE INDEX formats_hash ON formats(hash)",
            "CREATE TABLE progress (book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
            "section INTEGER NOT NULL, fraction REAL NOT NULL, percent REAL NOT NULL, updated INTEGER NOT NULL)",
            "CREATE TABLE annotations (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, kind TEXT NOT NULL, section INTEGER NOT NULL, "
            "fraction REAL NOT NULL, text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL)",
        },
        // Files that were removed, by hash, so syncing with another computer
        // doesn't bring them back.
        {"CREATE TABLE removed (hash TEXT PRIMARY KEY, removed_at INTEGER NOT NULL)"},
        // The AI's running notes per book, for catching up without rereading.
        {"CREATE TABLE recaps (book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, section INTEGER NOT NULL, "
         "fraction REAL NOT NULL, text TEXT NOT NULL, updated INTEGER NOT NULL)"},
        // Notes of more than one kind per book.
        {"CREATE TABLE notes (book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, kind TEXT NOT NULL, "
         "section INTEGER NOT NULL, fraction REAL NOT NULL, text TEXT NOT NULL, updated INTEGER NOT NULL, "
         "PRIMARY KEY (book_id, kind))",
         "INSERT INTO notes SELECT book_id, 'recap', section, fraction, text, updated FROM recaps",
         "DROP TABLE recaps"},
        // Books that are not files: paper ones and ones only known of, with
        // what is said of their reading and what other sites call them.
        {"ALTER TABLE books ADD COLUMN paper INTEGER NOT NULL DEFAULT 0",
         "ALTER TABLE books ADD COLUMN shelf TEXT NOT NULL DEFAULT ''",
         "ALTER TABLE books ADD COLUMN rating INTEGER NOT NULL DEFAULT 0",
         "ALTER TABLE books ADD COLUMN finished TEXT NOT NULL DEFAULT ''",
         "ALTER TABLE books ADD COLUMN identifiers TEXT NOT NULL DEFAULT '{}'"},
        // The knowledge graph (graph.h): things, how they relate, where in
        // which book each was met, and the days a book was read on.
        {"CREATE TABLE graph_nodes (id INTEGER PRIMARY KEY AUTOINCREMENT, scope TEXT NOT NULL DEFAULT '', kind TEXT NOT NULL, "
         "name TEXT NOT NULL, summary TEXT NOT NULL DEFAULT '', data TEXT NOT NULL DEFAULT '{}', "
         "book_id INTEGER REFERENCES books(id) ON DELETE CASCADE, updated INTEGER NOT NULL)",
         "CREATE INDEX graph_nodes_named ON graph_nodes(scope, kind, name COLLATE NOCASE)",
         "CREATE TABLE graph_edges (id INTEGER PRIMARY KEY AUTOINCREMENT, "
         "source INTEGER NOT NULL REFERENCES graph_nodes(id) ON DELETE CASCADE, "
         "target INTEGER NOT NULL REFERENCES graph_nodes(id) ON DELETE CASCADE, kind TEXT NOT NULL, "
         "note TEXT NOT NULL DEFAULT '', book_id INTEGER NOT NULL DEFAULT 0, section INTEGER NOT NULL DEFAULT 0, "
         "fraction REAL NOT NULL DEFAULT 0, UNIQUE (source, target, kind))",
         "CREATE INDEX graph_edges_target ON graph_edges(target)",
         "CREATE TABLE graph_seen (node_id INTEGER NOT NULL REFERENCES graph_nodes(id) ON DELETE CASCADE, "
         "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, section INTEGER NOT NULL, fraction REAL NOT NULL, "
         "PRIMARY KEY (node_id, book_id))",
         "CREATE TABLE reading_log (book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, day TEXT NOT NULL, "
         "from_percent REAL NOT NULL, to_percent REAL NOT NULL, PRIMARY KEY (book_id, day))"},
        // Counters the database keeps itself, one for what the shelf shows
        // and one for the knowledge graph, so a window that sees the file
        // change can tell which changed (often only the graph, while a book
        // is being read) without reading the whole library again.
        {"CREATE TABLE library_state (key TEXT PRIMARY KEY, value INTEGER NOT NULL)",
         "INSERT INTO library_state (key, value) VALUES ('books', 0), ('graph', 0)",
         "CREATE TRIGGER books_insert_counted AFTER INSERT ON books BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER books_update_counted AFTER UPDATE ON books BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER books_delete_counted AFTER DELETE ON books BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER formats_insert_counted AFTER INSERT ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER formats_update_counted AFTER UPDATE ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER formats_delete_counted AFTER DELETE ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER progress_insert_counted AFTER INSERT ON progress BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER progress_update_counted AFTER UPDATE ON progress BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER progress_delete_counted AFTER DELETE ON progress BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; END",
         "CREATE TRIGGER graph_nodes_insert_counted AFTER INSERT ON graph_nodes BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_nodes_update_counted AFTER UPDATE ON graph_nodes BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_nodes_delete_counted AFTER DELETE ON graph_nodes BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_edges_insert_counted AFTER INSERT ON graph_edges BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_edges_update_counted AFTER UPDATE ON graph_edges BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_edges_delete_counted AFTER DELETE ON graph_edges BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_seen_insert_counted AFTER INSERT ON graph_seen BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_seen_update_counted AFTER UPDATE ON graph_seen BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_seen_delete_counted AFTER DELETE ON graph_seen BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END"},
        // How a book is set on the page where it isn't as the defaults are
        // (readingformat.h): only the options it has of its own, as a JSON
        // object. Counted apart, so the reader follows a change made
        // elsewhere without the shelf being read again.
        {"CREATE TABLE book_format (book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
         "options TEXT NOT NULL DEFAULT '{}')",
         "INSERT INTO library_state (key, value) VALUES ('format', 0)",
         "CREATE TRIGGER book_format_insert_counted AFTER INSERT ON book_format BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'format'; END",
         "CREATE TRIGGER book_format_update_counted AFTER UPDATE ON book_format BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'format'; END",
         "CREATE TRIGGER book_format_delete_counted AFTER DELETE ON book_format BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'format'; END"},
        // A book's longest line is a measure in characters ("measure"), no
        // longer pixels ("line_width"): what it held at the book's own size
        // of type (or the default 19), half the size to a character, as
        // PageLayout::measureFromPixels counts it.
        {"UPDATE book_format SET options = json_remove(json_insert(options, '$.measure', "
         "MAX(40, MIN(90, CAST(ROUND(json_extract(options, '$.line_width') * 2.0 "
         "/ MAX(1, COALESCE(json_extract(options, '$.font_size'), 19))) AS INTEGER)))), '$.line_width') "
         "WHERE json_valid(options) AND json_type(options, '$.line_width') IN ('integer', 'real')"},
        // When each thing of the knowledge graph was revealed (graph.h):
        // every summary a thing had and every other name it was given, each
        // with its place in the book (none for one put in by hand) and a few
        // words of the text there to find the spot by, and those words for
        // where a thing was met and a relation learned too. What the graph
        // held before counts as known from where each thing was first met.
        // And the furthest each book was ever read, which the database keeps
        // itself as the reading place moves: going back to an earlier page
        // doesn't make what was read since a surprise again.
        {"CREATE TABLE graph_facts (id INTEGER PRIMARY KEY AUTOINCREMENT, "
         "node_id INTEGER NOT NULL REFERENCES graph_nodes(id) ON DELETE CASCADE, kind TEXT NOT NULL, value TEXT NOT NULL, "
         "book_id INTEGER REFERENCES books(id) ON DELETE CASCADE, section INTEGER NOT NULL DEFAULT 0, "
         "fraction REAL NOT NULL DEFAULT 0, anchor TEXT NOT NULL DEFAULT '')",
         "CREATE INDEX graph_facts_node ON graph_facts(node_id)",
         "ALTER TABLE graph_edges ADD COLUMN anchor TEXT NOT NULL DEFAULT ''",
         "ALTER TABLE graph_seen ADD COLUMN anchor TEXT NOT NULL DEFAULT ''",
         "INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction) SELECT n.id, 'summary', n.summary, "
         "s.book_id, s.section, s.fraction FROM graph_nodes AS n JOIN graph_seen AS s ON s.node_id = n.id WHERE n.summary <> ''",
         "INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction) SELECT n.id, 'alias', a.value, "
         "s.book_id, s.section, s.fraction FROM graph_nodes AS n JOIN graph_seen AS s ON s.node_id = n.id, "
         "json_each(CASE WHEN json_valid(n.data) THEN n.data ELSE '{}' END, '$.aliases') AS a WHERE a.type = 'text'",
         "CREATE TRIGGER graph_facts_insert_counted AFTER INSERT ON graph_facts BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_facts_update_counted AFTER UPDATE ON graph_facts BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "CREATE TRIGGER graph_facts_delete_counted AFTER DELETE ON graph_facts BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'graph'; END",
         "INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) "
         "SELECT book_id, 'furthest', section, fraction, '', updated FROM progress",
         "CREATE TRIGGER progress_furthest_insert AFTER INSERT ON progress BEGIN "
         "INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) "
         "SELECT NEW.book_id, 'furthest', NEW.section, NEW.fraction, '', NEW.updated WHERE NOT EXISTS (SELECT 1 FROM notes "
         "WHERE book_id = NEW.book_id AND kind = 'furthest' AND (section > NEW.section OR (section = NEW.section AND fraction >= NEW.fraction))); END",
         "CREATE TRIGGER progress_furthest_update AFTER UPDATE ON progress BEGIN "
         "INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) "
         "SELECT NEW.book_id, 'furthest', NEW.section, NEW.fraction, '', NEW.updated WHERE NOT EXISTS (SELECT 1 FROM notes "
         "WHERE book_id = NEW.book_id AND kind = 'furthest' AND (section > NEW.section OR (section = NEW.section AND fraction >= NEW.fraction))); END"},
    };

    // Asked and done with before anything changes: a statement left open
    // would keep a migration from dropping a table.
    const int schema = formatVersion();
    if (schema > migrations.size())
        return fail(QString("This library was last opened by a newer Omabook (its format is %1; this Omabook knows up to %2). "
                            "Update Omabook on this computer, or restore a copy from before that from %3.")
                        .arg(schema).arg(migrations.size()).arg(backupFolder()));
    // A library about to be changed to a newer format is copied first, so a
    // step that goes wrong, or a wish to go back to an older Omabook, never
    // costs the library. One that can't be copied is left as it is.
    if (schema > 0 && schema < migrations.size()) {
        if (const auto kept = backUp(schema); !kept)
            return fail("The library needs bringing up to date, but a copy of it couldn't be kept first, so it was left alone: "
                        + kept.error());
    }
    if (schema < migrations.size())
        qCInfo(lcLibrary).noquote() << "bringing" << m_root << "from format" << schema << "to" << migrations.size();
    for (int step = schema; step < migrations.size(); ++step) {
        database.transaction();
        for (const QString &statement : migrations.at(step) + QStringList{QString("PRAGMA user_version = %1").arg(step + 1)}) {
            if (!exec(statement)) {
                database.rollback();
                return fail("The library can't be set up: " + database.lastError().text());
            }
        }
        database.commit();
    }
    return true;
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
    QSqlDatabase database = QSqlDatabase::database(m_connection);
    Transaction transaction(database);
    const auto id = insert("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                           "description, tags, added) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
                            meta.isbn, meta.published, meta.description, meta.tags.join(", "),
                            QDateTime::currentSecsSinceEpoch()});
    if (!id)
        return {.error = "The library can't be written: " + id.error()};
    const QString folder = folderFor(meta, *id);
    exec("UPDATE books SET path = ? WHERE id = ?", {folder, *id});
    transaction.commit();

    if (QString error; !addFormat(*id, file, &error)) {
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
    // Copied beside its place and renamed, so a copy cut short never looks
    // like a book.
    const QString partial = target + ".part";
    QFile::remove(partial);
    if (!QFile::copy(file, partial) || QFileInfo(partial).size() != QFileInfo(file).size()) {
        QFile::remove(partial);
        return fail("The book can't be copied into the library: " + target);
    }
    const QString old = formatPath(id, format);
    if (!old.isEmpty())
        QFile::remove(old);
    QFile::remove(target);
    if (!QFile::rename(partial, target)) {
        QFile::remove(partial);
        return fail("The book can't be copied into the library: " + target);
    }
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    if (!exec("INSERT OR REPLACE INTO formats (book_id, format, file, size, hash) VALUES (?, ?, ?, ?, ?)",
              {id, format, name, QFileInfo(target).size(), hashFile(target)}))
        return fail("The library can't be written.");
    return true;
}

Library::Added Library::addEntry(const BookMeta &meta, bool paper) {
    if (meta.title.trimmed().isEmpty())
        return {.error = "A book needs a title."};
    const auto id = insert("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                           "description, tags, added, paper) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                           {meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher, meta.language,
                            meta.isbn, meta.published, meta.description, meta.tags.join(", "),
                            QDateTime::currentSecsSinceEpoch(), paper});
    if (!id)
        return {.error = "The library can't be written: " + id.error()};
    // The folder is made when there is something to put in it: a cover, or
    // a file the book is given later.
    exec("UPDATE books SET path = ? WHERE id = ?", {folderFor(meta, *id), *id});
    if (!meta.cover.isNull())
        setCover(*id, meta.cover);
    return {.id = *id, .duplicate = false, .error = {}};
}

bool Library::setReading(qint64 id, const QString &shelf, int rating, const QString &finished) {
    static constexpr std::array shelves{""_L1, "to-read"_L1, "reading"_L1, "read"_L1};
    return std::ranges::contains(shelves, shelf)
           && exec("UPDATE books SET shelf = ?, rating = ?, finished = ? WHERE id = ?", {shelf, std::clamp(rating, 0, 5), finished, id});
}

bool Library::setPaper(qint64 id, bool paper) { return exec("UPDATE books SET paper = ? WHERE id = ?", {paper, id}); }

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

bool Library::setIdentifier(qint64 id, const QString &scheme, const QString &value) {
    const auto found = book(id);
    if (!found)
        return false;
    QJsonObject identifiers = found->identifiers;
    if (value.isEmpty())
        identifiers.remove(scheme);
    else
        identifiers.insert(scheme, value);
    return exec("UPDATE books SET identifiers = ? WHERE id = ?",
                {QString::fromUtf8(QJsonDocument(identifiers).toJson(QJsonDocument::Compact)), id});
}

qint64 Library::bookWithIdentifier(const QString &scheme, const QString &value) const {
    if (value.isEmpty())
        return 0;
    const QList<Book> all = books();
    const auto called = std::ranges::find_if(all, [&scheme, &value](const Book &book) { return book.identifiers.value(scheme).toString() == value; });
    return called != all.end() ? called->id : 0;
}

bool Library::remove(qint64 id, QString *error) {
    const QString folder = bookFolder(id);
    if (folder.isEmpty())
        return failed(error, QString("There is no book %1.").arg(id));
    for (const FormatFile &format : formatFiles(id))
        exec("INSERT OR REPLACE INTO removed (hash, removed_at) VALUES (?, ?)", {format.hash, QDateTime::currentSecsSinceEpoch()});
    // To the trash, where a mistake can be undone; a filesystem with no
    // trash gets the folder removed, since the user asked for it to go.
    if (QFileInfo::exists(folder) && !QFile::moveToTrash(folder) && !QDir(folder).removeRecursively())
        return failed(error, "The book's folder can't be removed: " + folder);
    QDir().rmdir(QFileInfo(folder).absolutePath()); // the author's folder, when it is now empty
    return exec("DELETE FROM books WHERE id = ?", {id});
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

bool Library::setMeta(qint64 id, const BookMeta &meta, QString *error) {
    const auto before = book(id);
    if (!before)
        return failed(error, QString("There is no book %1.").arg(id));
    if (!exec("UPDATE books SET title = ?, authors = ?, series = ?, series_index = ?, publisher = ?, language = ?, "
              "isbn = ?, published = ?, description = ?, tags = ? WHERE id = ?",
              {meta.title.isEmpty() ? before->meta.title : meta.title, meta.authorText(), meta.series, meta.seriesIndex,
               meta.publisher, meta.language, meta.isbn, meta.published, meta.description, meta.tags.join(", "), id}))
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
        setCover(id, meta.cover);
    return true;
}

bool Library::setCover(qint64 id, const QImage &cover) {
    const QString folder = bookFolder(id);
    if (folder.isEmpty() || cover.isNull() || !QDir().mkpath(folder))
        return false;
    // Large enough for a sharp tile on a 4K screen, small enough to load a
    // shelf of them at once.
    const QImage scaled = cover.height() > 900 ? cover.scaledToHeight(900, Qt::SmoothTransformation) : cover;
    if (!scaled.convertToFormat(QImage::Format_RGB32).save(folder + "/cover.jpg", "JPG", 88))
        return false;
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

void Library::setProgress(qint64 id, const Progress &progress) {
    exec("INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, ?, ?, ?)",
         {id, progress.section, progress.fraction, progress.percent,
          progress.updated > 0 ? progress.updated : QDateTime::currentSecsSinceEpoch()});
}

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
    const auto comesBefore = [id, &current, &series](const Book &other) {
        return other.id != id && other.meta.seriesIndex < current->meta.seriesIndex
               && other.meta.series.trimmed().compare(series, Qt::CaseInsensitive) == 0;
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
        if (!exec("UPDATE books SET identifiers = ?, paper = ?, shelf = ?, rating = ?, finished = ? WHERE id = ?",
                  {QString::fromUtf8(QJsonDocument(identifiers).toJson(QJsonDocument::Compact)), kept->paper || folded->paper,
                   shelf, kept->rating > 0 ? kept->rating : folded->rating,
                   kept->finished.isEmpty() ? folded->finished : kept->finished, keep}))
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
    if (!remove(other, error))
        return false;
    // The node says what the book is now: its status, stars and formats.
    Graph(*this).bookNode(keep);
    return true;
}

qint64 Library::addAnnotation(qint64 bookId, const Annotation &annotation) {
    return insert("INSERT INTO annotations (book_id, kind, section, fraction, text, note, created) VALUES (?, ?, ?, ?, ?, ?, ?)",
                  {bookId, annotation.kind, annotation.section, annotation.fraction, annotation.text, annotation.note,
                   QDateTime::currentSecsSinceEpoch()})
        .value_or(0);
}

void Library::removeAnnotation(qint64 annotationId) { exec("DELETE FROM annotations WHERE id = ?", {annotationId}); }

QList<Annotation> Library::annotations(qint64 bookId) const {
    QList<Annotation> annotations;
    for (QSqlQuery found = query("SELECT id, kind, section, fraction, text, note, created FROM annotations WHERE book_id = ? "
                                 "ORDER BY section, fraction",
                                 {bookId});
         found.next();)
        annotations.append({.id = found.value(0).toLongLong(),
                            .kind = found.value(1).toString(),
                            .section = found.value(2).toInt(),
                            .fraction = found.value(3).toDouble(),
                            .text = found.value(4).toString(),
                            .note = found.value(5).toString(),
                            .created = found.value(6).toLongLong()});
    return annotations;
}
