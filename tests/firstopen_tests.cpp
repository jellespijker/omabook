// The first open of a library the change log has not taken in yet (its
// baseline, written as a snapshot since #54), and reconciling one a write
// bypassed (#104): what they write, and how long they take for a large
// library.

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <functional>
#include <map>
#include <utility>

#include "changelog.h"
#include "compaction.h"
#include "library.h"
#include "samples.h"

namespace {

// Runs `fill` on a connection of its own to the database at `dbPath`, in one transaction.
void withDatabase(const QString &dbPath, const std::function<void(QSqlDatabase &)> &fill) {
    const QString connection = "raw-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(dbPath);
        QVERIFY(database.open());
        QVERIFY(QSqlQuery(database).exec("PRAGMA foreign_keys = ON"));
        QVERIFY(database.transaction());
        fill(database);
        QVERIFY(database.commit());
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

void run(QSqlDatabase &database, const QString &sql) {
    QSqlQuery query(database);
    QVERIFY2(query.exec(sql), qPrintable(query.lastError().text() + "\n" + sql));
}

// Takes a library back to before the change log knew of it, as one from
// before the log is when it is first opened: no operations, nothing marked
// as logged, the tenants not placed. Its computer and its clock are fixed,
// the clock far ahead of any row and of today, so each tick of it is the
// next of the last and what is written is the same in every run.
void forgetTheLog(const QString &dbPath) {
    withDatabase(dbPath, [](QSqlDatabase &database) {
        for (const char *table :
             {"ops", "snapshot_ops", "snapshot_marks", "seen", "member_vectors", "log_ids", "log_dirty", "space_members", "spaces"})
            run(database, QString("DELETE FROM %1").arg(table));
        run(database, "UPDATE library_state SET value = -1 WHERE key LIKE 'logged_%' OR key = 'tenancy_one'");
        run(database, "INSERT OR REPLACE INTO library_state (key, value) VALUES ('hlc', 4102444800 * 65536), ('clock_offset', 0)");
        run(database, "INSERT OR REPLACE INTO global_ids (entity, local_id, global_id) "
                      "VALUES ('device', 0, 'the-computer-of-the-tests')");
    });
}

// What the log holds, every byte of it that is written, with the names its
// numbers stand for (log_ids), but without the signatures or whose they
// are: the person's key is made anew in each run.
QByteArray logOf(const Library &library) {
    QByteArray dump;
    const auto add = [&](const QString &sql) {
        dump += sql.toUtf8() + '\n';
        for (QSqlQuery query = ChangeLog::query(library, sql); query.next();) {
            QStringList row;
            for (int column = 0; column < query.record().count(); ++column)
                row << query.value(column).toString();
            dump += row.join('\x1f').toUtf8() + '\n';
        }
    };
    add("SELECT id, CASE WHEN id IN (SELECT signer FROM ops) THEN 'the signer' ELSE name END FROM log_ids ORDER BY id");
    for (const char *table : {"snapshot_ops", "ops"})
        add(QString("SELECT space, device, seq, hlc, author, entity, id, field, value, signer IS NOT NULL FROM %1 "
                    "ORDER BY space, device, seq")
                .arg(table));
    add("SELECT space, device, seq FROM snapshot_marks ORDER BY space, device");
    add("SELECT space, device, seq, hlc FROM seen ORDER BY space, device");
    add("SELECT id, kind, name, owner, created FROM spaces ORDER BY id");
    add("SELECT space, person, role, added FROM space_members ORDER BY space, person");
    add("SELECT key, value FROM library_state WHERE key = 'hlc' OR key LIKE 'logged_%' ORDER BY key");
    add("SELECT entity, local_id, global_id FROM global_ids ORDER BY entity, local_id");
    return dump;
}

QString digest(const QByteArray &dump) { return QString::fromLatin1(QCryptographicHash::hash(dump, QCryptographicHash::Sha256).toHex()); }

// Writes that bypass the log (another program, an older Omabook): a book's
// details, a thing gone with its relations, a fact and a sighting changed,
// a bookmark gone, a book removed.
void bypass(const QString &dbPath) {
    withDatabase(dbPath, [](QSqlDatabase &database) {
        run(database, "UPDATE books SET title = title || ' (revised)', details_at = 1700000000 WHERE id = (SELECT MIN(id) FROM books)");
        run(database, "DELETE FROM graph_nodes WHERE id = (SELECT MAX(id) FROM graph_nodes)");
        run(database, "UPDATE graph_facts SET value = value || '!' WHERE id = (SELECT MIN(id) FROM graph_facts)");
        run(database, "UPDATE graph_seen SET section = section + 1 WHERE rowid = (SELECT MIN(rowid) FROM graph_seen)");
        run(database, "DELETE FROM annotations WHERE id = (SELECT MAX(id) FROM annotations)");
        run(database, "DELETE FROM books WHERE id = (SELECT MAX(id) FROM books)");
    });
}

// Rows written straight into a database, by statements prepared once.
class Rows {
public:
    explicit Rows(QSqlDatabase &database) : m_database(database) {}

    void add(const QString &sql, const QVariantList &values) {
        auto kept = m_prepared.find(sql);
        if (kept == m_prepared.end()) {
            QSqlQuery made(m_database);
            QVERIFY2(made.prepare(sql), qPrintable(made.lastError().text()));
            kept = m_prepared.emplace(sql, std::move(made)).first;
        }
        // An empty string, never NULL, for the columns that take none, as Library::exec binds one.
        for (int at = 0; at < values.size(); ++at)
            kept->second.bindValue(at, values.at(at).typeId() == QMetaType::QString && values.at(at).toString().isNull()
                                           ? QVariant(QString(""))
                                           : values.at(at));
        QVERIFY2(kept->second.exec(), qPrintable(kept->second.lastError().text()));
    }
    // A thing of the graph: its id.
    qint64 thing(const QString &kind, const QString &name, const QVariant &bookId) {
        ++m_things;
        add("INSERT INTO graph_nodes (id, scope, kind, name, summary, data, book_id, updated) VALUES (?, '', ?, ?, ?, '{}', ?, ?)",
            {m_things, kind, name, kind == "book" ? QString("") : "A " + kind + " of the library.", bookId, when + m_things});
        return m_things;
    }
    void relate(qint64 source, qint64 target, const QString &kind, qint64 bookId = 0, int section = 0) {
        add("INSERT OR IGNORE INTO graph_edges (source, target, kind, note, book_id, section, fraction) VALUES (?, ?, ?, '', ?, ?, 0.25)",
            {source, target, kind, bookId, section});
    }

    static constexpr qint64 when = 1700000000;

private:
    QSqlDatabase &m_database;
    std::map<QString, QSqlQuery> m_prepared;
    qint64 m_things = 0;
};

// What the books share: their authors, tags and series.
struct Shared {
    QList<qint64> authors, tags, series;
};

// A book with a file, its reading for some, and its thing related to what it shares.
void addBook(Rows &rows, const Shared &shared, int id) {
    const bool inSeries = id % 3 != 0;
    rows.add("INSERT INTO books (id, title, authors, series, series_index, publisher, language, isbn, published, description, tags, "
             "path, added, details_at, shelf, rating) VALUES (?, ?, ?, ?, ?, 'A Publisher', 'en', ?, '2001', ?, ?, ?, ?, ?, ?, ?)",
             {id, QString("Book %1").arg(id), QString("Author %1").arg(id % shared.authors.size()),
              inSeries ? QString("Series %1").arg(id % shared.series.size()) : QString(), inSeries ? id % 7 + 1 : 0,
              QString("97800000%1").arg(id, 5, 10, QChar('0')), QString("What book %1 is about, told at some length. ").arg(id).repeated(6),
              QString("Tag %1, Tag %2").arg(id % shared.tags.size()).arg((id * 7) % shared.tags.size()), QString("Book %1").arg(id),
              Rows::when + id, Rows::when + id, id % 5 == 0 ? "read" : "", id % 4});
    rows.add("INSERT INTO formats (book_id, format, file, size, hash) VALUES (?, 'epub', ?, ?, ?)",
             {id, QString("Book %1.epub").arg(id), 100000 + id,
              QCryptographicHash::hash(QByteArray::number(id), QCryptographicHash::Sha256).toHex()});
    if (id % 3 == 0)
        rows.add("INSERT INTO progress (book_id, section, fraction, percent, updated) VALUES (?, ?, 0.5, ?, ?)",
                 {id, id % 20, id % 100, Rows::when + id});
    const qint64 self = rows.thing("book", QString("Book %1").arg(id), id);
    rows.relate(self, shared.authors.at(id % shared.authors.size()), "written by");
    if (id % 2 == 0)
        rows.relate(self, shared.authors.at((id * 13) % shared.authors.size()), "written by");
    for (int tag = 0; tag < 6; ++tag)
        rows.relate(self, shared.tags.at((id * (tag + 1)) % shared.tags.size()), "tagged");
    if (inSeries)
        rows.relate(self, shared.series.at(id % shared.series.size()), "in series");
}

// A book's story graph: the characters and places met in it, what was
// learned of them, and how they relate.
void addStory(Rows &rows, qint64 bookId) {
    QList<qint64> cast;
    for (int at = 0; at < 56; ++at) {
        cast << rows.thing(at % 4 == 0 ? "place" : "character", QString("Someone %1 of book %2").arg(at).arg(bookId), bookId);
        if (at < 54)
            rows.add("INSERT OR IGNORE INTO graph_seen (node_id, book_id, section, fraction, anchor) VALUES (?, ?, ?, 0.5, 'where')",
                     {cast.last(), bookId, at % 30});
    }
    for (int at = 0; at < 98; ++at)
        rows.add("INSERT INTO graph_facts (node_id, kind, value, book_id, section, fraction, anchor) "
                 "VALUES (?, 'summary', ?, ?, ?, 0.5, 'a few words')",
                 {cast.at(at % cast.size()), QString("Learned %1 of them.").arg(at), bookId, at % 30});
    for (int at = 0; at < 160; ++at)
        rows.relate(cast.at(at % cast.size()), cast.at((at * 7 + 1) % cast.size()), at % 2 == 0 ? "knows" : "fights", bookId, at % 30);
}

// How a real library is made up (the analysis of 2026-11-20, of a library
// of 1,511 books), `scale` times over: per book a thing for it and its
// authors, tags and series, related to them; a story graph for 27 books in
// 1,511; collections, a few at the top with some inside them, and bookmarks.
void fillLikeARealLibrary(const QString &dbPath, int scale) {
    withDatabase(dbPath, [scale](QSqlDatabase &database) {
        Rows rows(database);
        const int books = 1511 * scale;
        Shared shared;
        for (int at = 0; at < 1450 * scale; ++at)
            shared.authors << rows.thing("author", QString("Author %1").arg(at), {});
        for (int at = 0; at < 30 * scale; ++at)
            shared.tags << rows.thing("tag", QString("Tag %1").arg(at), {});
        for (int at = 0; at < 1050 * scale; ++at)
            shared.series << rows.thing("series", QString("Series %1").arg(at), {});
        for (int id = 1; id <= books; ++id)
            addBook(rows, shared, id);
        for (int told = 0; told < 27 * scale; ++told)
            addStory(rows, 1 + (told * 53) % books);
        for (int id = 1; id <= 12; ++id) {
            rows.add("INSERT INTO collections (id, name, rule, updated, parent) VALUES (?, ?, '', ?, ?)",
                     {id, QString("Collection %1").arg(id), Rows::when, id > 6 ? id - 6 : 0});
            for (int at = id; at <= books; at += 9 + id)
                rows.add("INSERT OR IGNORE INTO collection_books (collection_id, book_id, included) VALUES (?, ?, 1)", {id, at});
        }
        for (int at = 0; at < 300 * scale; ++at)
            rows.add(
                "INSERT INTO annotations (book_id, kind, section, fraction, text, note, created) VALUES (?, 'highlight', ?, 0.5, ?, '', ?)",
                {1 + (at * 17) % books, at % 20, QString("Highlighted %1").arg(at), Rows::when + at});
    });
}

// How long `work` takes, in seconds.
double secondsOf(const std::function<void()> &work) {
    QElapsedTimer timer;
    timer.start();
    work();
    return static_cast<double>(timer.nsecsElapsed()) / 1e9;
}

// The scale of the large library: 1 (as large as the library analysed),
// or what OMABOOK_FIRSTOPEN_SCALE says (10 for the measure of #104).
int largeScale() {
    bool given = false;
    const int scale = qEnvironmentVariableIntValue("OMABOOK_FIRSTOPEN_SCALE", &given);
    return given && scale > 0 ? scale : 1;
}

} // namespace

class FirstOpenTests : public QObject {
    Q_OBJECT

private slots:
    // What the first open of each sample writes into the log, and what
    // reconciling writes after a write bypassed it, byte for byte, as the
    // change log wrote it before it was written in batches (#104).
    void whatTheFirstOpenAndReconcileWriteIsAsBefore_data() {
        QTest::addColumn<QString>("sample");
        QTest::addColumn<QString>("baseline");
        QTest::addColumn<QString>("reconciled");
        QTest::newRow("0.1.0") << "library-0.1.0.sql" << "826ccb2c1aad6219d51fda0e4cce5ff3d164c14f5e2b645a333cccb53d5961f3"
                               << "09a4432cdbb8c6ebf912535d9150c6aaf531dc25bdccb8a0284fc6a90687d97d";
        QTest::newRow("0.1.2") << "library-0.1.2.sql" << "0184b2295a40e93f7fce1ba0ba8ef9eb064cd73c7c39924b8bc4e11bde4d10f5"
                               << "7310bfc49874281696c2c658c81f9e7cd33f61dbcd9816c33244aba78360b96f";
        QTest::newRow("0.1.3") << "library-0.1.3.sql" << "e9d3839d90271c52a28dcd25389961f5200e9048822a5ba69c9f680973b5b25c"
                               << "feb04c4ccb9f354e31c84aaf8fdb41a63029e908ee96fed0de17b954a46845f3";
        QTest::newRow("0.1.4") << "library-0.1.4.sql" << "31030d85cac667b83869ae87165b9f027f6626393ff555e1192b0d6f39276ca7"
                               << "46a8f21b22703b4d592fc472d10074f2e90b44dab3a80f1edd849b1c637c9514";
        QTest::newRow("0.1.5") << "library-0.1.5.sql" << "31030d85cac667b83869ae87165b9f027f6626393ff555e1192b0d6f39276ca7"
                               << "46a8f21b22703b4d592fc472d10074f2e90b44dab3a80f1edd849b1c637c9514";
    }
    void whatTheFirstOpenAndReconcileWriteIsAsBefore() {
        QFETCH(QString, sample);
        QFETCH(QString, baseline);
        QFETCH(QString, reconciled);
        QTemporaryDir dir;
        const QString db = dir.filePath("omabook.db");
        int format = 0;
        Samples::loadSqlDump(QFINDTESTDATA("data/" + sample), db, format);
        {
            Library upgraded(dir.path());
            QVERIFY(upgraded.open());
        }
        forgetTheLog(db);
        QString first;
        {
            Library library(dir.path());
            QVERIFY(library.open());
            const QByteArray dump = logOf(library);
            QVERIFY(dump.count('\n') > 20);
            first = digest(dump);
        }
        bypass(db);
        Library library(dir.path());
        QVERIFY(library.open());
        // Both at once, so a difference in either shows what both are.
        QCOMPARE(first + " " + digest(logOf(library)), baseline + " " + reconciled);
    }

