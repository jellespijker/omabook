// The books a computer got by a sync before #210 (#213): it wrote none of
// their fields into the change log as its own, so a change made there later
// went nowhere, and a third computer got them without their details. The
// upgrade to format 45 writes them once, at the stamps the log has them by,
// and changes nothing else.
//
// A library from before #210 is made here as that Omabook left it: synced,
// then without the operations the receiving computer's device wrote of what
// it took (each the same value at the same stamp as the other device's), and
// at format 44.

#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "changelog.h"
#include "changelogfields.h"
#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "librarymigrations.h"
#include "remote.h"
#include "replication.h"
#include "spacereducer.h"
#include "spaces.h"
#include "sync.h"
#include "syncplan.h"

namespace {

const QString title = QStringLiteral("Clanbook: Ventrue");

[[nodiscard]] BookMeta details(const QString &named = title) {
    BookMeta meta;
    meta.title = named;
    meta.authors = {"Ann Author"};
    meta.isbn = "9781565048836";
    meta.series = "Clanbooks";
    meta.seriesIndex = 3;
    meta.description = "The clan of kings.";
    meta.publisher = "White Wolf";
    meta.tags = {"Vampire"};
    return meta;
}

// The book, on paper or of a file in `folder`, with a Goodreads id and read.
[[nodiscard]] qint64 made(Library &library, bool paper, const QString &folder) {
    qint64 id = 0;
    if (paper)
        id = library.addEntry(details(), true).id;
    else if (Fixtures::epub(folder + "/ventrue.epub", title))
        id = library.add(folder + "/ventrue.epub", details()).id;
    return id > 0 && library.setIdentifier(id, "goodreads", "1014999") && library.setReading(id, "read", 4, {}) ? id : 0;
}

// The first value `sql` answers of a library, or -1.
[[nodiscard]] qint64 answer(const Library &library, const QString &sql, const QVariantList &values = {}) {
    QSqlQuery query = ChangeLog::query(library, sql, values);
    return query.next() ? query.value(0).toLongLong() : -1;
}

// What ChangeLogFields::write writes of a book, as SQL.
const QString bookFields = QStringLiteral("('title', 'authors', 'series', 'series_index', 'publisher', 'language', 'isbn', 'published', "
                                          "'description', 'tags', 'identifiers', 'added', 'has_cover', 'cover_hash', 'path', 'formats', "
                                          "'shelf', 'rating', 'finished', 'paper')");

// Of `device`'s operations of a book, those of what it took from another (the
// same value at the same stamp as another device's): what an Omabook before
// #210 never wrote, as SQL of the table `table`.
[[nodiscard]] QString echoes(const QString &table) {
    return QString("%1.device = (SELECT id FROM log_ids WHERE name = ?) AND %1.entity = 'book' AND %1.field IN %2 AND EXISTS "
                   "(SELECT 1 FROM live_ops AS o WHERE o.device <> %1.device AND o.entity = 'book' AND o.id = %1.id "
                   "AND o.field = %1.field AND o.value = %1.value AND o.hlc = %1.hlc)")
        .arg(table, bookFields);
}

// The library as an Omabook before #210 left it: none of what `device` took
// of a book written as its own, and at format 44.
[[nodiscard]] bool asBefore210(const Library &library, const QString &device) {
    // Not a correction of the log, which only compaction may take operations
    // out of: the stand-in for a log an older Omabook wrote, which never had them.
    // ast-grep-ignore: ops-deleted-only-by-compaction
    const bool taken = ChangeLog::exec(library, "DELETE FROM ops WHERE " + echoes("ops"), {device});
    return taken && ChangeLog::exec(library, "DELETE FROM snapshot_ops WHERE " + echoes("snapshot_ops"), {device})
           && ChangeLog::exec(library, "DELETE FROM library_state WHERE key = 'echo_taken_books'")
           && ChangeLog::exec(library, "PRAGMA user_version = 44");
}

// Changes `field` of a book to `value` (empty: clears it), a
// detail a minute on, as a sync's details are timed to the second: a minute
// after now, and after the book's details as they were. Two changes in one
// second stated at one time would be one moment, which no sync can order (#218).
[[nodiscard]] bool change(Library &library, qint64 id, const QString &field, const QString &value) {
    const Book book = *library.book(id);
    if (field == "rating")
        return library.setReading(id, book.shelf, value.toInt(), book.finished);
    BookMeta meta = book.meta;
    QJsonObject json = meta.toJson();
    json.insert(field, value);
    meta.applyJson(json);
    const qint64 at = std::max(QDateTime::currentSecsSinceEpoch(), Hlc::seconds(book.detailsAt)) + 60;
    return library.setMeta(id, meta, nullptr, Hlc::pack(at, 0));
}

// The book titled `title` of a library, or 0.
[[nodiscard]] qint64 titled(const Library &library) {
    const QList<Book> books = library.books();
    const auto found = std::ranges::find_if(books, [](const Book &book) { return book.meta.title == title; });
    return found == books.cend() ? 0 : found->id;
}

[[nodiscard]] QString said(const SyncSummary &summary) {
    return (QStringList{summary.error} + summary.problems + summary.unannounced).join('\n');
}

[[nodiscard]] bool clean(const SyncSummary &summary) {
    return summary.error.isEmpty() && summary.problems.isEmpty() && summary.unannounced.isEmpty();
}

} // namespace

