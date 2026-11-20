#include "libraryschema.h"

#include "annotationschema.h"
#include "graphindex.h"
#include "tenancyschema.h"

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
    // Compaction (ADR 0001): per space, a snapshot of the operations that still
    // matter, up to a mark per device, and what reads the log reads that and
    // the operations after the mark (live_ops), and the operations that are
    // in neither and that every member has seen are dropped from `ops`.
    {"CREATE TABLE IF NOT EXISTS snapshot_ops (space INTEGER NOT NULL, device INTEGER NOT NULL, seq INTEGER NOT NULL, "
     "hlc INTEGER NOT NULL, author INTEGER NOT NULL, entity TEXT NOT NULL, id INTEGER NOT NULL, field TEXT NOT NULL DEFAULT '', "
     "value TEXT NOT NULL DEFAULT '', PRIMARY KEY (space, device, seq)) WITHOUT ROWID",
     "CREATE TABLE IF NOT EXISTS snapshot_marks (space INTEGER NOT NULL, device INTEGER NOT NULL, seq INTEGER NOT NULL, "
     "PRIMARY KEY (space, device)) WITHOUT ROWID",
     // What each member of a space held of it when it last synced: what
     // compaction may drop must be what every member has seen.
     "CREATE TABLE IF NOT EXISTS member_vectors (space INTEGER NOT NULL, member TEXT NOT NULL, device INTEGER NOT NULL, "
     "seq INTEGER NOT NULL, PRIMARY KEY (space, member, device)) WITHOUT ROWID",
     "CREATE VIEW IF NOT EXISTS live_ops AS SELECT space, device, seq, hlc, author, entity, id, field, value FROM snapshot_ops "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0)"},
    // The snapshot takes the place of the operations it covers (#2), so what
    // looks up one thing's operations finds them there as it does in `ops`.
    {"CREATE INDEX IF NOT EXISTS snapshot_ops_entity ON snapshot_ops(entity, id, field)"},
    // Signed operations (ADR 0001, People and keys): who signed each, the
    // person who wrote it, and the signature of its canonical bytes; neither
    // for one written before. A snapshot keeps an operation as it was, its
    // signature with it, so what it holds can still be verified.
    {"ALTER TABLE ops ADD COLUMN signer INTEGER REFERENCES log_ids(id)", "ALTER TABLE ops ADD COLUMN signature BLOB",
     "ALTER TABLE snapshot_ops ADD COLUMN signer INTEGER", "ALTER TABLE snapshot_ops ADD COLUMN signature BLOB",
     "DROP VIEW IF EXISTS live_ops",
     "CREATE VIEW live_ops AS SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0)"},
    // A published space, one subscribed to (#4) or the owner's own copy, is a
    // layer beside the library and never its rows: what reads the log for the
    // rows (live_ops) leaves every "published:…" space out, by its id, and
    // what reads every space (compaction, replication, a layer) reads space_ops.
    {"CREATE VIEW IF NOT EXISTS space_ops AS "
     "SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0)",
     "DROP VIEW IF EXISTS live_ops",
     "CREATE VIEW live_ops AS SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "WHERE space NOT IN (SELECT id FROM log_ids WHERE name >= 'published:' AND name < 'published;') "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0) "
     "AND o.space NOT IN (SELECT id FROM log_ids WHERE name >= 'published:' AND name < 'published;')"},
    // A collection published as a space (#3): which collection, with which
    // parts, and the space's own log, numbered once and for good, so that one
    // who has its operations up to a number is sent only those after it.
    // Apart from `ops`, where compaction and replication would treat it as
    // a space its members sync (ADR 0001, *Compaction*, as built for #3).
    {"CREATE TABLE IF NOT EXISTS published (space TEXT PRIMARY KEY, collection TEXT NOT NULL, parts TEXT NOT NULL DEFAULT '')",
     "CREATE TABLE IF NOT EXISTS published_ops (space TEXT NOT NULL, device TEXT NOT NULL, seq INTEGER NOT NULL, hlc INTEGER NOT NULL, "
     "author TEXT NOT NULL, entity TEXT NOT NULL, id TEXT NOT NULL, field TEXT NOT NULL DEFAULT '', value TEXT NOT NULL DEFAULT '', "
     "PRIMARY KEY (space, device, seq)) WITHOUT ROWID"},
    // A shared space (#38) is a layer beside the library as a published one
    // is: its writers' operations never make the members' own rows.
    {"DROP VIEW IF EXISTS live_ops",
     "CREATE VIEW live_ops AS SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "WHERE space NOT IN (SELECT id FROM log_ids WHERE (name >= 'published:' AND name < 'published;') "
     "OR (name >= 'shared:' AND name < 'shared;')) "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0) "
     "AND o.space NOT IN (SELECT id FROM log_ids WHERE (name >= 'published:' AND name < 'published;') "
     "OR (name >= 'shared:' AND name < 'shared;'))"},
    // Where a space travels by a transport (#7): the place a space published
    // here was last sent to (a gist, updated in place when it is sent again),
    // and the place a subscription follows. This library's own, never sent.
    {"CREATE TABLE IF NOT EXISTS space_links (space TEXT NOT NULL, role TEXT NOT NULL, transport TEXT NOT NULL, "
     "address TEXT NOT NULL, PRIMARY KEY (space, role, transport)) WITHOUT ROWID"},
    // The snapshot kept in the order it is looked up in, by a thing's fields
    // (#54), in place of an index beside it, which came to a sixth of the
    // file once the baseline was written as a snapshot.
    {"DROP VIEW IF EXISTS live_ops", "DROP VIEW IF EXISTS space_ops",
     "CREATE TABLE snapshot_ops_keyed (space INTEGER NOT NULL, device INTEGER NOT NULL, seq INTEGER NOT NULL, hlc INTEGER NOT NULL, "
     "author INTEGER NOT NULL, entity TEXT NOT NULL, id INTEGER NOT NULL, field TEXT NOT NULL DEFAULT '', "
     "value TEXT NOT NULL DEFAULT '', signer INTEGER, signature BLOB, PRIMARY KEY (entity, id, field, space, device, seq)) WITHOUT ROWID",
     "INSERT INTO snapshot_ops_keyed SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops",
     "DROP TABLE snapshot_ops", "ALTER TABLE snapshot_ops_keyed RENAME TO snapshot_ops",
     "CREATE VIEW space_ops AS "
     "SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0)",
     "CREATE VIEW live_ops AS SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "WHERE space NOT IN (SELECT id FROM log_ids WHERE (name >= 'published:' AND name < 'published;') "
     "OR (name >= 'shared:' AND name < 'shared;')) "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0) "
     "AND o.space NOT IN (SELECT id FROM log_ids WHERE (name >= 'published:' AND name < 'published;') "
     "OR (name >= 'shared:' AND name < 'shared;'))"},
    // A plugin's worlds in spaces (#65): which plugin made each scope of the
    // graph here, and which of them a space was given, with the book each is
    // of. This computer's own, never sent: a space says what it was given.
    {"CREATE TABLE IF NOT EXISTS plugin_scopes (scope TEXT PRIMARY KEY, plugin TEXT NOT NULL) WITHOUT ROWID",
     "CREATE TABLE IF NOT EXISTS space_worlds (space TEXT NOT NULL, book TEXT NOT NULL, scope TEXT NOT NULL, plugin TEXT NOT NULL, "
     "file INTEGER NOT NULL DEFAULT 0, hlc INTEGER NOT NULL DEFAULT 0, PRIMARY KEY (space, book, scope)) WITHOUT ROWID"},
    // Bookmarks and highlights travel by the log (#66): which of them a write
    // touched, and a counter of their own, so a write that bypassed the log is
    // found on opening without the shelf reading every book again for it. The
    // log has none of them yet, so the next opening takes them all in.
    {"CREATE TRIGGER IF NOT EXISTS annotations_insert_dirty AFTER INSERT ON annotations BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('annotation', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS annotations_update_dirty AFTER UPDATE ON annotations BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('annotation', NEW.id); END",
     "CREATE TRIGGER IF NOT EXISTS annotations_delete_dirty AFTER DELETE ON annotations BEGIN INSERT OR IGNORE INTO log_dirty VALUES "
     "('annotation', OLD.id); END",
     "INSERT OR IGNORE INTO library_state (key, value) VALUES ('annotations', 0), ('logged_annotations', -1)",
     "CREATE TRIGGER IF NOT EXISTS annotations_insert_counted AFTER INSERT ON annotations BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key = 'annotations'; END",
     "CREATE TRIGGER IF NOT EXISTS annotations_update_counted AFTER UPDATE ON annotations BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key = 'annotations'; END",
     "CREATE TRIGGER IF NOT EXISTS annotations_delete_counted AFTER DELETE ON annotations BEGIN UPDATE library_state SET value = value + 1 "
     "WHERE key = 'annotations'; END"},
    // The snapshot looked up by space and device too (#103): what a peer
    // lacks of it, the last a device wrote of it, and one space's operations
    // are a range of it, not a read of all of it.
    {"CREATE INDEX IF NOT EXISTS snapshot_ops_space ON snapshot_ops(space, device, seq)"},
    // The tenants of the graph (ADR 0003, #94, format 25): which tenants see each thing,
    // relation and fact, kept by triggers as the rows are written. What they
    // are for the graph a library already has is filled in by Tenancy::follow
    // as the library is set up, after this step and the copy kept before it.
    TenancySchema::step(),
    // A book's sightings looked up by the book (#99, format 26): a book's
    // view, its spoiler cut and the graph palette's list of books read that
    // book's range of graph_seen, in the order they were met, not the whole
    // graph; so do the tenants a book gives the things met in it.
    {"CREATE INDEX IF NOT EXISTS graph_seen_book ON graph_seen(book_id, section, fraction)"},
    // A thing made by hand in a tenant stays in it (ADR 0003, #95, format 27):
    // the tenancy triggers again, by the amended rules, and the membership
    // placed again whole by them the next time the library is set up.
    TenancySchema::madeInTheirTenant(),
    // Each thing's degree in each of its tenants (#101, format 28), kept by
    // triggers, for the graph window's order: a tenant's most connected
    // things first, read as a range of an index rather than counted for
    // every thing each time.
    TenancySchema::degrees(),
    // A fact or a relation written by hand in a tenant stays in it, and a
    // relation's note is the one its tenant was told (ADR 0003, #109): the
    // columns that keep them (format 29), and the tenancy triggers again by
    // those rules, with the membership placed again whole (format 30).
    TenancySchema::writtenColumns(),
    TenancySchema::writtenInTheirTenant(),
    // The graph searched by its words, and followed by what changed (#102,
    // format 31): a full-text index of its things, and the things touched
    // since a count of the graph, both kept by triggers (GraphIndex).
    GraphIndex::step(),
    // A relation of no book to a thing every tenant sees is in the tenant of
    // its other end (#112, format 32): the tenancy triggers again by that
    // rule, and the membership placed again whole.
    TenancySchema::endsSeenFromEveryTenant(),
    // A plugin's data in the change log (ADR 0002, #76, format 33): each
    // plugin's "plugin:…" space is never the library's rows, as a published
    // or shared one isn't, and is read by itself (PluginData).
    {"DROP VIEW IF EXISTS live_ops",
     "CREATE VIEW live_ops AS SELECT space, device, seq, hlc, author, entity, id, field, value, signer, signature FROM snapshot_ops "
     "WHERE space NOT IN (SELECT id FROM log_ids WHERE (name >= 'published:' AND name < 'published;') "
     "OR (name >= 'shared:' AND name < 'shared;') OR (name >= 'plugin:' AND name < 'plugin;')) "
     "UNION ALL SELECT o.space, o.device, o.seq, o.hlc, o.author, o.entity, o.id, o.field, o.value, o.signer, o.signature FROM ops AS o "
     "LEFT JOIN snapshot_marks AS m ON m.space = o.space AND m.device = o.device WHERE o.seq > COALESCE(m.seq, 0) "
     "AND o.space NOT IN (SELECT id FROM log_ids WHERE (name >= 'published:' AND name < 'published;') "
     "OR (name >= 'shared:' AND name < 'shared;') OR (name >= 'plugin:' AND name < 'plugin;'))"},
    // A thing renamed by hand in a tenant is called so there alone (ADR 0003,
    // #110, format 34): the name a tenant gave it found by the word index
    // and by the name (GraphIndex).
    GraphIndex::names(),
    // Highlights and margin notes on a passage (#80, format 35): where it
    // ends, its words with a few before and after, the file its places were
    // found in, how sure that was, and whether it is lost. A highlight of an
    // older library is still a place and its words, found again by them once
    // its book is opened (AnnotationAnchors); a bookmark ends where it starts
    // (an end before the start is the start: Annotation::passage).
    AnnotationSchema::passages(),
};
// NOLINTEND(bugprone-suspicious-missing-comma,bugprone-throwing-static-initialization,cert-err58-cpp)

} // namespace

const QList<QStringList> &LibrarySchema::steps() { return migrations; }
