// Syncing two libraries brought up from format 12 (0.1.5), whose baseline is a
// snapshot and whose details have no time (#54): a dry run through the log
// changes nothing on either side, and only details that really differ are
// undecided, counted once per book (#59).

#include <QDateTime>
#include <QDir>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <tuple>

#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "remote.h"
#include "samples.h"
#include "sync.h"

namespace {

// Runs `statements` on a database file, as one from before would have them.
bool change(const QString &file, const QStringList &statements) {
    const QString connection = "change-" + QUuid::createUuid().toString(QUuid::Id128);
    bool done = true;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(file);
        done = database.open();
        QSqlQuery query(database);
        for (const QString &statement : statements)
            done = done && query.exec(statement);
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return done;
}

// Every row of every table of a library's database, table by table, in a
// stable order: what a sync that changes nothing leaves as it was.
QMap<QString, QStringList> rowsOf(const QString &file) {
    const QString connection = "rows-" + QUuid::createUuid().toString(QUuid::Id128);
    QMap<QString, QStringList> tables;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(file);
        if (database.open()) {
            QSqlQuery names(database);
            names.exec("SELECT name FROM sqlite_master WHERE type = 'table' ORDER BY name");
            while (names.next())
                tables.insert(names.value(0).toString(), {});
            for (auto table = tables.begin(); table != tables.end(); ++table) {
                QSqlQuery rows(database);
                rows.exec("SELECT * FROM \"" + table.key() + "\"");
                while (rows.next()) {
                    QStringList values;
                    for (int column = 0; column < rows.record().count(); ++column)
                        values << QString::fromLatin1(rows.value(column).toByteArray().toBase64());
                    *table << values.join('|');
                }
                std::ranges::sort(*table);
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return tables;
}

// The first table whose rows differ, with how many each holds, or nothing.
QString firstChanged(const QMap<QString, QStringList> &before, const QMap<QString, QStringList> &after) {
    for (const QString &table : before.keys() + after.keys())
        if (before.value(table) != after.value(table))
            return QString("%1: %2 rows before, %3 after").arg(table).arg(before.value(table).size()).arg(after.value(table).size());
    return {};
}

// Two copies of the 0.1.5 sample, `Here` and `There`, as two computers had
// them before updating: the same books, with no time to their details.
void loadTwo(const QTemporaryDir &dir) {
    int format = 0;
    for (const char *copy : {"Here", "There"}) {
        QVERIFY(QDir(dir.path()).mkpath(copy));
        Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), dir.filePath(QString(copy) + "/omabook.db"), format);
    }
    QCOMPARE(format, 12);
}

} // namespace

