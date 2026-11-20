// Bookmarks and highlights between one's own computers (#66): one entity of
// the change log, "annotation", whatever their kind, each by a global id.

#include <QDir>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <memory>

#include "annotationanchors.h"
#include "booktext.h"
#include "changelog.h"
#include "document.h"
#include "fixtures.h"
#include "formats.h"
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

    // A highlight travels with its passage (#80): its words, those around
    // them, where it ends, the file it was found in and how surely. Found
    // again in a new edition there, it comes back as it is there; one lost
    // there comes back lost.
    void aHighlightTravelsWithItsPassageAndAsItWasFoundAgain() {
        TwoComputers two;
        const Remote remote = two.remote();
        QVERIFY(syncLibrary(two.here, remote).problems.isEmpty());
        const std::optional<FormatFile> file = AnnotationAnchors::readableFile(two.here, two.mine);
        QVERIFY(file);
        const std::unique_ptr<Document> opened = Formats::open(file->path);
        QVERIFY(opened);
        const BookText text(*opened);
        int section = 0;
        while (section < text.sections() && text.text(section).simplified().size() < 40)
            ++section;
        QVERIFY(section < text.sections());
        const QString words = text.text(section).simplified().split(' ').mid(2, 6).join(' ');
        const qsizetype at = text.text(section).indexOf(words);
        QVERIFY2(at >= 0, qPrintable(words));
        Annotation highlight{.id = 0, .kind = "highlight", .place = {}, .text = words, .note = "Look", .created = 0};
        highlight.setPassage(Passage::of(text.text(section), section, at, at + words.size()));
        highlight.anchored = file->hash;
        QVERIFY(two.here.addAnnotation(two.mine, highlight) > 0);
        Annotation gone = highlight;
        gone.setPassage({.from = {.section = section, .fraction = 0.5},
                         .to = {.section = section, .fraction = 0.6},
                         .quote = "words no edition of it has",
                         .before = {},
                         .after = {}});
        QVERIFY(two.here.addAnnotation(two.mine, gone) > 0);
        QCOMPARE(syncLibrary(two.here, remote).annotationsSent, 2);
        const auto there = [&two](const QString &quote) {
            const QList<Annotation> all = two.there.annotations(two.theirs);
            const auto found = std::ranges::find(all, quote, &Annotation::quote);
            return found == all.cend() ? Annotation{} : *found;
        };
        const Annotation came = there(words);
        QCOMPARE(came.passage(), highlight.passage());
        QCOMPARE(came.anchored, file->hash);
        QCOMPARE(came.confidence, 1.0);
        QVERIFY(!came.lost);

        // Found again there, in a file whose text moved on: the passage as it is
        // there comes back, and the one it lacks comes back lost.
        QStringList sections;
        for (int each = 0; each < text.sections(); ++each)
            sections << (each == section ? "A line put in first. " + text.text(each) : text.text(each));
        const BookText edition(static_cast<int>(sections.size()), [sections](int each) { return sections.value(each); });
        QCOMPARE(AnnotationAnchors::anchor(two.there, two.theirs, edition, "edition"),
                 (AnnotationAnchors::Summary{.kept = 0, .moved = 1, .lost = 1}));
        const SyncSummary back = syncLibrary(two.here, remote);
        QVERIFY2(back.problems.isEmpty(), qPrintable(back.problems.join('\n')));
        QCOMPARE(back.annotationsTaken, 2);
        const QList<Annotation> here = two.here.annotations(two.mine);
        QCOMPARE(here.size(), 2);
        for (const Annotation &annotation : std::as_const(here)) {
            const Annotation theirs = there(annotation.quote);
            QCOMPARE(annotation.passage(), theirs.passage());
            QCOMPARE(annotation.anchored, QString("edition"));
            QCOMPARE(annotation.lost, annotation.quote != words);
            QCOMPARE(annotation.confidence, theirs.confidence);
        }
        QCOMPARE(changed(syncLibrary(two.here, remote)), 0);
    }

    // A margin note (#82) travels as a highlight does, with its passage and
    // its words; changed on either computer it changes on the other, and
    // removed on one it goes from the other.
    void aMarginNoteTravelsIsChangedOnEitherAndRemoved() {
        TwoComputers two;
        const Remote remote = two.remote();
        QVERIFY(syncLibrary(two.here, remote).problems.isEmpty());
        Annotation note{.id = 0, .kind = "margin-note", .place = {}, .text = "the storm", .note = "Read this twice", .created = 0};
        note.setPassage({.from = {.section = 1, .fraction = 0.25},
                         .to = {.section = 1, .fraction = 0.3},
                         .quote = "the storm",
                         .before = "Before ",
                         .after = " came"});
        const qint64 made = two.here.addAnnotation(two.mine, note);
        QVERIFY(made > 0);
        QCOMPARE(syncLibrary(two.here, remote).annotationsSent, 1);
        const QList<Annotation> came = two.there.annotations(two.theirs, "margin-note");
        QCOMPARE(came.size(), 1);
        QCOMPARE(came.first().note, QString("Read this twice"));
        QCOMPARE(came.first().passage(), note.passage());

        Annotation there = came.first();
        there.note = "Read this three times";
        QVERIFY(two.there.storeAnnotation(two.theirs, there));
        QCOMPARE(syncLibrary(two.here, remote).annotationsTaken, 1);
        QCOMPARE(two.here.annotations(two.mine, "margin-note").first().note, QString("Read this three times"));
        QCOMPARE(two.here.annotations(two.mine, "margin-note").first().id, made);

        Annotation here = two.here.annotations(two.mine).first();
        here.note = "Enough";
        QVERIFY(two.here.storeAnnotation(two.mine, here));
        QCOMPARE(syncLibrary(two.here, remote).annotationsSent, 1);
        QCOMPARE(two.there.annotations(two.theirs, "margin-note").first().note, QString("Enough"));

        two.there.removeAnnotation(there.id);
        QCOMPARE(syncLibrary(two.here, remote).annotationsTaken, 1);
        QVERIFY(two.here.annotations(two.mine).isEmpty());
        QCOMPARE(changed(syncLibrary(two.here, remote)), 0);
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
        // A bookmark from before passages ends where it starts (#80).
        QCOMPARE(here.annotations(1).first().to, here.annotations(1).first().place);
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
