// Details that are the same on both computers settle whichever computer
// starts the sync (#168): two libraries with books to read paired at their
// first meeting by their Goodreads id (their global ids differ, ADR 0009),
// whose details are written at different times on each, synced over the
// stand-in ssh. A dry run from either side lists nothing of a book whose
// details are equal, and one whose details differ is listed alike from both.

#include <QDateTime>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "fixtures.h"
#include "hlc.h"
#include "library.h"
#include "remote.h"
#include "spaces.h"
#include "sync.h"

namespace {

const QString isbn = QStringLiteral("9781565048836");

// A book to read from Goodreads, as an import makes one: no file, and what
// the last sync with Goodreads brought.
[[nodiscard]] qint64 toRead(Library &library, const QString &title, const QString &goodreads, const QString &withIsbn = {}) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Ann Author"};
    meta.isbn = withIsbn;
    const qint64 id = library.addEntry(meta, false).id;
    if (id <= 0 || !library.setReading(id, "to-read", 0, {}) || !library.setIdentifier(id, "goodreads", goodreads)
        || !library.setIdentifier(id, "goodreads:synced", "to-read|0|"))
        return 0;
    return id;
}

[[nodiscard]] SyncSummary syncFrom(bool fromA, Library &a, Library &b, const QTemporaryDir &dir, SyncOptions options = {}) {
    const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
    return fromA ? syncLibrary(a, Remote("laptop:" + dir.filePath("B"), {ssh}), {}, options)
                 : syncLibrary(b, Remote("laptop:" + dir.filePath("A"), {ssh}), {}, options);
}

[[nodiscard]] SyncSummary dryRunFrom(bool fromA, Library &a, Library &b, const QTemporaryDir &dir) {
    SyncOptions dry;
    dry.dryRun = true;
    return syncFrom(fromA, a, b, dir, dry);
}

[[nodiscard]] QStringList sorted(QStringList titles) {
    titles.sort();
    return titles;
}

// What a dry run lists of the books' details, as "taken: <title>" and "given: <title>".
[[nodiscard]] QStringList detailsListed(const SyncSummary &summary) {
    QStringList listed;
    for (const QString &title : summary.titles.detailsTaken)
        listed << "taken: " + title;
    for (const QString &title : summary.titles.detailsSent)
        listed << "given: " + title;
    for (const QString &title : summary.titles.detailsUndecided)
        listed << "undecided: " + title;
    for (const QString &rename : summary.titles.renamedHere + summary.titles.renamedThere)
        listed << "renamed: " + rename;
    return sorted(listed);
}

// A dry run from one side says of the books' details what one from the other does, mirrored.
void expectMirrored(const SyncSummary &fromA, const SyncSummary &fromB) {
    for (const SyncSummary *dry : {&fromA, &fromB})
        QVERIFY2(dry->error.isEmpty() && dry->problems.isEmpty(), qPrintable(dry->error + dry->problems.join('\n')));
    QCOMPARE(sorted(fromA.titles.detailsTaken), sorted(fromB.titles.detailsSent));
    QCOMPARE(sorted(fromA.titles.detailsSent), sorted(fromB.titles.detailsTaken));
    QCOMPARE(sorted(fromA.titles.renamedHere), sorted(fromB.titles.renamedThere));
    QCOMPARE(sorted(fromA.titles.renamedThere), sorted(fromB.titles.renamedHere));
    QCOMPARE(sorted(fromA.titles.detailsUndecided), sorted(fromB.titles.detailsUndecided));
}

[[nodiscard]] QImage picture(Qt::GlobalColor colour) {
    QImage image(120, 180, QImage::Format_RGB32);
    image.fill(colour);
    return image;
}

} // namespace

class SettlingTests : public QObject {
    Q_OBJECT

private slots:
    // How the cover each side has came there: none on either, one picture
    // set on each, or one fetched from the other side and saved again here.
    void equalDetailsOfBooksPairedByAnIdentifierAreListedFromNeitherSide_data() {
        QTest::addColumn<QString>("cover");
        QTest::addColumn<bool>("newerHere");
        for (const bool newer : {true, false}) {
            const char *side = newer ? ", written later here" : ", written later there";
            QTest::newRow(qPrintable(QStringLiteral("no cover%1").arg(side))) << "none" << newer;
            QTest::newRow(qPrintable(QStringLiteral("one picture%1").arg(side))) << "same" << newer;
            QTest::newRow(qPrintable(QStringLiteral("a picture saved again%1").arg(side))) << "again" << newer;
            QTest::newRow(qPrintable(QStringLiteral("a cover there only%1").arg(side))) << "there" << newer;
        }
    }

