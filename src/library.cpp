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

#include "formats.h"

namespace {

// A name that is safe as one path component on any filesystem a library
// might be synced to.
QString fileName(QString text, const QString &fallback) {
    for (QChar &c : text)
        if (c.unicode() < 32 || QStringLiteral("/\\:*?\"<>|").contains(c))
            c = ' ';
    text = text.simplified();
    while (text.startsWith('.'))
        text.remove(0, 1);
    text = text.left(80).trimmed();
    while (text.endsWith('.'))
        text.chop(1);
    return text.isEmpty() ? fallback : text;
}

QString folderFor(const BookMeta &meta, qint64 id) {
    return fileName(meta.authors.value(0), "Unknown") + "/" + fileName(meta.title, "Untitled")
           + QString(" (%1)").arg(id);
}

Book bookFrom(const QSqlQuery &query) {
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
QVariant bindable(const QVariant &value) {
    return value.typeId() == QMetaType::QString && value.toString().isNull() ? QVariant(QString("")) : value;
}

const char *selectBooks = "SELECT books.*, progress.percent, progress.updated FROM books "
                          "LEFT JOIN progress ON progress.book_id = books.id";

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
    for (const QString &word : query.split(' ', Qt::SkipEmptyParts)) {
        const QString key = word.section(':', 0, 0).toLower(), value = word.section(':', 1);
        bool found;
        if (word.contains(':') && key == "author") found = meta.authorText().contains(value, Qt::CaseInsensitive);
        else if (word.contains(':') && key == "title") found = meta.title.contains(value, Qt::CaseInsensitive);
        else if (word.contains(':') && key == "series") found = meta.series.contains(value, Qt::CaseInsensitive);
        else if (word.contains(':') && key == "tag") found = meta.tags.join("\n").contains(value, Qt::CaseInsensitive);
        else if (word.contains(':') && key == "format") found = formats.contains(value.toLower());
        else if (word.contains(':') && key == "lang") found = meta.language.startsWith(value, Qt::CaseInsensitive);
        else if (word.contains(':') && key == "status")
            found = value.toLower() == "unread" ? status() != "read" && status() != "reading" : status() == value.toLower();
        else if (word.contains(':') && key == "is")
            found = value.toLower() == "digital" ? !formats.isEmpty() : value.toLower() == "paper" ? paper
                    : value.toLower() == "wishlist" && held() == "none";
        else if (word.contains(':') && key == "rating") found = rating >= value.toInt() && rating > 0;
        else found = everything.contains(word, Qt::CaseInsensitive);
        if (!found)
            return false;
    }
    return true;
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

QString Library::databasePath() const { return m_root + "/omabook.db"; }

QString Library::backupFolder() const { return m_root + "/.backups"; }

int Library::formatVersion() const {
    QSqlQuery version(QSqlDatabase::database(m_connection));
    return version.exec("PRAGMA user_version") && version.next() ? version.value(0).toInt() : 0;
}

bool Library::backUp(int schema, QString *error) const {
    const QString folder = backupFolder();
    const QString file = QString("%1/omabook-format-%2-%3.db")
                             .arg(folder).arg(schema).arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    if (!QDir().mkpath(folder)) {
        if (error)
            *error = folder + " can't be made.";
        return false;
    }
    // SQLite writes the copy itself, whole and consistent, also while the
    // write-ahead log holds changes the main file doesn't have yet.
    QSqlQuery copy(QSqlDatabase::database(m_connection));
    QFile::remove(file);
    if (!copy.exec(QString("VACUUM INTO '%1'").arg(QString(file).replace('\'', "''")))) {
        if (error)
            *error = copy.lastError().text();
        return false;
    }
    // The last five are kept; older ones go.
    const QFileInfoList kept = QDir(folder).entryInfoList({"omabook-format-*.db"}, QDir::Files, QDir::Time);
    for (const QFileInfo &old : kept.mid(5))
        QFile::remove(old.filePath());
    return true;
}

bool Library::exec(const QString &sql, const QVariantList &values) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare(sql);
    for (const QVariant &value : values)
        query.addBindValue(bindable(value));
    return query.exec();
}

bool Library::open(QString *error) {
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
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
    static const QStringList remote{"fuse.sshfs", "sshfs", "nfs", "nfs4", "cifs", "smb3", "smbfs", "9p", "fuse.rclone"};
    const bool local = !remote.contains(QString::fromLatin1(QStorageInfo(m_root).fileSystemType()));
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
    };