class TakenBooksTests : public QObject {
    Q_OBJECT

private slots:
    void aBookGotBeforeTravelsOnOnceUpgraded_data() {
        QTest::addColumn<bool>("paper");
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        const QList<std::pair<QString, QString>> changes{{"description", "Lords of the night."}, {"description", ""}, {"rating", "5"}};
        for (const bool paper : {true, false})
            for (const auto &[field, value] : changes)
                QTest::addRow("%s, the %s %s", paper ? "on paper" : "a file", qPrintable(field), value.isEmpty() ? "cleared" : "changed")
                    << paper << field << value;
    }

    // A has the book and B got it by a sync, before #210. Once both are
    // upgraded, C, syncing with B, gets the book whole, and a change of it
    // made on B reaches A and C, each sync as its plan said: a detail, changed
    // or cleared, or its stars changed, which the newer wins (ADR 0009).
    void aBookGotBeforeTravelsOnOnceUpgraded() {
        QFETCH(const bool, paper);
        QFETCH(const QString, field);
        QFETCH(const QString, value);
        QTemporaryDir dir;
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        const QString rootA = dir.filePath("A");
        const QString rootB = dir.filePath("B");
        {
            Library a(rootA);
            Library b(rootB);
            QVERIFY(a.open() && b.open());
            QVERIFY(made(a, paper, dir.path()) > 0);
            const SyncSummary first = syncLibrary(a, Remote("laptop:" + rootB, {ssh}));
            QVERIFY2(clean(first), qPrintable(said(first)));
            const QString deviceB = Spaces::deviceId(b);
            const qint64 wrote = answer(b, "SELECT COUNT(*) FROM live_ops AS b WHERE " + echoes("b"), {deviceB});
            QVERIFY(wrote > 0);
            QVERIFY(asBefore210(a, deviceB) && asBefore210(b, deviceB));
        }
        Library a(rootA);
        Library b(rootB);
        Library c(dir.filePath("C"));
        QVERIFY(a.open() && b.open() && c.open());
        QCOMPARE(b.formatVersion(), LibraryMigrations::currentFormat());
        const qint64 mine = a.books().first().id;
        const qint64 theirs = b.books().first().id;
        // C, syncing with B with `options`, gets the book as A has it.
        const auto cGetsItWhole = [&](const SyncOptions &options) {
            const SyncSummary third = syncLibrary(c, Remote("laptop:" + rootB, {ssh}), {}, options);
            QVERIFY2(clean(third), qPrintable(said(third)));
            QCOMPARE(c.books().size(), 1);
            const SyncPlan::State there = SyncPlan::stateOf(c, c.books().first());
            const SyncPlan::State here = SyncPlan::stateOf(a, *a.book(mine));
            for (const char *name :
                 {"title", "authors", "isbn", "series", "description", "publisher", "tags", "identifiers", "paper", "rating"})
                QCOMPARE(there.value(QLatin1StringView(name)), here.value(QLatin1StringView(name)));
        };
        cGetsItWhole({});
        if (QTest::currentTestFailed())
            return;

        QVERIFY(change(b, theirs, field, value));
        const QJsonValue after = SyncPlan::stateOf(b, *b.book(theirs)).value(field);
        SyncOptions options;
        if (value.isEmpty()) {
            // A clear is refused until it is accepted for the book (#187).
            const SyncSummary refused = syncLibrary(a, Remote("laptop:" + rootB, {ssh}));
            QVERIFY2(refused.error.contains(title), qPrintable(said(refused)));
            options.accept = {title};
        }
        const SyncSummary synced = syncLibrary(a, Remote("laptop:" + rootB, {ssh}), {}, options);
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QCOMPARE(SyncPlan::stateOf(a, *a.book(mine)).value(field), after);
        // C had what B cleared: it is refused there too, until accepted.
        cGetsItWhole(options);
    }

