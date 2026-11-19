#include "librarymigrations.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <ranges>

#include "library.h"
#include "log.h"

namespace {

// Each entry takes the database from the version of its index to the next.
// NOLINTBEGIN(bugprone-suspicious-missing-comma,bugprone-throwing-static-initialization,cert-err58-cpp): constant table of multi-line SQL
// migration statements.
const QList<QStringList> migrations{
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
     "INSERT INTO notes SELECT book_id, 'recap', section, fraction, text, updated FROM recaps", "DROP TABLE recaps"},
    // Books that are not files: paper ones and ones only known of, with
    // what is said of their reading and what other sites call them.
    {"ALTER TABLE books ADD COLUMN paper INTEGER NOT NULL DEFAULT 0", "ALTER TABLE books ADD COLUMN shelf TEXT NOT NULL DEFAULT ''",
     "ALTER TABLE books ADD COLUMN rating INTEGER NOT NULL DEFAULT 0", "ALTER TABLE books ADD COLUMN finished TEXT NOT NULL DEFAULT ''",
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
     "CREATE TRIGGER formats_insert_counted AFTER INSERT ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; "
     "END",
     "CREATE TRIGGER formats_update_counted AFTER UPDATE ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; "
     "END",
     "CREATE TRIGGER formats_delete_counted AFTER DELETE ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = 'books'; "
     "END",
     "CREATE TRIGGER progress_insert_counted AFTER INSERT ON progress BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'books'; END",
     "CREATE TRIGGER progress_update_counted AFTER UPDATE ON progress BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'books'; END",
     "CREATE TRIGGER progress_delete_counted AFTER DELETE ON progress BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'books'; END",
     "CREATE TRIGGER graph_nodes_insert_counted AFTER INSERT ON graph_nodes BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_nodes_update_counted AFTER UPDATE ON graph_nodes BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_nodes_delete_counted AFTER DELETE ON graph_nodes BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_edges_insert_counted AFTER INSERT ON graph_edges BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_edges_update_counted AFTER UPDATE ON graph_edges BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_edges_delete_counted AFTER DELETE ON graph_edges BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_seen_insert_counted AFTER INSERT ON graph_seen BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_seen_update_counted AFTER UPDATE ON graph_seen BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_seen_delete_counted AFTER DELETE ON graph_seen BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END"},
    // How a book is set on the page where it isn't as the defaults are
    // (readingformat.h): only the options it has of its own, as a JSON
    // object. Counted apart, so the reader follows a change made
    // elsewhere without the shelf being read again.
    {"CREATE TABLE book_format (book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
     "options TEXT NOT NULL DEFAULT '{}')",
     "INSERT INTO library_state (key, value) VALUES ('format', 0)",
     "CREATE TRIGGER book_format_insert_counted AFTER INSERT ON book_format BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'format'; END",
     "CREATE TRIGGER book_format_update_counted AFTER UPDATE ON book_format BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'format'; END",
     "CREATE TRIGGER book_format_delete_counted AFTER DELETE ON book_format BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'format'; END"},
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
     "CREATE INDEX graph_facts_node ON graph_facts(node_id)", "ALTER TABLE graph_edges ADD COLUMN anchor TEXT NOT NULL DEFAULT ''",
     "ALTER TABLE graph_seen ADD COLUMN anchor TEXT NOT NULL DEFAULT ''",
     "INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction) SELECT n.id, 'summary', n.summary, "
     "s.book_id, s.section, s.fraction FROM graph_nodes AS n JOIN graph_seen AS s ON s.node_id = n.id WHERE n.summary <> ''",
     "INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction) SELECT n.id, 'alias', a.value, "
     "s.book_id, s.section, s.fraction FROM graph_nodes AS n JOIN graph_seen AS s ON s.node_id = n.id, "
     "json_each(CASE WHEN json_valid(n.data) THEN n.data ELSE '{}' END, '$.aliases') AS a WHERE a.type = 'text'",
     "CREATE TRIGGER graph_facts_insert_counted AFTER INSERT ON graph_facts BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_facts_update_counted AFTER UPDATE ON graph_facts BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "CREATE TRIGGER graph_facts_delete_counted AFTER DELETE ON graph_facts BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
     "'graph'; END",
     "INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) "
     "SELECT book_id, 'furthest', section, fraction, '', updated FROM progress",
     "CREATE TRIGGER progress_furthest_insert AFTER INSERT ON progress BEGIN "
     "INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) "
     "SELECT NEW.book_id, 'furthest', NEW.section, NEW.fraction, '', NEW.updated WHERE NOT EXISTS (SELECT 1 FROM notes "
     "WHERE book_id = NEW.book_id AND kind = 'furthest' AND (section > NEW.section OR (section = NEW.section AND fraction >= "
     "NEW.fraction))); END",
     "CREATE TRIGGER progress_furthest_update AFTER UPDATE ON progress BEGIN "
     "INSERT OR REPLACE INTO notes (book_id, kind, section, fraction, text, updated) "
     "SELECT NEW.book_id, 'furthest', NEW.section, NEW.fraction, '', NEW.updated WHERE NOT EXISTS (SELECT 1 FROM notes "
     "WHERE book_id = NEW.book_id AND kind = 'furthest' AND (section > NEW.section OR (section = NEW.section AND fraction >= "
     "NEW.fraction))); END"},
    // Collections (library.h): a name, a rule (a search), the books put
    // in or taken out by hand, how the collection is shown and set on
    // the page, and the collection it is inside (0 for the top).
    {"CREATE TABLE collections (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT NOT NULL, rule TEXT NOT NULL DEFAULT '', "
     "position INTEGER NOT NULL DEFAULT 0, shelf TEXT NOT NULL DEFAULT '{}', format TEXT NOT NULL DEFAULT '{}', "
     "updated INTEGER NOT NULL, removed INTEGER NOT NULL DEFAULT 0, parent INTEGER NOT NULL DEFAULT 0)",
     "CREATE UNIQUE INDEX collections_named ON collections(parent, name COLLATE NOCASE)",
     "CREATE TABLE collection_books (collection_id INTEGER NOT NULL REFERENCES collections(id) ON DELETE CASCADE, "
     "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, included INTEGER NOT NULL, "
     "PRIMARY KEY (collection_id, book_id))",
     "CREATE INDEX collection_books_book ON collection_books(book_id)",
     "CREATE TRIGGER collections_insert_counted AFTER INSERT ON collections BEGIN UPDATE library_state SET value = value + 1 WHERE key IN "
     "('books', 'format'); END",
     "CREATE TRIGGER collections_update_counted AFTER UPDATE ON collections BEGIN UPDATE library_state SET value = value + 1 WHERE key IN "
     "('books', 'format'); END",
     "CREATE TRIGGER collections_delete_counted AFTER DELETE ON collections BEGIN UPDATE library_state SET value = value + 1 WHERE key IN "
     "('books', 'format'); END",
     "CREATE TRIGGER collection_books_insert_counted AFTER INSERT ON collection_books BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key IN ('books', 'format'); END",
     "CREATE TRIGGER collection_books_update_counted AFTER UPDATE ON collection_books BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key IN ('books', 'format'); END",
     "CREATE TRIGGER collection_books_delete_counted AFTER DELETE ON collection_books BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key IN ('books', 'format'); END"},
    // When a book's details last changed, and what is said of its
    // reading, so a sync with another computer knows which side's are
    // newer, and whether a book changed after the other side removed it.
    {"ALTER TABLE books ADD COLUMN details_at INTEGER NOT NULL DEFAULT 0",
     "ALTER TABLE books ADD COLUMN reading_at INTEGER NOT NULL DEFAULT 0"},
    // Owned spaces, global ids and the replicated change log (ADR 0001).
    {"CREATE TABLE IF NOT EXISTS spaces (id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT NOT NULL DEFAULT '', "
     "owner TEXT NOT NULL DEFAULT '', key TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL DEFAULT 0)",
     "CREATE TABLE IF NOT EXISTS space_members (space TEXT NOT NULL REFERENCES spaces(id) ON DELETE CASCADE, "
     "person TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'writer', name TEXT NOT NULL DEFAULT '', "
     "added INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (space, person))",
     "CREATE TABLE IF NOT EXISTS log_ids (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)",
     "CREATE TABLE IF NOT EXISTS ops (space INTEGER NOT NULL REFERENCES log_ids(id), device INTEGER NOT NULL REFERENCES log_ids(id), "
     "seq INTEGER NOT NULL, hlc INTEGER NOT NULL, author INTEGER NOT NULL REFERENCES log_ids(id), entity TEXT NOT NULL, "
     "id INTEGER NOT NULL REFERENCES log_ids(id), field TEXT NOT NULL DEFAULT '', value TEXT NOT NULL DEFAULT '', "
     "PRIMARY KEY (space, device, seq)) WITHOUT ROWID",
     "CREATE INDEX IF NOT EXISTS ops_entity ON ops(entity, id, field)",
     "CREATE TABLE IF NOT EXISTS seen (space INTEGER NOT NULL REFERENCES log_ids(id), device INTEGER NOT NULL REFERENCES log_ids(id), "
     "seq INTEGER NOT NULL DEFAULT 0, hlc INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (space, device)) WITHOUT ROWID",
     "CREATE TABLE IF NOT EXISTS global_ids (entity TEXT NOT NULL, local_id INTEGER NOT NULL, global_id TEXT NOT NULL, "
     "PRIMARY KEY (entity, local_id))",
     "CREATE UNIQUE INDEX IF NOT EXISTS global_ids_gid ON global_ids(entity, global_id)",
     // Which rows a write touched, so the log takes in just those (ChangeLog::afterWrite)
     // instead of comparing the whole library after every write.
     "CREATE TABLE IF NOT EXISTS log_dirty (entity TEXT NOT NULL, local_id INTEGER NOT NULL, PRIMARY KEY (entity, local_id)) "
     "WITHOUT ROWID",
     "CREATE TRIGGER IF NOT EXISTS books_insert_dirty AFTER INSERT ON books BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS books_update_dirty AFTER UPDATE ON books BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS books_delete_dirty AFTER DELETE ON books BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', OLD.id); END",
     "CREATE TRIGGER IF NOT EXISTS formats_insert_dirty AFTER INSERT ON formats BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', NEW.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS formats_update_dirty AFTER UPDATE ON formats BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', NEW.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS formats_delete_dirty AFTER DELETE ON formats BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', OLD.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS progress_insert_dirty AFTER INSERT ON progress BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', NEW.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS progress_update_dirty AFTER UPDATE ON progress BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', NEW.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS progress_delete_dirty AFTER DELETE ON progress BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('book', OLD.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS removed_insert_dirty AFTER INSERT ON removed BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('removal', 0); END",
     "CREATE TRIGGER IF NOT EXISTS removed_update_dirty AFTER UPDATE ON removed BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('removal', 0); END",
     "CREATE TRIGGER IF NOT EXISTS removed_delete_dirty AFTER DELETE ON removed BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('removal', 0); END",
     "CREATE TRIGGER IF NOT EXISTS collections_insert_dirty AFTER INSERT ON collections BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('collection', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS collections_update_dirty AFTER UPDATE ON collections BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('collection', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS collections_delete_dirty AFTER DELETE ON collections BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('collection', OLD.id); END",
     "CREATE TRIGGER IF NOT EXISTS collection_books_insert_dirty AFTER INSERT ON collection_books BEGIN INSERT OR IGNORE INTO log_dirty "
     "VALUES "
     "('collection', NEW.collection_id); END",
     "CREATE TRIGGER IF NOT EXISTS collection_books_update_dirty AFTER UPDATE ON collection_books BEGIN INSERT OR IGNORE INTO log_dirty "
     "VALUES "
     "('collection', NEW.collection_id); END",
     "CREATE TRIGGER IF NOT EXISTS collection_books_delete_dirty AFTER DELETE ON collection_books BEGIN INSERT OR IGNORE INTO log_dirty "
     "VALUES "
     "('collection', OLD.collection_id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_nodes_insert_dirty AFTER INSERT ON graph_nodes BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('node', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_nodes_update_dirty AFTER UPDATE ON graph_nodes BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('node', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_nodes_delete_dirty AFTER DELETE ON graph_nodes BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('node', OLD.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_seen_insert_dirty AFTER INSERT ON graph_seen BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('node', NEW.node_id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_seen_update_dirty AFTER UPDATE ON graph_seen BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('node', NEW.node_id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_seen_delete_dirty AFTER DELETE ON graph_seen BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('node', OLD.node_id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_facts_insert_dirty AFTER INSERT ON graph_facts BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('fact', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_facts_update_dirty AFTER UPDATE ON graph_facts BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('fact', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_facts_delete_dirty AFTER DELETE ON graph_facts BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('fact', OLD.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_edges_insert_dirty AFTER INSERT ON graph_edges BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('edge', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_edges_update_dirty AFTER UPDATE ON graph_edges BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('edge', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS graph_edges_delete_dirty AFTER DELETE ON graph_edges BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('edge', OLD.id); END",
     "CREATE TRIGGER IF NOT EXISTS reading_log_insert_dirty AFTER INSERT ON reading_log BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('reading_log', NEW.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS reading_log_update_dirty AFTER UPDATE ON reading_log BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('reading_log', NEW.book_id); END",
     "CREATE TRIGGER IF NOT EXISTS reading_log_delete_dirty AFTER DELETE ON reading_log BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('reading_log', OLD.book_id); END",
     "INSERT OR IGNORE INTO library_state (key, value) VALUES ('hlc', 0), ('logged_books', -1), ('logged_graph', -1), ('logged_format', "
     "-1)",
     "CREATE TRIGGER IF NOT EXISTS removed_insert_counted AFTER INSERT ON removed BEGIN UPDATE library_state SET value = value + 1 WHERE "
     "key = 'books'; END",
     "CREATE TRIGGER IF NOT EXISTS removed_update_counted AFTER UPDATE ON removed BEGIN UPDATE library_state SET value = value + 1 WHERE "
     "key = 'books'; END",
     "CREATE TRIGGER IF NOT EXISTS removed_delete_counted AFTER DELETE ON removed BEGIN UPDATE library_state SET value = value + 1 WHERE "
     "key = 'books'; END",
     "CREATE TRIGGER IF NOT EXISTS reading_log_insert_counted AFTER INSERT ON reading_log BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key = 'graph'; END",
     "CREATE TRIGGER IF NOT EXISTS reading_log_update_counted AFTER UPDATE ON reading_log BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key = 'graph'; END",
     "CREATE TRIGGER IF NOT EXISTS reading_log_delete_counted AFTER DELETE ON reading_log BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key = 'graph'; END"},
};
// NOLINTEND(bugprone-suspicious-missing-comma,bugprone-throwing-static-initialization,cert-err58-cpp)

} // namespace

