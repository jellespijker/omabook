// Bookmarks and highlights between one's own computers (#66): one entity of
// the change log, "annotation", whatever their kind, each by a global id.

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>

#include "changelog.h"
#include "fixtures.h"
#include "importer.h"
#include "library.h"
#include "remote.h"
#include "replication.h"
#include "samples.h"
#include "spaces.h"
#include "sync.h"

namespace {

// A stand-in for ssh in front of Fixtures::standInSsh. As an `older` Omabook
// it refuses the options one from before the log doesn't know, as
// QCommandLineParser does (exit code 1).
QString ssh(const QTemporaryDir &dir, const QString &name, bool older) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    const QByteArray refuse = older ? "case \"$*\" in\n"
                                      "  *\"'--log'\"*) echo \"Unknown option 'log'.\" >&2; exit 1 ;;\n"
                                      "  *\"'--replicate'\"*) echo \"Unknown option 'replicate'.\" >&2; exit 1 ;;\n"
                                      "esac\n"
                                    : "";
    return Fixtures::script(dir.filePath(name), refuse + "exec '" + standIn + "' \"$@\"\n");
}

// The first value `sql` answers of a library, or -1.
qint64 answer(const Library &library, const QString &sql) {
    QSqlQuery query = ChangeLog::query(library, sql);
    return query.next() ? query.value(0).toLongLong() : -1;
}

qint64 annotationOps(const Library &library) { return answer(library, "SELECT COUNT(*) FROM live_ops WHERE entity = 'annotation'"); }

int changed(const SyncSummary &summary) { return summary.annotationsTaken + summary.annotationsSent; }

// Two libraries with the same book, synced once so they are paired.
struct TwoComputers {
    TwoComputers() : here(dir.filePath("Here")), there(dir.filePath("There")) {
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Marked Twice"));
        QVERIFY(here.open() && there.open());
        mine = here.add(dir.filePath("shared.epub")).id;
        theirs = there.add(dir.filePath("shared.epub")).id;
    }

    [[nodiscard]] Remote remote(bool older = false) const {
        return Remote("laptop:" + dir.filePath("There"), {ssh(dir, older ? "ssh-older" : "ssh-log", older)});
    }

    QTemporaryDir dir;
    Library here, there;
    qint64 mine = 0, theirs = 0;
};

} // namespace

class AnnotationSyncTests : public QObject {
    Q_OBJECT

private slots:
    void aBookmarkTravelsIsRenamedAndRemovedAndASecondSyncChangesNothing() {
        TwoComputers two;
        const Remote remote = two.remote();
        QVERIFY(syncLibrary(two.here, remote).problems.isEmpty());

        const qint64 made = two.here.addAnnotation(
            two.mine,
            {.id = 0, .kind = "bookmark", .place = {.section = 2, .fraction = 0.375}, .text = "The storm", .note = {}, .created = 0});
        QVERIFY(made > 0);
        const SyncSummary first = syncLibrary(two.here, remote);
        QVERIFY2(first.problems.isEmpty(), qPrintable(first.problems.join('\n')));
        QCOMPARE(first.annotationsSent, 1);
        const QList<Annotation> there = two.there.annotations(two.theirs);
        QCOMPARE(there.size(), 1);
        QCOMPARE(there.first().kind, QString("bookmark"));
        QCOMPARE(there.first().place.section, 2);
        QCOMPARE(there.first().place.fraction, 0.375);
        QCOMPARE(there.first().text, QString("The storm"));
        QCOMPARE(there.first().created, two.here.annotations(two.mine).first().created);
        // The same one on both, by one id.
        QCOMPARE(Spaces::annotationId(two.there, there.first().id), Spaces::annotationId(two.here, made));

        // Again: nothing to do, nothing written into either log.
        const qint64 opsHere = annotationOps(two.here);
        const qint64 opsThere = annotationOps(two.there);
        QCOMPARE(changed(syncLibrary(two.here, remote)), 0);
        QCOMPARE(annotationOps(two.here), opsHere);
        QCOMPARE(annotationOps(two.there), opsThere);
        QCOMPARE(two.there.annotations(two.theirs).size(), 1);

        // Renamed there: renamed here, still one.
        QVERIFY(two.there.renameAnnotation(there.first().id, "After the storm"));
        const SyncSummary renamed = syncLibrary(two.here, remote);
        QCOMPARE(renamed.annotationsTaken, 1);
        QCOMPARE(two.here.annotations(two.mine).size(), 1);
        QCOMPARE(two.here.annotations(two.mine).first().text, QString("After the storm"));
        QCOMPARE(two.here.annotations(two.mine).first().id, made);

        // Removed here: a tombstone, and gone there too.
        two.here.removeAnnotation(made);
        const SyncSummary removed = syncLibrary(two.here, remote);
        QCOMPARE(removed.annotationsSent, 1);
        QVERIFY(two.there.annotations(two.theirs).isEmpty());
        QCOMPARE(answer(two.there, "SELECT COUNT(*) FROM live_ops WHERE entity = 'annotation' AND field = 'removed' AND value = '1'"),
                 qint64(1));
        QCOMPARE(changed(syncLibrary(two.here, remote)), 0);
        QVERIFY(two.here.annotations(two.mine).isEmpty() && two.there.annotations(two.theirs).isEmpty());
    }