    // The first open of a large library, its baseline and its tenants, and
    // reconciling it after a write bypassed the log, each well within a
    // minute: about 3 minutes at ten times the library analysed, before
    // they were written in batches (#104).
    void aLargeLibraryOpensTheFirstTimeQuickly() {
        const int scale = largeScale();
        QTemporaryDir dir;
        const QString db = dir.filePath("omabook.db");
        {
            Library made(dir.path());
            QVERIFY(made.open());
        }
        const double filling = secondsOf([&] { fillLikeARealLibrary(db, scale); });
        forgetTheLog(db);
        qint64 operations = 0;
        const double first = secondsOf([&] {
            Library library(dir.path());
            QVERIFY(library.open());
            QSqlQuery count = ChangeLog::query(library, "SELECT COUNT(*) FROM snapshot_ops");
            operations = count.next() ? count.value(0).toLongLong() : 0;
        });
        bypass(db);
        qint64 reconciled = 0;
        const double reconcile = secondsOf([&] {
            Library library(dir.path());
            QVERIFY(library.open());
            QSqlQuery count = ChangeLog::query(library, "SELECT COUNT(*) FROM ops");
            reconciled = count.next() ? count.value(0).toLongLong() : 0;
        });
        const double again = secondsOf([&] {
            Library library(dir.path());
            QVERIFY(library.open());
        });
        Library library(dir.path());
        QVERIFY(library.open());
        const double compacting = secondsOf([&] { Compaction::compact(library); });
        qInfo().noquote() << QString("at %1x (%2 books, filled in %3 s): first open %4 s writing %5 operations, reconcile %6 s writing %9, "
                                     "open again %7 s, compaction %8 s")
                                 .arg(scale)
                                 .arg(1511 * scale)
                                 .arg(filling, 0, 'f', 1)
                                 .arg(first, 0, 'f', 2)
                                 .arg(operations)
                                 .arg(reconcile, 0, 'f', 2)
                                 .arg(again, 0, 'f', 2)
                                 .arg(compacting, 0, 'f', 2)
                                 .arg(reconciled);
        QVERIFY(operations > 10000 * scale);
        QVERIFY(reconciled > 0);
        // Three seconds for each time the library analysed: the 30 s of #104 at ten times.
        QVERIFY2(first < 3.0 * scale, qPrintable(QString("the first open took %1 s").arg(first)));
        QVERIFY2(reconcile < 3.0 * scale, qPrintable(QString("reconciling took %1 s").arg(reconcile)));
    }
};

QTEST_GUILESS_MAIN(FirstOpenTests)
#include "firstopen_tests.moc"
