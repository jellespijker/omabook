#include "annotationschema.h"

#include <QString>
#include <utility>

namespace {

// The columns of a passage (#80), after the table's first ones.
constexpr auto passageColumns =
    "to_section INTEGER NOT NULL DEFAULT 0, to_fraction REAL NOT NULL DEFAULT 0, quote TEXT NOT NULL DEFAULT '', "
    "quote_before TEXT NOT NULL DEFAULT '', quote_after TEXT NOT NULL DEFAULT '', anchored TEXT NOT NULL "
    "DEFAULT '', confidence REAL NOT NULL DEFAULT 1, lost INTEGER NOT NULL DEFAULT 0";

// The annotations table made anew with `columns` after its first ones, its
// rows copied as `copied` lists them from the table as it was, as SQLite adds
// a column only once: so the step can be taken again (a library put back to
// the format before). Its rows keep their numbers, and the numbers it gave
// out stay given (sqlite_sequence), so the change log's ids for removed ones
// never come back; its change log's triggers are made again with it (those
// of its word index, from format 38, are wordTriggers()).
QStringList madeAnew(const QString &columns, const QString &copied, const QString &from) {
    const QString table = "CREATE TABLE annotations_next (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                          "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, kind TEXT NOT NULL, section INTEGER NOT NULL, "
                          "fraction REAL NOT NULL, text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, "
                          + columns + ")";
    const QString copy = "INSERT INTO annotations_next (" + copied + ") SELECT " + from + " FROM annotations";
    const QString counted = "INSERT INTO sqlite_sequence (name, seq) SELECT 'annotations_next', 0 "
                            "WHERE NOT EXISTS (SELECT 1 FROM sqlite_sequence WHERE name = 'annotations_next')";
    const QString given = "UPDATE sqlite_sequence SET seq = (SELECT MAX(seq) FROM sqlite_sequence WHERE name IN ('annotations', "
                          "'annotations_next')) WHERE name = 'annotations_next'";
    QStringList steps{QString("DROP TABLE IF EXISTS annotations_next"),
                      table,
                      copy,
                      counted,
                      given,
                      QString("DROP TABLE annotations"),
                      QString("ALTER TABLE annotations_next RENAME TO annotations"),
                      QString("CREATE INDEX IF NOT EXISTS annotations_book ON annotations(book_id, section, fraction)")};
    for (const auto &[when, row] : {std::pair{"insert", "NEW"}, std::pair{"update", "NEW"}, std::pair{"delete", "OLD"}}) {
        steps << QString("CREATE TRIGGER IF NOT EXISTS annotations_%1_dirty AFTER %2 ON annotations BEGIN INSERT OR IGNORE INTO "
                         "log_dirty VALUES ('annotation', %3.id); END")
                     .arg(when, QString(when).toUpper(), row)
              << QString("CREATE TRIGGER IF NOT EXISTS annotations_%1_counted AFTER %2 ON annotations BEGIN UPDATE library_state "
                         "SET value = value + 1 WHERE key = 'annotations'; END")
                     .arg(when, QString(when).toUpper());
    }
    return steps;
}

} // namespace

QStringList AnnotationSchema::passages() {
    return madeAnew(passageColumns, "id, book_id, kind, section, fraction, text, note, created, to_section, to_fraction",
                    "id, book_id, kind, section, fraction, text, note, created, section, fraction");
}

QStringList AnnotationSchema::colors() {
    const QString kept = "id, book_id, kind, section, fraction, text, note, created, to_section, to_fraction, quote, quote_before, "
                         "quote_after, anchored, confidence, lost";
    return madeAnew(QString(passageColumns) + ", color TEXT NOT NULL DEFAULT ''", kept, kept);
}

QStringList AnnotationSchema::regions() {
    const QString kept = "id, book_id, kind, section, fraction, text, note, created, to_section, to_fraction, quote, quote_before, "
                         "quote_after, anchored, confidence, lost, color";
    QStringList steps = madeAnew(QString(passageColumns) + ", color TEXT NOT NULL DEFAULT '', area TEXT NOT NULL DEFAULT ''", kept, kept);
    // The words of a highlight's passage, a note, and a bookmark's name or
    // the recognised text of a page's ink (#84), folded as the graph's word
    // index folds them (GraphIndex), and contentless: the rows stay where
    // they are. Made anew whole, so the step can be taken again.
    steps << QString("DROP TABLE IF EXISTS annotation_words")
          << QString("CREATE VIRTUAL TABLE annotation_words USING fts5(quote, note, text, content='', contentless_delete=1, "
                     "tokenize=\"unicode61 remove_diacritics 2 categories 'L* N* Co M*'\", prefix='2 3')")
          << QString("INSERT INTO annotation_words (rowid, quote, note, text) SELECT id, quote, note, text FROM annotations");
    return steps + wordTriggers();
}

QStringList AnnotationSchema::wordTriggers() {
    // FTS5 keeps no copy of what it was given: a row's words are put in again whole.
    constexpr auto words =
        "INSERT OR REPLACE INTO annotation_words (rowid, quote, note, text) VALUES (NEW.id, NEW.quote, NEW.note, NEW.text)";
    return {QString("CREATE TRIGGER IF NOT EXISTS annotations_words_insert AFTER INSERT ON annotations BEGIN %1; END").arg(words),
            QString("CREATE TRIGGER IF NOT EXISTS annotations_words_update AFTER UPDATE OF quote, note, text ON annotations BEGIN %1; END")
                .arg(words),
            QString("CREATE TRIGGER IF NOT EXISTS annotations_words_delete AFTER DELETE ON annotations BEGIN DELETE FROM annotation_words "
                    "WHERE rowid = OLD.id; END")};
}