class UpgradedSyncTests : public QObject {
    Q_OBJECT

private slots:
    void aDryRunThroughTheLogChangesNothingOnEitherSide() {
        QTemporaryDir dir;
        loadTwo(dir);
        QVERIFY(Fixtures::epub(dir.filePath("here.epub"), "Only Here") && Fixtures::epub(dir.filePath("there.epub"), "Only There"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        const QString hereFile = dir.filePath("Here/omabook.db");
        const QString thereFile = dir.filePath("There/omabook.db");

        // Before the two ever synced: a dry run pairs nothing, and refuses nothing a sync would take.
        const QMap<QString, QStringList> hereUnpaired = rowsOf(hereFile);
        const QMap<QString, QStringList> thereUnpaired = rowsOf(thereFile);
        const SyncSummary unpaired = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QVERIFY2(unpaired.error.isEmpty() && unpaired.problems.isEmpty(), qPrintable(unpaired.error + unpaired.problems.join('\n')));
        QCOMPARE(unpaired.refused, 0);
        QCOMPARE(firstChanged(hereUnpaired, rowsOf(hereFile)), QString());
        QCOMPARE(firstChanged(thereUnpaired, rowsOf(thereFile)), QString());

        const SyncSummary first = syncLibrary(here, remote);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty(), qPrintable(first.error + first.problems.join('\n')));

        // Then each side reads on, changes details, files a book and gets one the other lacks.
        const qint64 later = QDateTime::currentSecsSinceEpoch() + 60;
        here.setProgress(1, {.section = 3, .fraction = 0.5, .percent = 60, .updated = later});
        BookMeta meta = here.book(3)->meta;
        meta.publisher = "Here Press";
        QVERIFY(here.setMeta(3, meta, nullptr, Hlc::pack(later, 0)));
        QVERIFY(here.add(dir.filePath("here.epub")).id > 0);
        QVERIFY(there.setReading(1, "read", 5, "2026-11-01"));
        QVERIFY(there.addCollection("Lent Out", "rating:5"));
        there.setProgress(3, {.section = 2, .fraction = 0.25, .percent = 30, .updated = later + 10});
        QVERIFY(there.add(dir.filePath("there.epub")).id > 0);

        const QMap<QString, QStringList> hereBefore = rowsOf(hereFile);
        const QMap<QString, QStringList> thereBefore = rowsOf(thereFile);
        const SyncSummary dry = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QVERIFY2(dry.error.isEmpty() && dry.problems.isEmpty(), qPrintable(dry.error + dry.problems.join('\n')));
        // It says what a sync would do, each way.
        QCOMPARE(dry.received, 1);
        QCOMPARE(dry.sent, 1);
        QCOMPARE(dry.placesTaken, 1);
        QCOMPARE(dry.placesSent, 1);
        QCOMPARE(dry.readingTaken, 1);
        QCOMPARE(dry.detailsSent, 1);
        QCOMPARE(dry.collectionsTaken, 1);
        // And nothing of either library changed: no operation, snapshot, mark, row, pairing or space.
        QCOMPARE(firstChanged(hereBefore, rowsOf(hereFile)), QString());
        QCOMPARE(firstChanged(thereBefore, rowsOf(thereFile)), QString());

        // A sync then does what the dry run said.
        const SyncSummary synced = syncLibrary(here, remote);
        QVERIFY2(synced.problems.isEmpty(), qPrintable(synced.problems.join('\n')));
        QCOMPARE(synced.placesTaken, dry.placesTaken);
        QCOMPARE(synced.placesSent, dry.placesSent);
        QCOMPARE(synced.detailsSent, dry.detailsSent);
        QCOMPARE(there.book(3)->meta.publisher, QString("Here Press"));
    }

    void equalDetailsWithNoTimeAreNotUndecidedAndADifferenceCountsOnce() {
        QTemporaryDir dir;
        loadTwo(dir);
        // The same cover on each, saved apart (as a sync before the log saved what it fetched),
        // so its files differ; and one field of one book that really differs, with no time.
        QImage cover(40, 60, QImage::Format_RGB32);
        cover.fill(Qt::darkCyan);
        for (const auto &[copy, quality, publisher] : {std::tuple{"Here", 90, "Here Books"}, std::tuple{"There", 40, "There Books"}}) {
            QVERIFY(QDir(dir.filePath(copy)).mkpath("Ann Author/The Sea Wall (1)"));
            QVERIFY(cover.save(dir.filePath(QString(copy) + "/Ann Author/The Sea Wall (1)/cover.jpg"), "JPG", quality));
            QVERIFY(change(
                dir.filePath(QString(copy) + "/omabook.db"),
                {"UPDATE books SET has_cover = 1 WHERE id = 1", QString("UPDATE books SET publisher = '%1' WHERE id = 2").arg(publisher)}));
        }
        QVERIFY(Fixtures::epub(dir.filePath("new.epub"), "Brought Along"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QVERIFY(Library::hashFile(here.coverPath(1)) != Library::hashFile(there.coverPath(1)));
        // A book that moves, so the log goes once more at the end of the sync.
        QVERIFY(here.add(dir.filePath("new.epub")).id > 0);
        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});

        const SyncSummary first = syncLibrary(here, remote);
        QVERIFY2(first.error.isEmpty() && first.problems.isEmpty(), qPrintable(first.error + first.problems.join('\n')));
        QCOMPARE(first.sent, 1);
        QCOMPARE(first.detailsUndecided, 1);
        QCOMPARE(first.titles.detailsUndecided, QStringList{"A Paper Book"});
        const SyncSummary dry = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QCOMPARE(dry.detailsUndecided, 1);
        QCOMPARE(dry.titles.detailsUndecided, QStringList{"A Paper Book"});
        QCOMPARE(syncLibrary(here, remote).detailsUndecided, 1);

        // Settled by choosing this computer's: the one book's details go, the cover stays as it is.
        const SyncSummary settled = syncLibrary(here, remote, {}, SyncOptions{.details = SyncOptions::Details::Mine});
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(settled.detailsSent, 1);
        QCOMPARE(settled.titles.detailsSent, QStringList{"A Paper Book"});
        QCOMPARE(there.book(2)->meta.publisher, QString("Here Books"));
        const SyncSummary after = syncLibrary(here, remote);
        QCOMPARE(after.detailsUndecided + after.detailsTaken + after.detailsSent, 0);
    }
};

QTEST_MAIN(UpgradedSyncTests)
#include "upgradedsync_tests.moc"