    // Calibre's highlights are annotations as bookmarks are, so they travel
    // the same way, with their notes, once the book they are in has come.
    void aHighlightFromCalibreTravelsAsABookmarkDoes() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::calibreShelf(dir.filePath("Shelf")));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const ImportSummary imported = importBooks(here, {dir.filePath("Shelf")});
        QVERIFY2(imported.problems.isEmpty(), qPrintable(imported.problems.join('\n')));
        const QList<Book> halfHere = here.books("Half Way");
        QCOMPARE(halfHere.size(), 1);
        QCOMPARE(here.annotations(halfHere.first().id).size(), 2);

        const Remote remote("laptop:" + dir.filePath("There"), {ssh(dir, "ssh-log", false)});
        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        const QList<Book> halfThere = there.books("Half Way");
        QCOMPARE(halfThere.size(), 1);
        QStringList kinds;
        for (const Annotation &note : there.annotations(halfThere.first().id))
            kinds << note.kind + ":" + note.text + ":" + note.note;
        kinds.sort();
        QCOMPARE(kinds, (QStringList{"bookmark:Where I stopped:", "highlight:Half Way.:Remember this"}));
        QCOMPARE(changed(syncLibrary(here, remote)), 0);
        QCOMPARE(there.annotations(halfThere.first().id).size(), 2);
    }

    // Two copies of one library (a sample from 0.1.5, with a bookmark) each
    // take their bookmarks into the log as they open, by the same id: synced,
    // each still has the one, not two.
    void copiesOfOneLibraryKeepOneBookmarkEach() {
        QTemporaryDir dir;
        int format = 0;
        for (const char *copy : {"Here", "There"}) {
            QVERIFY(QDir(dir.path()).mkpath(copy));
            Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), dir.filePath(QString(copy) + "/omabook.db"), format);
        }
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QCOMPARE(here.annotations(1).size(), 1);
        QVERIFY(annotationOps(here) > 0);
        QCOMPARE(Spaces::annotationId(here, 1), Spaces::annotationId(there, 1));
        const SyncSummary summary = syncLibrary(here, Remote("laptop:" + dir.filePath("There"), {ssh(dir, "ssh-log", false)}));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(changed(summary), 0);
        QCOMPARE(here.annotations(1).size(), 1);
        QCOMPARE(there.annotations(1).size(), 1);
    }

    // A bookmark written past this Omabook (a tool, a backup put back) is
    // found by its counter on opening and taken into the log.
    void aBookmarkWrittenPastTheLogIsTakenInOnOpening() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "Written Elsewhere"));
        qint64 book = 0;
        {
            Library library(dir.filePath("Library"));
            QVERIFY(library.open());
            book = library.add(dir.filePath("book.epub")).id;
        }
        const QString connection = "past-the-log-" + QUuid::createUuid().toString(QUuid::Id128);
        {
            QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(dir.filePath("Library/omabook.db"));
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec(QString("INSERT INTO annotations (book_id, kind, section, fraction, text, note, created) "
                                       "VALUES (%1, 'bookmark', 1, 0.5, 'By another program', '', 1794700000)")
                                   .arg(book)));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QCOMPARE(answer(library,
                        "SELECT COUNT(*) FROM live_ops WHERE entity = 'annotation' AND field = 'text' AND value = 'By another program'"),
                 qint64(1));
    }

    // An Omabook from before the log syncs by the manifest as it always did,
    // and bookmarks don't go there: nothing of them, nothing refused.
    void anOlderOmabookSyncsAsBeforeWithoutBookmarks() {
        TwoComputers two;
        QVERIFY(two.here.addAnnotation(
                    two.mine,
                    {.id = 0, .kind = "bookmark", .place = {.section = 1, .fraction = 0.5}, .text = "Here only", .note = {}, .created = 0})
                > 0);
        const SyncSummary summary = syncLibrary(two.here, two.remote(true));
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(changed(summary), 0);
        QVERIFY(two.there.annotations(two.theirs).isEmpty());
        QCOMPARE(two.here.annotations(two.mine).size(), 1);
    }

    // An Omabook from 0.1.6 speaks the log but takes no bookmarks, and its
    // vector then says it holds what came after them. Once it says what it
    // holds of them, it is sent each it lacks, whatever its vector says.
    void aComputerThatTookNoBookmarksYetIsSentThemWhenItDoes() {
        TwoComputers two;
        QVERIFY(two.here.addAnnotation(
                    two.mine,
                    {.id = 0, .kind = "bookmark", .place = {.section = 1, .fraction = 0.25}, .text = "Before", .note = {}, .created = 0})
                > 0);
        two.here.setProgress(two.mine, {.place = {.section = 2, .fraction = 0.5}, .percent = 40, .updated = 1794700000});
        const Replication::Vector all = Replication::vector(two.here);
        QVERIFY(Replication::since(two.here, all).isEmpty());
        const QList<SpaceOperation> lacking = Replication::since(two.here, all, {});
        QVERIFY(!lacking.isEmpty());
        QVERIFY(std::ranges::all_of(lacking, [](const SpaceOperation &op) { return op.entity == "annotation"; }));
        QCOMPARE(static_cast<qint64>(lacking.size()), annotationOps(two.here));
        QVERIFY(Replication::since(two.here, all, Replication::annotationVector(two.here)).isEmpty());
    }
};

QTEST_MAIN(AnnotationSyncTests)
#include "annotationsync_tests.moc"