    QSqlQuery version(database);
    version.exec("PRAGMA user_version");
    const int schema = version.next() ? version.value(0).toInt() : 0;
    // A statement left open would keep a migration from dropping a table.
    version.finish();
    if (schema > migrations.size())
        return fail(QString("This library was last opened by a newer Omabook (its format is %1; this Omabook knows up to %2). "
                            "Update Omabook on this computer, or restore a copy from before that from %3.")
                        .arg(schema).arg(migrations.size()).arg(backupFolder()));
    // A library about to be changed to a newer format is copied first, so a
    // step that goes wrong, or a wish to go back to an older Omabook, never
    // costs the library. One that can't be copied is left as it is.
    if (schema > 0 && schema < migrations.size()) {
        QString why;
        if (!backUp(schema, &why))
            return fail("The library needs bringing up to date, but a copy of it couldn't be kept first, so it was left alone: " + why);
    }
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
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT path FROM books WHERE id = ?");
    query.addBindValue(id);
    return query.exec() && query.next() ? m_root + "/" + query.value(0).toString() : QString();
}

Library::Added Library::add(const QString &file, const std::optional<BookMeta> &known) {
    Added added;
    const QFileInfo info(file);
    if (!info.isFile() || !info.isReadable()) {
        added.error = "There is no readable file at " + file;
        return added;
    }
    if (!Formats::isBook(file)) {
        added.error = info.fileName() + " is not a book format Omabook knows.";
        return added;
    }
    const QString hash = hashFile(file);
    QSqlDatabase database = QSqlDatabase::database(m_connection);
    if (const qint64 existing = bookWithHash(hash)) {
        added.id = existing;
        added.duplicate = true;
        return added;
    }
    // Added on purpose, so no longer something that was thrown away.
    exec("DELETE FROM removed WHERE hash = ?", {hash});

    BookMeta meta = known ? *known : Formats::readMeta(file);
    if (meta.title.isEmpty())
        meta.title = Formats::metaFromFileName(file).title;
    database.transaction();
    QSqlQuery insert(database);
    insert.prepare("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                   "description, tags, added) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const QVariant &value : QVariantList{meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher,
                                              meta.language, meta.isbn, meta.published, meta.description,
                                              meta.tags.join(", "), QDateTime::currentSecsSinceEpoch()})
        insert.addBindValue(bindable(value));
    if (!insert.exec()) {
        database.rollback();
        added.error = "The library can't be written: " + insert.lastError().text();
        return added;
    }
    const qint64 id = insert.lastInsertId().toLongLong();
    const QString folder = folderFor(meta, id);
    exec("UPDATE books SET path = ? WHERE id = ?", {folder, id});
    database.commit();

    QString error;
    if (!addFormat(id, file, &error)) {
        exec("DELETE FROM books WHERE id = ?", {id});
        QDir(m_root + "/" + folder).removeRecursively();
        added.error = error;
        return added;
    }
    if (!meta.cover.isNull())
        setCover(id, meta.cover);
    added.id = id;
    return added;
}

bool Library::addFormat(qint64 id, const QString &file, QString *error) {
    auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
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
    Added added;
    if (meta.title.trimmed().isEmpty()) {
        added.error = "A book needs a title.";
        return added;
    }
    QSqlDatabase database = QSqlDatabase::database(m_connection);
    QSqlQuery insert(database);
    insert.prepare("INSERT INTO books (title, authors, series, series_index, publisher, language, isbn, published, "
                   "description, tags, added, paper) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    for (const QVariant &value : QVariantList{meta.title, meta.authorText(), meta.series, meta.seriesIndex, meta.publisher,
                                              meta.language, meta.isbn, meta.published, meta.description,
                                              meta.tags.join(", "), QDateTime::currentSecsSinceEpoch(), paper})
        insert.addBindValue(bindable(value));
    if (!insert.exec()) {
        added.error = "The library can't be written: " + insert.lastError().text();
        return added;
    }
    added.id = insert.lastInsertId().toLongLong();
    // The folder is made when there is something to put in it: a cover, or
    // a file the book is given later.
    exec("UPDATE books SET path = ? WHERE id = ?", {folderFor(meta, added.id), added.id});
    if (!meta.cover.isNull())
        setCover(added.id, meta.cover);
    return added;
}

bool Library::setReading(qint64 id, const QString &shelf, int rating, const QString &finished) {
    static const QStringList shelves{"", "to-read", "reading", "read"};
    return shelves.contains(shelf)
           && exec("UPDATE books SET shelf = ?, rating = ?, finished = ? WHERE id = ?", {shelf, qBound(0, rating, 5), finished, id});
}

bool Library::setPaper(qint64 id, bool paper) { return exec("UPDATE books SET paper = ? WHERE id = ?", {paper, id}); }

