// Spaces, global ids, the hybrid logical clock and the change log (ADR 0001).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>

#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "hlc.h"
#include "library.h"
#include "librarymigrations.h"
#include "spacereplay.h"
#include "spaces.h"

namespace {

void loadSqlDump(const QString &dumpPath, const QString &dbPath, int &formatOut) {
    QFile file(dumpPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString connection = "dump-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(dbPath);
        QVERIFY(database.open());
        for (const QString &statement : QString::fromUtf8(file.readAll()).split(";\n", Qt::SkipEmptyParts)) {
            const QString sql = statement.trimmed();
            if (sql.isEmpty() || sql == "COMMIT" || sql.startsWith("BEGIN"))
                continue;
            QSqlQuery query(database);
            QVERIFY2(query.exec(sql), qPrintable(dumpPath + ": " + query.lastError().text() + "\n" + sql.left(120)));
        }
        QSqlQuery version(database);
        QVERIFY(version.exec("PRAGMA user_version") && version.next());
        formatOut = version.value(0).toInt();
        version.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QStringList tableNames(const QString &dbPath) {
    QStringList names;
    const QString connection = "tables-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(dbPath);
        if (database.open()) {
            QSqlQuery query(database);
            query.exec("SELECT name FROM sqlite_master WHERE type = 'table'");
            while (query.next())
                names << query.value(0).toString();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return names;
}

QJsonObject librarySnapshot(const Library &library) {
    QJsonArray books;
    for (const Book &book : library.books()) {
        const Progress prog = library.progress(book.id);
        books << QJsonObject{{"gid", Spaces::bookId(library, book.id)},
                             {"title", book.meta.title},
                             {"authors", book.meta.authorText()},
                             {"series", book.meta.series},
                             {"series_index", book.meta.seriesIndex},
                             {"publisher", book.meta.publisher},
                             {"isbn", book.meta.isbn},
                             {"tags", QJsonArray::fromStringList(book.meta.tags)},
                             {"shelf", book.shelf},
                             {"rating", book.rating},
                             {"finished", book.finished},
                             {"paper", book.paper},
                             {"details_at", book.detailsAt},
                             {"reading_at", book.readingAt},
                             {"section", prog.section},
                             {"fraction", prog.fraction},
                             {"percent", prog.percent},
                             {"progress_updated", prog.updated}};
    }
    QList<Collection> sortedCols = library.collections(true);
    std::ranges::sort(sortedCols, {}, &Collection::path);
    QJsonArray collections;
    for (const Collection &col : sortedCols) {
        QStringList included;
        for (const qint64 bid : col.included)
            included << Spaces::bookId(library, bid);
        included.sort();
        collections << QJsonObject{{"gid", Spaces::collectionId(library, col.id)},
                                   {"path", col.path},
                                   {"rule", col.rule},
                                   {"removed", col.removed},
                                   {"updated", col.updated},
                                   {"included", QJsonArray::fromStringList(included)}};
    }
    QJsonArray removals;
    for (const Library::Removal &rem : library.removals())
        removals << QJsonObject{{"key", rem.key}, {"at", rem.at}};

    const Graph graph(library);
    QList<QJsonObject> nodeList;
    for (const QString &scope : graph.scopes()) {
        const GraphView view = graph.whole(scope);
        for (const GraphNode &node : view.nodes) {
            QStringList aliases = node.aliases();
            aliases.sort();
            nodeList << QJsonObject{{"gid", Spaces::nodeId(library, node.id)},
                                    {"scope", node.scope},
                                    {"kind", node.kind},
                                    {"name", node.name},
                                    {"summary", node.summary},
                                    {"aliases", QJsonArray::fromStringList(aliases)},
                                    {"facts", int(graph.facts(node.id).size())},
                                    {"seen", int(graph.seen(node.id).size())}};
        }
    }
    std::ranges::sort(nodeList, {}, [](const QJsonObject &obj) { return obj.value("gid").toString(); });
    QJsonArray nodes;
    for (const QJsonObject &obj : nodeList)
        nodes << obj;
    QJsonArray days;
    for (const ReadingDay &day : graph.readingLog())
        days << QJsonObject{{"book", Spaces::bookId(library, day.bookId)}, {"day", day.day}, {"from", day.from}, {"to", day.to}};

    return {{"books", books}, {"collections", collections}, {"removals", removals}, {"nodes", nodes}, {"days", days}};
}

QList<SpaceOperation> sampleOperations() {
    const QString space = "personal:shared";
    const QString b1 = "b:1111111111111111111111111111111111111111111111111111111111111111";
    const QString b2 = "e:entry:title=secondbook|author";
    const QString b3 = "b:3333333333333333333333333333333333333333333333333333333333333333";
    const QString col = "c:Favourites";
    const QString n1 = Spaces::nodeGlobalId({}, "character", "Mara");
    const QString n2 = Spaces::nodeGlobalId({}, "place", "Harlingen");
    const QString f1 = Spaces::factGlobalId(n1, "summary", "Ferry pilot.", b1);
    const QString e1 = Spaces::edgeGlobalId(n1, n2, "sails from");

    const qint64 t1 = Hlc::pack(1794860000, 1);
    const qint64 t2 = Hlc::pack(1794860010, 0);
    const qint64 t3 = Hlc::pack(1794860020, 2);

    return {
        {space, "dev-a", 1, t1, "dev-a", "book", b1, "title", "Early Title"},
        {space, "dev-a", 2, t2, "dev-a", "book", b1, "title", "Title From A"},
        {space, "dev-b", 1, t2, "dev-b", "book", b1, "title", "Title From B"},
        {space, "dev-a", 3, t2, "dev-a", "book", b1, "publisher", "Press A"},
        {space, "dev-b", 2, t3, "dev-b", "book", b1, "publisher", "Press B Later"},
        {space, "dev-a", 4, t1, "dev-a", "book", b1, "tags", "classic"},
        {space, "dev-b", 3, t2, "dev-b", "book", b1, "tag:sea", "0"},
        {space, "dev-a", 5, t2, "dev-a", "book", b1, "tag:sea", "1"},
        {space, "dev-a", 6, t1, "dev-a", "book", b1, "progress", R"({"section":1,"fraction":0.2,"percent":20})"},
        {space, "dev-b", 4, t3, "dev-b", "book", b1, "progress", R"({"section":3,"fraction":0.8,"percent":75})"},
        {space, "dev-a", 7, t1, "dev-a", "book", b2, "title", "Revived Book"},
        {space, "dev-b", 5, t1, "dev-b", "book", b2, "removed", "entry:title=secondbook|author"},
        {space, "dev-a", 8, t2, "dev-a", "book", b2, "shelf", "reading"},
        {space, "dev-a", 9, t1, "dev-a", "book", b3, "title", "Removed Book"},
        {space, "dev-b", 6, t3, "dev-b", "book", b3, "removed", b3.mid(2)},
        {space, "dev-a", 10, t1, "dev-a", "collection", col, "name", "Favourites"},
        {space, "dev-a", 11, t1, "dev-a", "collection", col, "rule", "tag:sea"},
        {space, "dev-b", 7, t2, "dev-b", "collection", col, "book:" + b1, "out"},
        {space, "dev-a", 12, t2, "dev-a", "collection", col, "book:" + b1, "in"},
        {space, "dev-a", 13, t1, "dev-a", "node", n1, "kind", "character"},
        {space, "dev-a", 14, t1, "dev-a", "node", n1, "name", "Mara"},
        {space, "dev-a", 15, t1, "dev-a", "node", n1, "summary", "Ferry pilot."},
        {space, "dev-b", 8, t3, "dev-b", "node", n1, "seen:" + b1, R"({"section":4,"fraction":0.5,"anchor":"late"})"},
        {space, "dev-a", 16, t1, "dev-a", "node", n1, "seen:" + b1, R"({"section":1,"fraction":0.2,"anchor":"early"})"},
        {space, "dev-b", 9, t2, "dev-b", "node", n1, "alias:The Pilot", "0"},
        {space, "dev-a", 17, t2, "dev-a", "node", n1, "alias:The Pilot", "1"},
        {space, "dev-a", 18, t1, "dev-a", "node", n2, "kind", "place"},
        {space, "dev-a", 19, t1, "dev-a", "node", n2, "name", "Harlingen"},
        {space, "dev-b", 10, t3, "dev-b", "fact", f1, "revealed",
         QString(R"({"node":"%1","kind":"summary","value":"Ferry pilot.","book":"%2","section":5,"fraction":0.4,"anchor":"ch5"})")
             .arg(n1, b1)},
        {space, "dev-a", 20, t1, "dev-a", "fact", f1, "revealed",
         QString(R"({"node":"%1","kind":"summary","value":"Ferry pilot.","book":"%2","section":1,"fraction":0.2,"anchor":"ch1"})")
             .arg(n1, b1)},
        {space, "dev-a", 21, t2, "dev-a", "edge", e1, "link",
         QString(R"({"source":"%1","target":"%2","kind":"sails from","note":"Daily","book":"%3","section":2,"fraction":0.1,"anchor":""})")
             .arg(n1, n2, b1)},
        {space, "dev-a", 22, t1, "dev-a", "reading_log", b1 + ":2026-11-16", "range",
         QString(R"({"book":"%1","day":"2026-11-16","from":20,"to":45})").arg(b1)},
        {space, "dev-b", 11, t2, "dev-b", "reading_log", b1 + ":2026-11-16", "range",
         QString(R"({"book":"%1","day":"2026-11-16","from":10,"to":80})").arg(b1)},
    };
}

} // namespace

class SpacesTests : public QObject {
    Q_OBJECT

private slots:
    void hlcIsMonotonicThroughBurstsAndBackwardClockJumps() {
        qint64 wall = 1794860000;
        Hlc clock(0, [&wall] { return wall; });

        const qint64 first = clock.tick();
        const qint64 second = clock.tick();
        const qint64 third = clock.tick();
        QVERIFY(first < second);
        QVERIFY(second < third);
        QCOMPARE(Hlc::seconds(first), wall);
        QCOMPARE(Hlc::counter(first), 0);
        QCOMPARE(Hlc::counter(second), 1);
        QCOMPARE(Hlc::counter(third), 2);

        // If the system clock jumps backwards, the logical clock never goes back.
        wall -= 300;
        const qint64 afterBackwardJump = clock.tick();
        QVERIFY(afterBackwardJump > third);
        QCOMPARE(Hlc::seconds(afterBackwardJump), qint64(1794860000));
        QCOMPARE(Hlc::counter(afterBackwardJump), 3);

        // Once the system clock catches up and passes the logical second,
        // the counter resets to 0 on the new second.
        wall = 1794860010;
        const qint64 caughtUp = clock.tick();
        QVERIFY(caughtUp > afterBackwardJump);
        QCOMPARE(Hlc::seconds(caughtUp), wall);
        QCOMPARE(Hlc::counter(caughtUp), 0);
    }

    void hlcMergesRemoteClocksAndRoundTripsThroughJson() {
        qint64 wall = 1794860000;
        Hlc local(0, [&wall] { return wall; });

        const qint64 t1 = local.tick();
        // A remote event from a clock 5 minutes ahead arrives: observing it
        // lifts our floor, and the next local tick orders after it.
        const qint64 remoteAhead = Hlc::pack(wall + 300, 7);
        local.observe(remoteAhead);
        const qint64 t2 = local.tick();
        QVERIFY(t2 > remoteAhead);
        QVERIFY(remoteAhead > t1);
        QCOMPARE(Hlc::seconds(t2), wall + 300);
        QCOMPARE(Hlc::counter(t2), 8);

        // receive() both observes a remote clock and ticks immediately.
        const qint64 remoteFurther = Hlc::pack(wall + 600, 0);
        const qint64 t3 = local.receive(remoteFurther);
        QVERIFY(t3 > remoteFurther);
        QCOMPARE(Hlc::seconds(t3), wall + 600);
        QCOMPARE(Hlc::counter(t3), 1);

        // Plain Unix-second timestamps from older libraries normalize to
        // counter 0 on the same scale, while 0 stays 0 (undecided).
        QCOMPARE(Hlc::normalize(0), qint64(0));
        QCOMPARE(Hlc::seconds(0), qint64(0));
        QCOMPARE(Hlc::normalize(1794694143), Hlc::pack(1794694143, 0));
        QCOMPARE(Hlc::seconds(1794694143), qint64(1794694143));
        QCOMPARE(Hlc::seconds(t3), wall + 600);
        QVERIFY(Hlc::compare(1794694143, t3) < 0);

        // Packed values fit in 47 bits and round-trip through JSON numbers.
        const QByteArray json = QJsonDocument(QJsonObject{{"hlc", t3}}).toJson(QJsonDocument::Compact);
        const qint64 parsed = QJsonDocument::fromJson(json).object().value("hlc").toInteger();
        QCOMPARE(parsed, t3);
    }

    void hlcSurvivesTwoComputersWithClocksMinutesApart() {
        // Laptop's wall clock is 10 minutes ahead of desktop's.
        qint64 laptopWall = 1794860600;
        qint64 desktopWall = 1794860000;
        Hlc laptop(0, [&laptopWall] { return laptopWall; });
        Hlc desktop(0, [&desktopWall] { return desktopWall; });

        // 1. Laptop edits first.
        const qint64 step1 = laptop.tick();
        // 2. Desktop syncs (observes laptop's timestamp) and then edits while
        //    its own wall clock is still 10 minutes behind laptop's.
        desktop.observe(step1);
        const qint64 step2 = desktop.tick();
        // 3. Laptop syncs (observes desktop's edit) and edits again a second later.
        ++laptopWall;
        laptop.observe(step2);
        const qint64 step3 = laptop.tick();
        // 4. Desktop syncs and edits once more, still minutes behind.
        ++desktopWall;
        desktop.observe(step3);
        const qint64 step4 = desktop.tick();

        QVERIFY(step1 < step2);
        QVERIFY(step2 < step3);
        QVERIFY(step3 < step4);
    }

    void everyReleasedLibraryMigratesToSpacesAndDerivesGlobalIds() {
        const QDir data(QFileInfo(QFINDTESTDATA("data/library-0.1.0.sql")).absolutePath());
        const QStringList dumps = data.entryList({"library-*.sql"}, QDir::Files, QDir::Name);
        QVERIFY(dumps.size() >= 5);

        for (const QString &dump : dumps) {
            QTemporaryDir dir;
            int oldFormat = 0;
            loadSqlDump(data.filePath(dump), dir.filePath("omabook.db"), oldFormat);
            QVERIFY2(oldFormat > 0 && oldFormat < LibraryMigrations::currentFormat(), qPrintable(dump));

            Library library(dir.path());
            QString error;
            QVERIFY2(library.open(&error), qPrintable(dump + ": " + error));
            QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());

            // Backed up before the migration ran.
            const QStringList backups = QDir(library.backupFolder()).entryList({QString("omabook-format-%1-*.db").arg(oldFormat)});
            QCOMPARE(backups.size(), 1);

            const QStringList tables = tableNames(library.databasePath());
            for (const char *required : {"spaces", "space_members", "ops", "seen", "global_ids"})
                QVERIFY2(tables.contains(QLatin1StringView(required)), qPrintable(dump + " missing table " + required));

            // One device id and personal space per library, plus the story world space.
            const QString device = Spaces::deviceId(library);
            QVERIFY(!device.isEmpty());
            QCOMPARE(Spaces::personalSpace(library), QString("personal:" + device));
            QCOMPARE(Spaces::spaceForScope(library, "world:1"), QString("world:1"));

            // Book 1 has a file; books 2 and 3 have no files and are named by their entry keys.
            const QString book1Gid = Spaces::bookId(library, 1);
            const QString book2Gid = Spaces::bookId(library, 2);
            const QString book3Gid = Spaces::bookId(library, 3);
            QVERIFY(!library.formatFiles(1).isEmpty());
            QCOMPARE(book1Gid, QString("b:" + library.formatFiles(1).first().hash));
            QCOMPARE(book2Gid, QString("e:entry:title=apaperbook|bowriter"));
            QCOMPARE(book3Gid, QString("e:entry:title=oneforlater|cyguide"));
            QCOMPARE(Spaces::localId(library, "book", book1Gid), qint64(1));
            QCOMPARE(Spaces::localId(library, "book", book2Gid), qint64(2));
            QCOMPARE(Spaces::localId(library, "book", book3Gid), qint64(3));

            if (oldFormat >= 11) {
                QCOMPARE(Spaces::collectionId(library, 1), QString("c:Stories"));
                QCOMPARE(Spaces::collectionId(library, 2), QString("c:Stories/The Coast"));
                QCOMPARE(Spaces::localId(library, "collection", "c:Stories/The Coast"), qint64(2));
            }

            const Graph graph(library);
            const qint64 ann = graph.named({}, "character", "Ann");
            const qint64 ilse = graph.named("world:1", "character", "Ilse");
            QVERIFY(ann > 0 && ilse > 0);
            const QString annGid = Spaces::nodeId(library, ann);
            const QString ilseGid = Spaces::nodeId(library, ilse);
            QVERIFY(annGid.startsWith("n:") && ilseGid.startsWith("n:") && annGid != ilseGid);
            QCOMPARE(Spaces::localId(library, "node", annGid), ann);
            QCOMPARE(Spaces::localId(library, "node", ilseGid), ilse);

            const QList<GraphFact> facts = graph.facts(ann);
            QVERIFY(!facts.isEmpty());
            const QString factGid = Spaces::factId(library, facts.first().id);
            QVERIFY(factGid.startsWith("f:"));
            QCOMPARE(Spaces::localId(library, "fact", factGid), facts.first().id);

            // Step 4: opening wrote the initial baseline into `ops` once, timed
            // by each row's existing timestamp (0 when 0). Re-opening is a no-op.
            const QList<SpaceOperation> initialOps = SpaceReplay::operations(library);
            QVERIFY(!initialOps.isEmpty());
            bool sawWorldSpaceOp = false;
            bool sawZeroTimestampBookField = false;
            for (const SpaceOperation &op : initialOps) {
                if (op.space == "world:1")
                    sawWorldSpaceOp = true;
                if (op.entity == "book" && op.id == book2Gid && op.field == "title")
                    sawZeroTimestampBookField = (op.hlc == 0);
            }
            QVERIFY(sawWorldSpaceOp);
            QVERIFY(sawZeroTimestampBookField);

            Library reopened(dir.path());
            QVERIFY(reopened.open());
            QCOMPARE(SpaceReplay::operations(reopened).size(), initialOps.size());
        }
    }

    void everyTravellingWriteAppendsToChangeLogInOnePath() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "A Voyage"));
        Library library(dir.filePath("Lib"));
        QVERIFY(library.open());

