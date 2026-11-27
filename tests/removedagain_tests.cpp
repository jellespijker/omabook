// A book removed, synced, and added again travels again (#290): from where it
// was added, or to a computer the removal reached, whichever side starts the
// sync, and through a computer in between. What was removed on one side and
// kept on the other, for being changed or added there since, is named by every
// sync that sees the two differ.

#include <QDateTime>
#include <QTemporaryDir>
#include <QtTest>

#include "compaction.h"
#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "sync.h"

namespace {

// Libraries A, B and H (the one in between, as a hub), empty and open.
struct Computers {
    QTemporaryDir dir;
    Library a{dir.filePath("A")};
    Library b{dir.filePath("B")};
    Library h{dir.filePath("H")};
    QString file = dir.filePath("salt.epub");

    [[nodiscard]] bool made() { return a.open() && b.open() && h.open() && Fixtures::epub(file, "The Salt Harbour"); }

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

// The book of a library, a file's or a paper copy, added.
[[nodiscard]] bool added(Computers &computers, Library &library, bool paper) {
    if (!paper)
        return library.add(computers.file).id > 0;
    BookMeta meta;
    meta.title = "The Salt Harbour";
    meta.authors = {"Ann Author"};
    return library.addEntry(meta, true).id > 0;
}

[[nodiscard]] bool removeOnly(Library &library) {
    return library.books().size() == 1 && library.remove(library.books().first().id).has_value();
}

// Times are kept to the second: what comes after is later.
void aSecondOn() { QTest::qWait(1100); }

// The one book of a library is in its collection "Shelf" by hand.
[[nodiscard]] bool onShelf(const Library &library) {
    const auto shelf = library.collectionNamed("Shelf");
    return shelf && library.books().size() == 1 && shelf->included.contains(library.books().first().id);
}

[[nodiscard]] bool putOnShelf(Library &library) {
    const auto shelf = library.collectionNamed("Shelf");
    return shelf && library.books().size() == 1 && library.putInCollection(shelf->id, library.books().first().id, true);
}

} // namespace

class RemovedAgainTests : public QObject {
    Q_OBJECT

private slots:
    void aBookAddedAgainTravelsAgain_data() {
        QTest::addColumn<bool>("paper");
        QTest::addColumn<bool>("whereRemoved");
        QTest::addColumn<bool>("fromWhereAdded");
        for (const bool paper : {false, true})
            for (const bool whereRemoved : {true, false})
                for (const bool fromWhereAdded : {true, false})
                    QTest::addRow("%s, added again %s, synced from %s", paper ? "on paper" : "a file",
                                  whereRemoved ? "where it was removed" : "where the removal reached",
                                  fromWhereAdded ? "there" : "the other")
                        << paper << whereRemoved << fromWhereAdded;
    }

    void aBookAddedAgainTravelsAgain() {
        QFETCH(const bool, paper);
        QFETCH(const bool, whereRemoved);
        QFETCH(const bool, fromWhereAdded);
        Computers computers;
        QVERIFY(computers.made() && added(computers, computers.a, paper));
        SyncSummary synced = computers.sync(computers.a, "B");
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        aSecondOn();
        QVERIFY(removeOnly(computers.a));
        synced = computers.sync(computers.a, "B");
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QVERIFY(computers.b.books().isEmpty());
        aSecondOn();
        Library &again = whereRemoved ? computers.a : computers.b;
        Library &other = whereRemoved ? computers.b : computers.a;
        QVERIFY(added(computers, again, paper));
        Library &from = fromWhereAdded ? again : other;
        const QString there = &from == &computers.a ? "B" : "A";

        const SyncSummary dry = computers.sync(from, there, true);
        QVERIFY2(clean(dry), qPrintable(said(dry)));
        QCOMPARE(fromWhereAdded ? dry.sent : dry.received, 1);
        QCOMPARE(dry.removedHere + dry.removedThere + dry.keptHere + dry.keptThere, 0);
        synced = computers.sync(from, there);
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QCOMPARE(computers.a.books().size(), 1);
        QCOMPARE(computers.b.books().size(), 1);
        // And stays: nothing takes it away at the next syncs, from either side.
        for (Library *library : {&computers.a, &computers.b, &computers.a}) {
            const SyncSummary next = computers.sync(*library, library == &computers.a ? "B" : "A");
            QVERIFY2(clean(next), qPrintable(said(next)));
            QCOMPARE(next.received + next.sent + next.removedHere + next.removedThere + next.keptHere + next.keptThere, 0);
        }
        QCOMPARE(computers.a.books().size(), 1);
        QCOMPARE(computers.b.books().size(), 1);
        // Removed once more, it is removed on both, as ever.
        aSecondOn();
        QVERIFY(removeOnly(other));
        synced = computers.sync(other, &other == &computers.a ? "B" : "A");
        QVERIFY2(clean(synced), qPrintable(said(synced)));
        QVERIFY(computers.a.books().isEmpty() && computers.b.books().isEmpty());
    }

