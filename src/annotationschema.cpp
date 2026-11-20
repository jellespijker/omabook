#include "annotationschema.h"

#include <QString>
#include <utility>

// The annotations table made anew with a passage's columns, as SQLite adds a
// column only once: so the step can be taken again (a library put back to
// the format before). Its rows keep their numbers, and the numbers it gave
// out stay given (sqlite_sequence), so the change log's ids for removed ones
// never come back; its triggers are made again with it.
QStringList AnnotationSchema::passages() {
    const QString table = "CREATE TABLE annotations_next (id INTEGER PRIMARY KEY AUTOINCREMENT, "
                          "book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, kind TEXT NOT NULL, section INTEGER NOT NULL, "
                          "fraction REAL NOT NULL, text TEXT NOT NULL DEFAULT '', note TEXT NOT NULL DEFAULT '', created INTEGER NOT NULL, "
                          "to_section INTEGER NOT NULL DEFAULT 0, to_fraction REAL NOT NULL DEFAULT 0, quote TEXT NOT NULL DEFAULT '', "
                          "quote_before TEXT NOT NULL DEFAULT '', quote_after TEXT NOT NULL DEFAULT '', anchored TEXT NOT NULL DEFAULT '', "
                          "confidence REAL NOT NULL DEFAULT 1, lost INTEGER NOT NULL DEFAULT 0)";
    const QString copy =
        "INSERT INTO annotations_next (id, book_id, kind, section, fraction, text, note, created, to_section, to_fraction) "
        "SELECT id, book_id, kind, section, fraction, text, note, created, section, fraction FROM annotations";
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