        const qint64 bookId = library.add(dir.filePath("book.epub")).id;
        QVERIFY(bookId > 0);
        const QString bookGid = Spaces::bookId(library, bookId);
        int countAfterAdd = SpaceReplay::operations(library).size();
        QVERIFY(countAfterAdd > 0);

        // 1. Reading place (`setProgress`).
        library.setProgress(bookId, Progress{.section = 2, .fraction = 0.42, .percent = 42.0, .updated = 1794860100});
        QList<SpaceOperation> ops = SpaceReplay::operations(library);
        QVERIFY(ops.size() > countAfterAdd);
        QCOMPARE(ops.last().entity, QString("book"));
        QCOMPARE(ops.last().id, bookGid);
        QCOMPARE(ops.last().field, QString("progress"));
        QCOMPARE(Hlc::seconds(ops.last().hlc), qint64(1794860100));
        countAfterAdd = ops.size();

        // 2. Reading shelf and paper edition (`setReading`, `setPaper`).
        QVERIFY(library.setReading(bookId, "read", 5, "2026-11-16"));
        QVERIFY(library.setPaper(bookId, true));
        ops = SpaceReplay::operations(library);
        QVERIFY(ops.size() > countAfterAdd);
        countAfterAdd = ops.size();

        // 3. Book details (`setMeta`, `setIdentifier`, `setDetailsAt`).
        BookMeta meta = library.book(bookId)->meta;
        meta.publisher = "Noordzee Pers";
        meta.tags = QStringList{"zeilen", "kust"};
        QVERIFY(library.setMeta(bookId, meta));
        QVERIFY(library.setIdentifier(bookId, "isbn", "9789000000002"));
        QVERIFY(library.setDetailsAt(bookId, 1794860300));
        ops = SpaceReplay::operations(library);
        QVERIFY(ops.size() > countAfterAdd);
        const QHash<QString, qint64> fieldHlcs = SpaceReplay::bookFieldHlcs(library, bookId);
        QVERIFY(fieldHlcs.value("publisher") > 0);
        QVERIFY(fieldHlcs.value("isbn") > 0);
        QVERIFY(fieldHlcs.value("tags") > 0);
        countAfterAdd = ops.size();