    void equalDetailsOfBooksPairedByAnIdentifierAreListedFromNeitherSide() {
        QFETCH(const QString, cover);
        QFETCH(const bool, newerHere);
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        // There made with its ISBN, here given it afterwards: their ids differ, and the Goodreads id pairs them.
        const qint64 mine = toRead(a, "Clanbook: Ventrue", "1014999");
        const qint64 theirs = toRead(b, "Clanbook: Ventrue", "1014999", isbn);
        QVERIFY(mine > 0 && theirs > 0);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        BookMeta details = b.book(theirs)->meta;
        details.description = "The clan of kings.";
        details.publisher = "White Wolf";
        details.tags = {"Vampire"};
        if (cover == "same" || cover == "again" || cover == "there")
            details.cover = picture(Qt::darkRed);
        QVERIFY(b.setMeta(theirs, details, nullptr, Hlc::pack(now + (newerHere ? 20 : 40), 0)));
        if (cover == "again")
            details.cover = QImage(b.coverPath(theirs));
        if (cover == "there")
            details.cover = QImage();
        QVERIFY(a.setMeta(mine, details, nullptr, Hlc::pack(now + (newerHere ? 40 : 20), 0)));
        QVERIFY(Spaces::bookId(a, mine) != Spaces::bookId(b, theirs));
        QCOMPARE(a.book(mine)->identifiers, b.book(theirs)->identifiers);

        // The first sync pairs them, by their Goodreads id.
        const SyncSummary paired = syncFrom(true, a, b, dir);
        QVERIFY2(paired.error.isEmpty() && paired.problems.isEmpty(), qPrintable(paired.error + paired.problems.join('\n')));
        QCOMPARE(a.books().size(), 1);
        QCOMPARE(b.books().size(), 1);

        const SyncSummary dryA = dryRunFrom(true, a, b, dir);
        const SyncSummary dryB = dryRunFrom(false, a, b, dir);
        QVERIFY2(detailsListed(dryA).isEmpty(), qPrintable(detailsListed(dryA).join('\n')));
        QVERIFY2(detailsListed(dryB).isEmpty(), qPrintable(detailsListed(dryB).join('\n')));
        expectMirrored(dryA, dryB);
    }

    // What differs between the two: their words, or their covers, newer on
    // one side or the other. A dry run from either side lists the book, as
    // one from the other does, mirrored; once synced, neither lists it.
    void differentDetailsAreListedAlikeFromBothSides_data() {
        QTest::addColumn<QString>("differs");
        QTest::addColumn<bool>("newerHere");
        for (const bool newer : {true, false}) {
            const char *side = newer ? ", newer here" : ", newer there";
            QTest::newRow(qPrintable(QStringLiteral("the description%1").arg(side))) << "description" << newer;
            QTest::newRow(qPrintable(QStringLiteral("the cover%1").arg(side))) << "cover" << newer;
        }
    }

    void differentDetailsAreListedAlikeFromBothSides() {
        QFETCH(const QString, differs);
        QFETCH(const bool, newerHere);
        QTemporaryDir dir;
        Library a(dir.filePath("A"));
        Library b(dir.filePath("B"));
        QVERIFY(a.open() && b.open());
        const qint64 mine = toRead(a, "Clanbook: Tremere", "847425");
        const qint64 theirs = toRead(b, "Clanbook: Tremere", "847425", isbn);
        QVERIFY(mine > 0 && theirs > 0);
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        BookMeta details = b.book(theirs)->meta;
        details.description = "The clan of wizards.";
        details.cover = picture(Qt::darkBlue);
        QVERIFY(b.setMeta(theirs, details, nullptr, Hlc::pack(now + 10, 0)));
        QVERIFY(a.setMeta(mine, details, nullptr, Hlc::pack(now + 10, 0)));
        QVERIFY(syncFrom(true, a, b, dir).problems.isEmpty());
        const QStringList first = detailsListed(dryRunFrom(true, a, b, dir));
        QVERIFY2(first.isEmpty(), qPrintable(first.join('\n')));

        // Then changed on one side, later.
        Library &changed = newerHere ? a : b;
        const qint64 id = newerHere ? mine : theirs;
        BookMeta later = changed.book(id)->meta;
        if (differs == "description")
            later.description = "The clan of the pyramid.";
        else
            later.cover = picture(Qt::darkGreen);
        QVERIFY(changed.setMeta(id, later, nullptr, Hlc::pack(now + 60, 0)));

        const SyncSummary dryA = dryRunFrom(true, a, b, dir);
        const SyncSummary dryB = dryRunFrom(false, a, b, dir);
        expectMirrored(dryA, dryB);
        const QStringList listed{QString(newerHere ? "given: " : "taken: ") + "Clanbook: Tremere"};
        QCOMPARE(detailsListed(dryA), listed);
        for (const bool side : {true, false}) {
            const SyncSummary synced = syncFrom(side, a, b, dir);
            QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty(), qPrintable(synced.error + synced.problems.join('\n')));
        }
        for (const bool side : {true, false}) {
            const QStringList settled = detailsListed(dryRunFrom(side, a, b, dir));
            QVERIFY2(settled.isEmpty(), qPrintable(settled.join('\n')));
        }
        if (differs == "description")
            QCOMPARE(a.book(mine)->meta.description, QString("The clan of the pyramid."));
    }
};

QTEST_MAIN(SettlingTests)
#include "settling_tests.moc"