    // A computer that joined another's space (#51) numbers its operations
    // there from 1 again, after those of its own space, so of one field its
    // last is its latest by stamp, not by number (#216). B, the one that
    // joined, once wrote the book's ISBN in its own space; it took A's clear
    // of it, and then A's ISBN again: B writes that as its own, so a third
    // computer gets the book with its ISBN, and a sync with A plans nothing
    // of it on B.
    void aComputerThatJoinedAnothersSpaceWritesWhatItTakes() {
        QTemporaryDir dir;
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        QStringList roots{dir.filePath("X"), dir.filePath("Y")};
        QStringList devices;
        for (const QString &root : roots) {
            Library library(root);
            QVERIFY(library.open());
            devices << Spaces::deviceId(library);
        }
        // B is the one whose space sorts later: it writes in A's once they are paired.
        if (devices.first() > devices.last()) {
            std::ranges::reverse(roots);
            std::ranges::reverse(devices);
        }
        Library a(roots.first());
        Library b(roots.last());
        Library c(dir.filePath("C"));
        QVERIFY(a.open() && b.open() && c.open());
        const QString ownSpace = "personal:" + devices.last();
        for (int n = 0; n < 4; ++n) { // what B had of its own, numbered in its own space: other books
            BookMeta other = details(QString("Clanbook %1").arg(n));
            other.isbn.clear();
            QVERIFY(b.addEntry(other, true).id > 0);
        }
        const qint64 mine = made(a, true, dir.path());
        QVERIFY(mine > 0);
        const Remote toB("laptop:" + roots.last(), {ssh});
        const SyncSummary first = syncLibrary(a, toB);
        QVERIFY2(clean(first), qPrintable(said(first)));
        const qint64 theirs = titled(b);
        QVERIFY(theirs > 0);
        const QString gid = Spaces::bookId(b, theirs);
        // The ISBN as B wrote it in its own space before it joined A's: a higher number, an older stamp.
        ChangeLog::append(b, {.space = ownSpace,
                              .entity = "book",
                              .id = gid,
                              .field = "isbn",
                              .value = details().isbn,
                              .hlc = Hlc::pack(QDateTime::currentSecsSinceEpoch() - 86400, 0)});

        QVERIFY(change(a, mine, "isbn", ""));
        SyncOptions accepted;
        accepted.accept = {title};
        const SyncSummary cleared = syncLibrary(a, toB, {}, accepted);
        QVERIFY2(clean(cleared), qPrintable(said(cleared)));
        QCOMPARE(b.book(theirs)->meta.isbn, QString());
        QVERIFY(change(a, mine, "isbn", details().isbn));
        const SyncSummary again = syncLibrary(a, toB);
        QVERIFY2(clean(again), qPrintable(said(again)));
        QCOMPARE(b.book(theirs)->meta.isbn, details().isbn);
        // B's last number in its own space, and in A's (`comparison`).
        const auto lastIn = [&](const char *comparison) {
            return answer(
                b,
                QString("SELECT MAX(o.seq) FROM ops AS o JOIN log_ids AS s ON s.id = o.space JOIN log_ids AS d ON d.id = o.device "
                        "WHERE s.name %1 ? AND d.name = ?")
                    .arg(QLatin1StringView(comparison)),
                {ownSpace, devices.last()});
        };
        QVERIFY(lastIn("=") > lastIn("<>"));

        // What B holds as its device's operations say, as every sync reads it.
        const ReducedState held = SpaceReducer::reduce(Replication::operations(b), devices.last());
        QCOMPARE(Replication::bookView(held.books.value(gid)).value("isbn").toString(), details().isbn);
        SyncOptions dry;
        dry.dryRun = true;
        const SyncSummary planned = syncLibrary(a, toB, {}, dry);
        QVERIFY2(clean(planned) && planned.breaches.isEmpty(), qPrintable(said(planned) + planned.breaches.join('\n')));
        QVERIFY(!planned.plans.there.books.contains(gid));
        const SyncSummary third = syncLibrary(c, toB);
        QVERIFY2(clean(third), qPrintable(said(third)));
        QCOMPARE(c.book(titled(c))->meta.isbn, details().isbn);
    }

