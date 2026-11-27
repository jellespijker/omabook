// What an import read from a file never replaces what a person typed (#284):
// the same file added on a second computer, later than the details were
// changed on the first, takes the changed details at the next sync, whichever
// side starts it, also through a computer that only received it. Two files'
// readings that differ, with nobody having typed anything, are left for the
// person to settle.

#include <QDateTime>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "formats.h"
#include "hlc.h"
#include "library.h"
#include "remote.h"
#include "sync.h"

namespace {

// Libraries A, B and C, empty and open, and one file for them to add.
struct Computers {
    QTemporaryDir dir;
    Library a{dir.filePath("A")};
    Library b{dir.filePath("B")};
    Library c{dir.filePath("C")};
    QString file = dir.filePath("salt.epub");

    [[nodiscard]] bool made() { return a.open() && b.open() && c.open() && Fixtures::epub(file, "The Salt Road"); }

    // A sync of `library` with the one called `there`, over the stand-in ssh.
    [[nodiscard]] SyncSummary sync(Library &library, const QString &there, bool dryRun = false) const {
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        SyncOptions options;
        options.dryRun = dryRun;
        return syncLibrary(library, Remote(there.toLower() + ":" + dir.filePath(there), {ssh}), {}, options);
    }
};

[[nodiscard]] QString said(const SyncSummary &summary) {
    return (QStringList{summary.error} + summary.problems + summary.breaches + summary.unannounced).join('\n');
}

[[nodiscard]] bool clean(const SyncSummary &summary) { return said(summary).isEmpty(); }

// The details of a library's only book changed by hand, a minute ago.
[[nodiscard]] bool edit(Library &library) {
    const Book book = library.books().value(0);
    BookMeta meta = book.meta;
    meta.title = "The Salt Road (my title)";
    meta.authors = {"Marit de Vries", "Jo Editor"};
    meta.series = "Coast";
    meta.seriesIndex = 7;
    meta.description = "Mine.";
    meta.publisher = "My Press";
    meta.tags = {"sea"};
    meta.cover = QImage(60, 90, QImage::Format_RGB32);
    meta.cover.fill(Qt::darkGreen);
    return library.setMeta(book.id, meta, nullptr, Hlc::pack(QDateTime::currentSecsSinceEpoch() - 60, 0));
}

[[nodiscard]] QString detailsOf(const Library &library) {
    const Book book = library.books().value(0);
    const QString cover = library.coverPath(book.id);
    return QStringList{
        book.meta.title,       book.meta.authorText(), book.meta.series,         QString::number(book.meta.seriesIndex),
        book.meta.description, book.meta.publisher,    book.meta.tags.join(','), cover.isEmpty() ? QString() : Library::hashFile(cover)}
        .join('|');
}

} // namespace

class ImportDetailsTests : public QObject {
    Q_OBJECT

private slots:
    void aFileAddedAgainElsewhereTakesTheDetailsTypedMeanwhile_data() {
        QTest::addColumn<bool>("fromTheImporter");
        QTest::newRow("synced from the computer that added it again") << true;
        QTest::newRow("synced from the computer that changed it") << false;
    }

    void aFileAddedAgainElsewhereTakesTheDetailsTypedMeanwhile() {
        QFETCH(const bool, fromTheImporter);
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.a.add(computers.file).id > 0 && edit(computers.a));
        const QString typed = detailsOf(computers.a);
        QVERIFY(computers.b.add(computers.file).id > 0);
        QVERIFY(detailsOf(computers.b) != typed);

        Library &from = fromTheImporter ? computers.b : computers.a;
        const QString there = fromTheImporter ? "A" : "B";
        const SyncSummary dry = computers.sync(from, there, true);
        QVERIFY2(clean(dry), qPrintable(said(dry)));
        QCOMPARE(fromTheImporter ? dry.detailsTaken : dry.detailsSent, 1);
        QCOMPARE(fromTheImporter ? dry.detailsSent : dry.detailsTaken, 0);
        const SyncSummary synced = computers.sync(from, there);
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QCOMPARE(detailsOf(computers.a), typed);
        QCOMPARE(detailsOf(computers.b), typed);
        const SyncSummary again = computers.sync(from, there);
        QVERIFY2(clean(again), qPrintable(said(again)));
        QCOMPARE(again.detailsTaken + again.detailsSent + again.detailsUndecided, 0);
    }

    // As through a hub: a computer that got the book of the later import holds nothing typed either.
    void anImportPassedOnReplacesNothingTyped() {
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.a.add(computers.file).id > 0 && edit(computers.a));
        const QString typed = detailsOf(computers.a);
        QVERIFY(computers.b.add(computers.file).id > 0);
        const SyncSummary passed = computers.sync(computers.b, "C");
        QVERIFY2(clean(passed), qPrintable(said(passed)));
        const SyncSummary first = computers.sync(computers.a, "C");
        QVERIFY2(clean(first), qPrintable(said(first)));
        QCOMPARE(detailsOf(computers.a), typed);
        QCOMPARE(detailsOf(computers.c), typed);
        const SyncSummary last = computers.sync(computers.c, "B");
        QVERIFY2(clean(last), qPrintable(said(last)));
        QCOMPARE(detailsOf(computers.b), typed);
        QCOMPARE(detailsOf(computers.a), typed);
    }

    // Field by field: what was typed after the import wins its own field, and nothing of the others.
    void whatEachSideTypedIsKept() {
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.a.add(computers.file).id > 0);
        Book book = computers.a.books().value(0);
        BookMeta meta = book.meta;
        meta.title = "Typed here";
        QVERIFY(computers.a.setMeta(book.id, meta));
        QVERIFY(computers.b.add(computers.file).id > 0);
        book = computers.b.books().value(0);
        meta = book.meta;
        meta.publisher = "Typed there";
        QVERIFY(computers.b.setMeta(book.id, meta));
        const SyncSummary synced = computers.sync(computers.b, "A");
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        for (const Library *library : {&computers.a, &computers.b}) {
            QCOMPARE(library->books().value(0).meta.title, QString("Typed here"));
            QCOMPARE(library->books().value(0).meta.publisher, QString("Typed there"));
        }
    }

    void theSameFileAddedOnBothIsInStep() {
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.a.add(computers.file).id > 0 && computers.b.add(computers.file).id > 0);
        const SyncSummary synced = computers.sync(computers.b, "A");
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QCOMPARE(synced.detailsTaken + synced.detailsSent + synced.detailsUndecided, 0);
    }

    // Two readings of one file that differ (Calibre's details on one side), nothing typed: neither replaces the other.
    void twoImportsThatDifferWaitForThePerson() {
        Computers computers;
        QVERIFY(computers.made());
        QVERIFY(computers.a.add(computers.file).id > 0);
        BookMeta other = Formats::readMeta(computers.file);
        other.title = "As another reading has it";
        QVERIFY(computers.b.add(computers.file, other).id > 0);
        const SyncSummary synced = computers.sync(computers.b, "A");
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QCOMPARE(synced.detailsUndecided, 1);
        QCOMPARE(computers.a.books().value(0).meta.title, QString("The Salt Road"));
        QCOMPARE(computers.b.books().value(0).meta.title, other.title);
    }
};

QTEST_GUILESS_MAIN(ImportDetailsTests)
#include "importdetails_tests.moc"