        // 4. Collections (`addCollection`, `putInCollection`, `removeCollection`).
        const auto colId = library.addCollection("Zee", "");
        QVERIFY(colId.has_value());
        QVERIFY(library.putInCollection(*colId, bookId, true));
        ops = SpaceReplay::operations(library);
        QVERIFY(ops.size() > countAfterAdd);
        countAfterAdd = ops.size();

        // 5. Graph (`upsert`, `learn`, `link`, `logReading`).
        Graph graph(library);
        const qint64 ship = graph.upsert({}, "thing", "De Stern", "Een tweemaster.", {}, GraphPlace{bookId, 1, 0.25});
        const qint64 port = graph.upsert({}, "place", "Harlingen", "Havenstad.");
        QVERIFY(ship > 0 && port > 0);
        QVERIFY(graph.learn(ship, "summary", "Gebouwd in 1912.", GraphMoment{GraphPlace{bookId, 2, 0.5}, {}}) > 0);
        QVERIFY(graph.link(ship, port, "thuishaven", "Sinds 1920", GraphPlace{bookId, 2, 0.6}) > 0);
        graph.logReading(bookId, 20.0, 42.0, "2026-11-16");
        ops = SpaceReplay::operations(library);
        QVERIFY(ops.size() > countAfterAdd);
        countAfterAdd = ops.size();

