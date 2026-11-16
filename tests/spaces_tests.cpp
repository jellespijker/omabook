// Spaces, global ids, the hybrid logical clock and the change log (ADR 0001).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

#include "fixtures.h"
#include "graph.h"
#include "hlc.h"
#include "library.h"
#include "librarymigrations.h"
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
        }
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
    }
};

QTEST_MAIN(SpacesTests)
#include "spaces_tests.moc"