    void aBookAddedAgainReachesEveryComputerThroughTheOneBetween_data() {
        QTest::addColumn<bool>("paper");
        QTest::newRow("a file") << false;
        QTest::newRow("on paper") << true;
    }

    void aBookAddedAgainReachesEveryComputerThroughTheOneBetween() {
        QFETCH(const bool, paper);
        Computers computers;
        QVERIFY(computers.made() && added(computers, computers.a, paper));
        for (Library *library : {&computers.a, &computers.b}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
        }
        aSecondOn();
        QVERIFY(removeOnly(computers.a));
        for (Library *library : {&computers.a, &computers.b}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
        }
        QVERIFY(computers.h.books().isEmpty() && computers.b.books().isEmpty());
        aSecondOn();
        QVERIFY(added(computers, computers.a, paper));
        for (Library *library : {&computers.a, &computers.b, &computers.a, &computers.b}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
            QCOMPARE(synced.removedHere + synced.removedThere + synced.keptHere + synced.keptThere, 0);
            // That it was removed and added again outlasts compaction, which keeps only the latest of most things.
            Compaction::compact(computers.a);
            Compaction::compact(computers.h);
        }
        QCOMPARE(computers.a.books().size(), 1);
        QCOMPARE(computers.h.books().size(), 1);
        QCOMPARE(computers.b.books().size(), 1);
    }

    // Removed on A, and changed on B before B heard of it: kept there, as documented, and said.
    void aBookKeptOnOneSideIsNamed_data() {
        QTest::addColumn<bool>("removalFirst");
        QTest::addColumn<bool>("read");
        for (const bool removalFirst : {true, false})
            for (const bool read : {true, false})
                QTest::addRow("%s first, %s", removalFirst ? "the removal" : "the change", read ? "read on" : "renamed")
                    << removalFirst << read;
    }

    void aBookKeptOnOneSideIsNamed() {
        QFETCH(const bool, removalFirst);
        QFETCH(const bool, read);
        Computers computers;
        QVERIFY(computers.made() && added(computers, computers.a, false));
        for (Library *library : {&computers.a, &computers.b}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
        }
        aSecondOn();
        QVERIFY(removeOnly(computers.a));
        aSecondOn();
        const Book book = computers.b.books().first();
        if (read) {
            computers.b.setProgress(book.id, {.place = {}, .percent = 40, .updated = QDateTime::currentSecsSinceEpoch()});
        } else {
            BookMeta meta = book.meta;
            meta.title = "Harbour edited later";
            QVERIFY(computers.b.setMeta(book.id, meta));
        }
        if (removalFirst) {
            const SyncSummary removal = computers.sync(computers.a, "H");
            QVERIFY2(clean(removal), qPrintable(said(removal)));
            QCOMPARE(removal.removedThere, 1);
            for (const bool dryRun : {true, false, false}) {
                const SyncSummary synced = computers.sync(computers.b, "H", dryRun);
                QVERIFY2(clean(synced), qPrintable(said(synced)));
                QCOMPARE(synced.keptHere, 1);
                QCOMPARE(synced.titles.keptHere.size(), 1);
                QCOMPARE(synced.keptThere + synced.removedHere + synced.sent, 0);
            }
        } else {
            const SyncSummary change = computers.sync(computers.b, "H");
            QVERIFY2(clean(change), qPrintable(said(change)));
            for (const bool dryRun : {true, false, false}) {
                const SyncSummary synced = computers.sync(computers.a, "H", dryRun);
                QVERIFY2(clean(synced), qPrintable(said(synced)));
                QCOMPARE(synced.keptThere, 1);
                QCOMPARE(synced.titles.keptThere.size(), 1);
                QCOMPARE(synced.keptHere + synced.removedThere + synced.received, 0);
            }
        }
        QVERIFY(computers.a.books().isEmpty());
        QCOMPARE(computers.b.books().size(), 1);
    }