int LibraryMigrations::currentFormat() noexcept { return static_cast<int>(migrations.size()); }

std::expected<void, QString> Library::backUp(int schema) const {
    const QString folder = backupFolder();
    const QString file =
        QString("%1/omabook-format-%2-%3.db").arg(folder).arg(schema).arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
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

std::expected<void, QString> LibraryMigrations::migrate(const Library &library) {
    // Asked and done with before anything changes: a statement left open
    // would keep a migration from dropping a table.
    const int schema = library.formatVersion();
    if (library.m_keepFormat && schema > 0 && schema != migrations.size())
        return std::unexpected(QString("This library is in format %1, and is opened here only as it is (this Omabook's is %2).")
                                   .arg(schema)
                                   .arg(migrations.size()));
    if (schema > migrations.size())
        return std::unexpected(QString("This library was last opened by a newer Omabook (its format is %1; this Omabook knows up to %2). "
                                       "Update Omabook on this computer, or restore a copy from before that from %3.")
                                   .arg(schema)
                                   .arg(migrations.size())
                                   .arg(library.backupFolder()));
    // A library about to be changed to a newer format is copied first, so a
    // step that goes wrong, or a wish to go back to an older Omabook, never
    // costs the library. One that can't be copied is left as it is.
    if (schema > 0 && schema < migrations.size()) {
        if (const auto kept = library.backUp(schema); !kept)
            return std::unexpected("The library needs bringing up to date, but a copy of it couldn't be kept first, so it was left alone: "
                                   + kept.error());
    }
    if (schema < migrations.size())
        qCInfo(lcLibrary).noquote() << "bringing" << library.m_root << "from format" << schema << "to" << migrations.size();
    QSqlDatabase database = QSqlDatabase::database(library.m_connection);
    for (int step = schema; step < migrations.size(); ++step) {
        database.transaction();
        for (const QString &statement : migrations.at(step) + QStringList{QString("PRAGMA user_version = %1").arg(step + 1)}) {
            if (!library.exec(statement)) {
                database.rollback();
                return std::unexpected("The library can't be set up: " + database.lastError().text());
            }
        }
        database.commit();
    }
    // A library that took the collections' step in its first form, before
    // collections nested, is given what nesting needs.
    QSqlQuery columns = library.query("SELECT 1 FROM pragma_table_info('collections') WHERE name = 'parent'");
    const bool nested = columns.next() || library.formatVersion() < migrations.size();
    columns.finish();
    if (!nested) {
        library.exec("ALTER TABLE collections ADD COLUMN parent INTEGER NOT NULL DEFAULT 0");
        library.exec("DROP INDEX IF EXISTS collections_named");
        library.exec("CREATE UNIQUE INDEX collections_named ON collections(parent, name COLLATE NOCASE)");
    }
    return {};
}

Library::Changes Library::changes() const {
    Changes changes;
    for (QSqlQuery query = this->query("SELECT key, value FROM library_state WHERE key IN ('books', 'graph', 'format')"); query.next();) {
        const QString key = query.value(0).toString();
        if (key == "books")
            changes.books = query.value(1).toLongLong();
        else if (key == "graph")
            changes.graph = query.value(1).toLongLong();
        else
            changes.format = query.value(1).toLongLong();
    }
    return changes;
}