        // 6. Removals (`removeCollection`, `remove`).
        QVERIFY(library.removeCollection(*colId));
        QVERIFY(library.remove(bookId));
        ops = SpaceReplay::operations(library);
        QVERIFY(ops.size() > countAfterAdd);
        QVERIFY(std::ranges::any_of(ops.mid(countAfterAdd), [&](const SpaceOperation &op) {
            return op.entity == "book" && op.id == bookGid && op.field == "removed";
        }));
    }

    void reconcileOnOpenCatchesDirectSqlWritesThatBypassedTheLog() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "Bypass Test"));
        const QString libPath = dir.filePath("Lib");
        QString bookGid;
        qint64 bookId = 0;
        int opsBeforeBypass = 0;

        {
            Library library(libPath);
            QVERIFY(library.open());
            bookId = library.add(dir.filePath("book.epub")).id;
            QVERIFY(bookId > 0);
            bookGid = Spaces::bookId(library, bookId);
            opsBeforeBypass = SpaceReplay::operations(library).size();
        }

        // Simulate a write path or external sqlite3 edit that mutated tables
        // directly (triggering `library_state` counters) without calling ChangeLog.
        const QString conn = "bypass-" + QUuid::createUuid().toString(QUuid::Id128);
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(QDir(libPath).filePath("omabook.db"));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec("UPDATE books SET publisher = 'Stille Drukker', details_at = 1794870000 WHERE id = 1"));
            QVERIFY(query.exec("INSERT OR REPLACE INTO progress (book_id, section, fraction, percent, updated) "
                               "VALUES (1, 3, 0.75, 75.0, 1794870010)"));
            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        // Re-opening runs `ChangeLog::reconcile` and appends the missing ops.
        Library reopened(libPath);
        QVERIFY(reopened.open());
        const QList<SpaceOperation> opsAfterReconcile = SpaceReplay::operations(reopened);
        QVERIFY(opsAfterReconcile.size() > opsBeforeBypass);

        bool foundBypassedPublisher = false;
        bool foundBypassedProgress = false;
        for (const SpaceOperation &op : opsAfterReconcile.mid(opsBeforeBypass)) {
            if (op.entity == "book" && op.id == bookGid && op.field == "publisher" && op.value == "Stille Drukker")
                foundBypassedPublisher = true;
            if (op.entity == "book" && op.id == bookGid && op.field == "progress")
                foundBypassedProgress = true;
        }
        QVERIFY(foundBypassedPublisher);
        QVERIFY(foundBypassedProgress);

        // Opening once more after reconciliation appends nothing new.
        const int countAfterReconcile = opsAfterReconcile.size();
        Library thirdOpen(libPath);
        QVERIFY(thirdOpen.open());
        QCOMPARE(SpaceReplay::operations(thirdOpen).size(), countAfterReconcile);
    }

    void twoComputersDeriveIdenticalGlobalIdsAndRetainBookIdAcrossFormats() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "The Shared Book"));
        QVERIFY(Fixtures::epub(dir.filePath("other.epub"), "Only On There"));
        QVERIFY(Fixtures::write(dir.filePath("extra.txt"), "A second format.\n"));

        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());

        // Give There an extra book first so every local row id differs between Here and There.
        QVERIFY(there.add(dir.filePath("other.epub")).id > 0);

        const qint64 bookHere = here.add(dir.filePath("shared.epub")).id;
        const qint64 bookThere = there.add(dir.filePath("shared.epub")).id;
        QVERIFY(bookHere != bookThere);
        const QString initialBookGid = Spaces::bookId(here, bookHere);
        QVERIFY(initialBookGid.startsWith("b:"));
        QCOMPARE(Spaces::bookId(there, bookThere), initialBookGid);

        // Adding another format later leaves the book's global id as it was first made.
        QVERIFY(here.addFormat(bookHere, dir.filePath("extra.txt")));
        QCOMPARE(Spaces::bookId(here, bookHere), initialBookGid);

        // A book without a file derives its id from its first entry key.
        BookMeta wish;
        wish.title = "The Unwritten Volume";
        wish.authors = QStringList{"Ursula K. Le Guin"};
        const qint64 wishHere = here.addEntry(wish, false).id;
        const qint64 wishThere = there.addEntry(wish, false).id;
        QVERIFY(wishHere != wishThere);
        QCOMPARE(Spaces::bookId(here, wishHere), Spaces::bookId(there, wishThere));
        QVERIFY(Spaces::bookId(here, wishHere).startsWith("e:entry:"));

        // Collections derive their id from their path.
        const auto colHere = here.addCollection("Studie/Python", "tag:python");
        const auto colThere = there.addCollection("Studie/Python", "tag:python");
        QVERIFY(colHere && colThere);
        QCOMPARE(Spaces::collectionId(here, *colHere), QString("c:Studie/Python"));
        QCOMPARE(Spaces::collectionId(there, *colThere), QString("c:Studie/Python"));

        // Graph nodes, relations and facts derive identical ids regardless of row numbers or name casing.
        Graph graphHere(here);
        Graph graphThere(there);
        const qint64 gedHere = graphHere.upsert({}, "character", "Ged", "A young mage.", {}, GraphPlace{bookHere, 1, 0.2});
        const qint64 gedThere = graphThere.upsert({}, "character", "  ged ", "A young mage.", {}, GraphPlace{bookThere, 1, 0.2});
        const qint64 rokeHere = graphHere.upsert({}, "place", "Roke", "The island of the wise.");
        const qint64 rokeThere = graphThere.upsert({}, "place", "roke", "The island of the wise.");
        QCOMPARE(Spaces::nodeId(here, gedHere), Spaces::nodeId(there, gedThere));
        QCOMPARE(Spaces::nodeId(here, rokeHere), Spaces::nodeId(there, rokeThere));

        const qint64 edgeHere = graphHere.link(gedHere, rokeHere, "studied at");
        const qint64 edgeThere = graphThere.link(gedThere, rokeThere, "Studied At");
        QCOMPARE(Spaces::edgeId(here, edgeHere), Spaces::edgeId(there, edgeThere));

        const qint64 factHere = graphHere.facts(gedHere).first().id;
        const qint64 factThere = graphThere.facts(gedThere).first().id;
        QCOMPARE(Spaces::factId(here, factHere), Spaces::factId(there, factThere));

        // Two id-less books (neither a file hash nor an entry key) get random
        // "u:<uuid>" ids stored once in global_ids and never share an id.
        const QString random1 = Spaces::bookGlobalId({}, {});
        const QString random2 = Spaces::bookGlobalId({}, {});
        QVERIFY(random1.startsWith("u:") && random2.startsWith("u:") && random1 != random2);
        const auto idlessHere = ChangeLog::insert(here, "INSERT INTO books (title, authors, path, added) VALUES ('', '', '', 0)");
        const auto idlessThere = ChangeLog::insert(there, "INSERT INTO books (title, authors, path, added) VALUES ('', '', '', 0)");
        QVERIFY(idlessHere && idlessThere);
        const QString idlessGidHere = Spaces::bookId(here, *idlessHere);
        const QString idlessGidThere = Spaces::bookId(there, *idlessThere);
        QVERIFY(idlessGidHere.startsWith("u:") && idlessGidThere.startsWith("u:"));
        QVERIFY(idlessGidHere != idlessGidThere);
        QCOMPARE(Spaces::bookId(here, *idlessHere), idlessGidHere);
    }

    void replayingTheLogRebuildsEveryTravellingRow() {
        QTemporaryDir dir;
        int oldFormat = 0;
        QDir().mkpath(dir.filePath("Source"));
        loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), dir.filePath("Source/omabook.db"), oldFormat);

        Library source(dir.filePath("Source"));
        QVERIFY(source.open());
        // Add further mutations on top of the migrated library.
        source.setProgress(1, {.section = 2, .fraction = 0.75, .percent = 85.0, .updated = 1794880000});
        const auto extraCol = source.addCollection("Stories/Favourites", "rating:4");
        QVERIFY(extraCol.has_value());
        QVERIFY(source.putInCollection(*extraCol, 3, true));
        Graph(source).logReading(1, 60.0, 85.0, "2026-11-16");

        const QJsonObject expected = librarySnapshot(source);
        const QList<SpaceOperation> ops = SpaceReplay::operations(source);
        QVERIFY(!ops.isEmpty());

        // 1. Applying the log to a brand-new empty library rebuilds every row.
        Library rebuilt(dir.filePath("Rebuilt"));
        QVERIFY(rebuilt.open());
        ChangeLog::exec(rebuilt, "DELETE FROM ops");
        ChangeLog::exec(rebuilt, "DELETE FROM seen");
        QVERIFY(SpaceReplay::apply(rebuilt, ops));
        QCOMPARE(librarySnapshot(rebuilt), expected);

        // 2. Wiping the travelling tables in `source` and replaying its `ops`
        //    restores the rows identically.
        {
            const ChangeLog::Scope guard;
            for (const char *table : {"reading_log", "graph_facts", "graph_edges", "graph_seen", "graph_nodes", "collection_books",
                                      "collections", "progress", "formats", "removed", "books"})
                ChangeLog::exec(source, QString("DELETE FROM %1").arg(table));
        }
        QCOMPARE(source.books().size(), 0);
        QVERIFY(SpaceReplay::replay(source));
        QCOMPARE(librarySnapshot(source), expected);
    }

    void randomOperationsInRandomOrdersConvergeToTheSameState() {
        const QList<SpaceOperation> baseOps = sampleOperations();
        QTemporaryDir dir;

        Library canonical(dir.filePath("Canonical"));
        QVERIFY(canonical.open());
        ChangeLog::exec(canonical, "DELETE FROM ops");
        QVERIFY(SpaceReplay::apply(canonical, baseOps));
        const QJsonObject expected = librarySnapshot(canonical);

        // Verify the expected CRDT winners on the canonical state.
        QCOMPARE(canonical.books().size(), 2); // b1 and revived b2; b3 is removed
        const qint64 b1Local = Spaces::localId(canonical, "book", "b:1111111111111111111111111111111111111111111111111111111111111111");
        QVERIFY(b1Local > 0);
        const Book book1 = *canonical.book(b1Local);
        QCOMPARE(book1.meta.title, QString("Title From B"));      // device tie-break at t2
        QCOMPARE(book1.meta.publisher, QString("Press B Later")); // later HLC t3
        QVERIFY(book1.meta.tags.contains("sea"));                 // add wins over concurrent remove at t2
        QCOMPARE(canonical.progress(b1Local).section, 3);         // later progress at t3
        QVERIFY(canonical.wasRemoved("3333333333333333333333333333333333333333333333333333333333333333"));
        QVERIFY(canonical.collectionNamed("Favourites")->included.contains(b1Local));

        const Graph graph(canonical);
        const qint64 mara = graph.named({}, "character", "Mara");
        QVERIFY(mara > 0);
        QVERIFY(graph.node(mara)->aliases().contains("The Pilot"));
        QCOMPARE(graph.firstMet(mara, b1Local)->place.section, 1); // earliest sighting wins
        QCOMPARE(graph.facts(mara).first().revealed.place.section, 1);
        QCOMPARE(graph.readingLog(b1Local).first().from, 10.0);
        QCOMPARE(graph.readingLog(b1Local).first().to, 80.0);

        // Apply 8 random permutations (including reversed and duplicated ops)
        // to independent libraries and verify identical state.
        QRandomGenerator rng(0xC0FFEEU);
        for (int run = 0; run < 8; ++run) {
            QList<SpaceOperation> shuffled = baseOps;
            if (run == 0) {
                std::ranges::reverse(shuffled);
            } else {
                for (qsizetype i = shuffled.size() - 1; i > 0; --i)
                    shuffled.swapItemsAt(i, rng.bounded(int(i + 1)));
                if (run % 2 == 1)
                    shuffled.append(baseOps.mid(0, 10)); // idempotence under duplicate delivery
            }
            Library replica(dir.filePath(QString("Replica-%1").arg(run)));
            QVERIFY(replica.open());
            ChangeLog::exec(replica, "DELETE FROM ops");
            QVERIFY(SpaceReplay::apply(replica, shuffled));
            QCOMPARE(librarySnapshot(replica), expected);
        }
    }
};

QTEST_MAIN(SpacesTests)
#include "spaces_tests.moc"
