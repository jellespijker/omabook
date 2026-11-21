#include "fileschema.h"

#include <QString>
#include <utility>

using namespace Qt::StringLiterals;

QStringList FileSchema::originals() {
    QStringList steps{
        u"DROP TABLE IF EXISTS formats_next"_s,
        u"CREATE TABLE formats_next (book_id INTEGER NOT NULL REFERENCES books(id) ON DELETE CASCADE, format TEXT NOT NULL, "
        "file TEXT NOT NULL, size INTEGER NOT NULL, hash TEXT NOT NULL, original TEXT NOT NULL DEFAULT '', PRIMARY KEY (book_id, format))"_s,
        u"INSERT INTO formats_next (book_id, format, file, size, hash) SELECT book_id, format, file, size, hash FROM formats"_s,
        u"DROP TABLE formats"_s,
        u"ALTER TABLE formats_next RENAME TO formats"_s,
        u"CREATE INDEX formats_hash ON formats(hash)"_s,
        u"CREATE INDEX formats_original ON formats(original) WHERE original <> ''"_s};
    // Its triggers, gone with the table it was: what the change log and the
    // window's counters follow it by.
    for (const auto &[when, row] : {std::pair{"insert", "NEW"}, std::pair{"update", "NEW"}, std::pair{"delete", "OLD"}}) {
        const QString upper = QString::fromLatin1(when).toUpper();
        steps << u"CREATE TRIGGER formats_%1_counted AFTER %2 ON formats BEGIN UPDATE library_state SET value = value + 1 WHERE key = "
                 "'books'; END"_s.arg(QLatin1StringView(when), upper)
              << u"CREATE TRIGGER formats_%1_dirty AFTER %2 ON formats BEGIN INSERT OR IGNORE INTO log_dirty VALUES ('book', "
                 "%3.book_id); END"_s.arg(QLatin1StringView(when), upper, QLatin1StringView(row));
    }
    return steps;
}