bool Library::setIdentifier(qint64 id, const QString &scheme, const QString &value) {
    const auto found = book(id);
    if (!found)
        return false;
    QJsonObject identifiers = found->identifiers;
    value.isEmpty() ? identifiers.remove(scheme) : void(identifiers.insert(scheme, value));
    return exec("UPDATE books SET identifiers = ? WHERE id = ?",
                {QString::fromUtf8(QJsonDocument(identifiers).toJson(QJsonDocument::Compact)), id});
}

qint64 Library::bookWithIdentifier(const QString &scheme, const QString &value) const {
    for (const Book &book : books())
        if (!value.isEmpty() && book.identifiers.value(scheme).toString() == value)
            return book.id;
    return 0;
}

bool Library::remove(qint64 id, QString *error) {
    const QString folder = bookFolder(id);
    if (folder.isEmpty()) {
        if (error)
            *error = QString("There is no book %1.").arg(id);
        return false;
    }
    for (const FormatFile &format : formatFiles(id))
        exec("INSERT OR REPLACE INTO removed (hash, removed_at) VALUES (?, ?)", {format.hash, QDateTime::currentSecsSinceEpoch()});
    // To the trash, where a mistake can be undone; a filesystem with no
    // trash gets the folder removed, since the user asked for it to go.
    if (QFileInfo::exists(folder) && !QFile::moveToTrash(folder) && !QDir(folder).removeRecursively()) {
        if (error)
            *error = "The book's folder can't be removed: " + folder;
        return false;
    }
    QDir().rmdir(QFileInfo(folder).absolutePath()); // the author's folder, when it is now empty
    return exec("DELETE FROM books WHERE id = ?", {id});
}

QList<Book> Library::books(const QString &query) const {
    QSqlDatabase database = QSqlDatabase::database(m_connection);
    QHash<qint64, QStringList> formats;
    QSqlQuery formatQuery(database);
    formatQuery.exec("SELECT book_id, format FROM formats");
    while (formatQuery.next())
        formats[formatQuery.value(0).toLongLong()] << formatQuery.value(1).toString();

    QList<Book> books;
    QSqlQuery bookQuery(database);
    bookQuery.exec(QString(selectBooks) + " ORDER BY books.id");
    while (bookQuery.next()) {
        Book book = bookFrom(bookQuery);
        book.formats = Formats::byPreference(formats.value(book.id));
        if (query.isEmpty() || book.matches(query))
            books << book;
    }
    return books;
}

std::optional<Book> Library::book(qint64 id) const {
    QSqlDatabase database = QSqlDatabase::database(m_connection);
    QSqlQuery query(database);
    query.prepare(QString(selectBooks) + " WHERE books.id = ?");
    query.addBindValue(id);
    if (!query.exec() || !query.next())
        return std::nullopt;
    Book book = bookFrom(query);
    QSqlQuery formatQuery(database);
    formatQuery.prepare("SELECT format FROM formats WHERE book_id = ?");
    formatQuery.addBindValue(id);
    formatQuery.exec();
    while (formatQuery.next())
        book.formats << formatQuery.value(0).toString();
    book.formats = Formats::byPreference(book.formats);
    return book;
}

bool Library::setMeta(qint64 id, const BookMeta &meta, QString *error) {
    const auto before = book(id);
    if (!before) {
        if (error)
            *error = QString("There is no book %1.").arg(id);
        return false;
    }
    if (!exec("UPDATE books SET title = ?, authors = ?, series = ?, series_index = ?, publisher = ?, language = ?, "
              "isbn = ?, published = ?, description = ?, tags = ? WHERE id = ?",
              {meta.title.isEmpty() ? before->meta.title : meta.title, meta.authorText(), meta.series, meta.seriesIndex,
               meta.publisher, meta.language, meta.isbn, meta.published, meta.description, meta.tags.join(", "), id})) {
        if (error)
            *error = "The library can't be written.";
        return false;
    }
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
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT format, file FROM formats WHERE book_id = ?");
    query.addBindValue(id);
    query.exec();
    QHash<QString, QString> files;
    while (query.next())
        files.insert(query.value(0).toString(), query.value(1).toString());
    const QString chosen = format.isEmpty() ? Formats::byPreference(files.keys()).value(0) : format.toLower();
    return files.contains(chosen) ? bookFolder(id) + "/" + files.value(chosen) : QString();
}

QStringList Library::removedHashes() const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.exec("SELECT hash FROM removed");
    QStringList hashes;
    while (query.next())
        hashes << query.value(0).toString();
    return hashes;
}

Progress Library::progress(qint64 id) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT section, fraction, percent, updated FROM progress WHERE book_id = ?");
    query.addBindValue(id);
    Progress progress;
    if (query.exec() && query.next()) {
        progress.section = query.value(0).toInt();
        progress.fraction = query.value(1).toDouble();
        progress.percent = query.value(2).toDouble();
        progress.updated = query.value(3).toLongLong();
    }
    return progress;
}