    // Of 1,500 books with covers got by a sync (all their operations
    // another device's), the repair writes each field once, the same value
    // at the same stamp, changes nothing a person sees, and the second time
    // nothing; in a few seconds.
    void theRepairEchoesEachBookOnceAndQuickly() {
        QTemporaryDir dir;
        Library library(dir.filePath("B"));
        QVERIFY(library.open());
        constexpr qsizetype books = 1500;
        {
            const Library::Batch batch(library);
            QImage cover(300, 450, QImage::Format_RGB32);
            for (qsizetype n = 0; n < books; ++n) {
                cover.fill(QColor::fromHsv(static_cast<int>(n % 360), 120, 200));
                const qint64 id = library.addEntry(details(QString("Clanbook %1").arg(n)), n % 2 == 0).id;
                QVERIFY(id > 0 && library.setCover(id, cover) && library.setReading(id, "read", 1 + static_cast<int>(n % 5), {}));
            }
        }
        const QString me = Spaces::deviceId(library);
        const qint64 elsewhere = ChangeLog::internId(library, "d:elsewhere");
        for (const char *table : {"ops", "snapshot_ops"})
            QVERIFY(ChangeLog::exec(
                library,
                QString("UPDATE %1 SET device = ? WHERE entity = 'book' AND device = (SELECT id FROM log_ids WHERE name = ?)")
                    .arg(QLatin1StringView(table)),
                {elsewhere, me}));
        const SyncPlan::States before = SyncPlan::states(library);

        QElapsedTimer timer;
        timer.start();
        qsizetype echoed = 0;
        {
            const Library::Batch batch(library);
            echoed = ChangeLogFields::echoTaken(library);
        }
        const qint64 took = timer.elapsed();
        qInfo().noquote() << "echoed" << echoed << "books in" << took << "ms";
        QCOMPARE(echoed, books);
        QVERIFY2(took < 15000, qPrintable(QString("%1 ms").arg(took)));

        // Each operation written is one another device wrote, value and stamp.
        const QString mineSql = "FROM live_ops AS m WHERE m.device = (SELECT id FROM log_ids WHERE name = ?)";
        QVERIFY(answer(library, "SELECT COUNT(*) " + mineSql, {me}) >= books * 16);
        QCOMPARE(answer(library,
                        "SELECT COUNT(*) " + mineSql
                            + " AND NOT EXISTS (SELECT 1 FROM live_ops AS o WHERE o.device = ? AND o.entity = m.entity "
                              "AND o.id = m.id AND o.field = m.field AND o.value = m.value AND o.hlc = m.hlc)",
                        {me, elsewhere}),
                 0);
        QCOMPARE(SyncPlan::states(library), before);
        const qint64 written = answer(library, "SELECT COUNT(*) FROM live_ops");
        QCOMPARE(ChangeLogFields::echoTaken(library), 0);
        QCOMPARE(answer(library, "SELECT COUNT(*) FROM live_ops"), written);
    }

    // The upgrade runs the repair once: opened again, nothing is written.
    void theUpgradeRepairsOnce() {
        QTemporaryDir dir;
        const QString root = dir.filePath("B");
        qint64 written = 0;
        {
            Library library(root);
            QVERIFY(library.open());
            QVERIFY(library.addEntry(details(), true).id > 0);
            const QString me = Spaces::deviceId(library);
            const qint64 elsewhere = ChangeLog::internId(library, "d:elsewhere");
            for (const char *table : {"ops", "snapshot_ops"})
                QVERIFY(ChangeLog::exec(
                    library,
                    QString("UPDATE %1 SET device = ? WHERE entity = 'book' AND device = (SELECT id FROM log_ids WHERE name = ?)")
                        .arg(QLatin1StringView(table)),
                    {elsewhere, me}));
            QVERIFY(asBefore210(library, me));
            written = answer(library, "SELECT COUNT(*) FROM live_ops");
        }
        {
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
            QCOMPARE(answer(library, "SELECT COUNT(*) FROM library_state WHERE key = 'echo_taken_books'"), 0);
            QVERIFY(answer(library, "SELECT COUNT(*) FROM live_ops") > written);
            written = answer(library, "SELECT COUNT(*) FROM live_ops");
        }
        Library again(root);
        QVERIFY(again.open());
        QCOMPARE(answer(again, "SELECT COUNT(*) FROM live_ops"), written);
    }
};

QTEST_GUILESS_MAIN(TakenBooksTests)
#include "takenbooks_tests.moc"