    // Added again and put back in the collection it was in (#300): in it on every computer, and it stays there,
    // though the log still holds the word from before the removal that it is in.
    void aBookAddedAgainAndPutBackInItsCollectionIsInItEverywhere_data() {
        QTest::addColumn<bool>("whereRemoved");
        QTest::newRow("added again where it was removed") << true;
        QTest::newRow("added again where the removal reached") << false;
    }

    void aBookAddedAgainAndPutBackInItsCollectionIsInItEverywhere() {
        QFETCH(const bool, whereRemoved);
        Computers computers;
        QVERIFY(computers.made() && added(computers, computers.a, false));
        QVERIFY(computers.a.addCollection("Shelf") && putOnShelf(computers.a));
        for (Library *library : {&computers.a, &computers.b}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
        }
        QVERIFY(onShelf(computers.b) && onShelf(computers.h));
        aSecondOn();
        QVERIFY(removeOnly(computers.a));
        for (Library *library : {&computers.a, &computers.b}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
        }
        QVERIFY(computers.b.books().isEmpty() && computers.h.books().isEmpty());
        aSecondOn();
        Library &again = whereRemoved ? computers.a : computers.b;
        Library &other = whereRemoved ? computers.b : computers.a;
        QVERIFY(added(computers, again, false) && putOnShelf(again));
        for (Library *library : {&again, &other, &again, &other}) {
            const SyncSummary synced = computers.sync(*library, "H");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
            Compaction::compact(computers.a);
            Compaction::compact(computers.b);
            Compaction::compact(computers.h);
        }
        QVERIFY(onShelf(again));
        QVERIFY(onShelf(computers.h));
        QVERIFY(onShelf(other));
        for (Library *library : {&computers.a, &computers.b}) {
            const SyncSummary settled = computers.sync(*library, "H");
            QVERIFY2(clean(settled), qPrintable(said(settled)));
            QCOMPARE(settled.collectionsTaken + settled.collectionsSent, 0);
        }
        QVERIFY(onShelf(computers.a) && onShelf(computers.b) && onShelf(computers.h));
    }

    // Added after the removal on a computer that never removed it: nothing was added again, so it isn't brought
    // back to where it was removed (README), and the sync says the two differ.
    void aBookAddedUnawareOfTheRemovalStaysThereAndIsNamed() {
        Computers computers;
        QVERIFY(computers.made() && added(computers, computers.a, false));
        aSecondOn();
        QVERIFY(removeOnly(computers.a));
        aSecondOn();
        QVERIFY(added(computers, computers.b, false));
        for (int time = 0; time < 3; ++time) {
            const SyncSummary synced = computers.sync(computers.b, "A");
            QVERIFY2(clean(synced), qPrintable(said(synced)));
            QCOMPARE(synced.keptHere, 1);
            QCOMPARE(synced.sent + synced.removedHere, 0);
            // Read on once the removal is known here: still no book added again.
            computers.b.setProgress(computers.b.books().first().id,
                                    {.place = {}, .percent = 10.0 * (time + 1), .updated = QDateTime::currentSecsSinceEpoch()});
        }
        QVERIFY(computers.a.books().isEmpty());
        QCOMPARE(computers.b.books().size(), 1);
    }
};

QTEST_GUILESS_MAIN(RemovedAgainTests)
#include "removedagain_tests.moc"