void Library::setProgress(qint64 id, const Progress &progress) {
    exec("INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, ?, ?, ?)",
         {id, progress.section, progress.fraction, progress.percent,
          progress.updated > 0 ? progress.updated : QDateTime::currentSecsSinceEpoch()});
}

QList<FormatFile> Library::formatFiles(qint64 id) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT format, file, hash, size FROM formats WHERE book_id = ?");
    query.addBindValue(id);
    query.exec();
    const QString folder = bookFolder(id);
    QList<FormatFile> files;
    while (query.next())
        files.append({query.value(0).toString(), folder + "/" + query.value(1).toString(), query.value(2).toString(),
                      query.value(3).toLongLong()});
    return files;
}

qint64 Library::bookWithHash(const QString &hash) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT book_id FROM formats WHERE hash = ?");
    query.addBindValue(hash);
    return query.exec() && query.next() ? query.value(0).toLongLong() : 0;
}

bool Library::wasRemoved(const QString &hash) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT 1 FROM removed WHERE hash = ?");
    query.addBindValue(hash);
    return query.exec() && query.next();
}

Recap Library::notes(qint64 id, const QString &kind) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT section, fraction, text, updated FROM notes WHERE book_id = ? AND kind = ?");
    query.addBindValue(id);
    query.addBindValue(kind);
    Recap notes;
    if (query.exec() && query.next())
        notes = {query.value(0).toInt(), query.value(1).toDouble(), query.value(2).toString(), query.value(3).toLongLong()};
    return notes;
}

QList<Book> Library::earlierInSeries(qint64 id) const {
    const std::optional<Book> current = book(id);
    const QString series = current ? current->meta.series.trimmed() : QString();
    QList<Book> earlier;
    if (series.isEmpty())
        return earlier;
    for (const Book &other : books())
        if (other.id != id && other.meta.seriesIndex < current->meta.seriesIndex
            && other.meta.series.trimmed().compare(series, Qt::CaseInsensitive) == 0)
            earlier.append(other);
    std::sort(earlier.begin(), earlier.end(),
              [](const Book &a, const Book &b) { return a.meta.seriesIndex < b.meta.seriesIndex; });
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
            found.append({other.meta.title, text, blurb});
    }
    return found;
}

void Library::setNotes(qint64 id, const QString &kind, const Recap &notes) {
    exec("INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) VALUES (?, ?, ?, ?, ?, ?)",
         {id, kind, notes.section, notes.fraction, notes.text, QDateTime::currentSecsSinceEpoch()});
}

bool Library::merge(qint64 keep, qint64 other, QString *error) {
    const auto kept = book(keep), folded = book(other);
    if (!kept || !folded || keep == other) {
        if (error)
            *error = "There are no two such books to merge.";
        return false;
    }
    for (const FormatFile &file : formatFiles(other))
        if (!kept->formats.contains(file.format) && !addFormat(keep, file.path, error))
            return false;
    if (!kept->hasCover && folded->hasCover)
        setCover(keep, QImage(coverPath(other)));
    // The reading place that is further along is the one that was read.
    if (progress(other).percent > progress(keep).percent)
        setProgress(keep, progress(other));
    return remove(other, error);
}

qint64 Library::addAnnotation(qint64 bookId, const Annotation &annotation) {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("INSERT INTO annotations (book_id, kind, section, fraction, text, note, created) VALUES (?, ?, ?, ?, ?, ?, ?)");
    for (const QVariant &value : QVariantList{bookId, annotation.kind, annotation.section, annotation.fraction,
                                              annotation.text, annotation.note, QDateTime::currentSecsSinceEpoch()})
        query.addBindValue(bindable(value));
    return query.exec() ? query.lastInsertId().toLongLong() : 0;
}

void Library::removeAnnotation(qint64 annotationId) { exec("DELETE FROM annotations WHERE id = ?", {annotationId}); }

QList<Annotation> Library::annotations(qint64 bookId) const {
    QSqlQuery query(QSqlDatabase::database(m_connection));
    query.prepare("SELECT id, kind, section, fraction, text, note, created FROM annotations WHERE book_id = ? "
                  "ORDER BY section, fraction");
    query.addBindValue(bookId);
    query.exec();
    QList<Annotation> annotations;
    while (query.next())
        annotations.append({query.value(0).toLongLong(), query.value(1).toString(), query.value(2).toInt(),
                            query.value(3).toDouble(), query.value(4).toString(), query.value(5).toString(),
                            query.value(6).toLongLong()});
    return annotations;
}
