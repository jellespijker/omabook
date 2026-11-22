// Two writers of one library at once (#161): the window, and a worker with a
// connection of its own (a sync bringing a reading site's shelves in), or
// another program. What one reads in a batch before it writes (the clock, at
// least) can't be changed by the other under it: the other waits for the
// batch, and both writes are kept, never one refused as "database is locked".

#include <QElapsedTimer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <latch>
#include <thread>

#include "bookmeta.h"
#include "fixtures.h"
#include "library.h"
#include "librarybatch.h"
#include "readinggoal.h"

using namespace std::chrono_literals;

namespace {

[[nodiscard]] qint64 entry(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    return library.addEntry(meta, true).id;
}

} // namespace

class LibraryWritesTests : public QObject {
    Q_OBJECT

private slots:
    void aBatchAndAnotherWriterBothWrite() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        Library window(root);
        QVERIFY(window.open());
        const qint64 one = entry(window, "One Day");
        const qint64 two = entry(window, "Two Days");
        QVERIFY(one > 0 && two > 0);

        std::atomic<bool> theirs = false;
        std::jthread worker;
        {
            const Library::Batch batch(window);
            QVERIFY(window.book(one)); // read, as a batch does before it writes
            // The other writes now, while the batch is between its read and its write.
            worker = std::jthread([&root, two, &theirs] {
                Library other(root);
                theirs = other.open() && other.setReading(two, "read", 3, "2024-03-09");
            });
            std::this_thread::sleep_for(300ms);
            QVERIFY(window.setIdentifier(one, "goodreads", "501"));
        }
        worker.join();
        QVERIFY(theirs);
        QCOMPARE(window.book(one)->identifiers.value("goodreads").toString(), QString("501"));
        QCOMPARE(window.book(two)->shelf, QString("read"));
        QCOMPARE(window.book(two)->rating, 3);
    }
    // One writing batch after batch on a connection of its own (a sync's
    // worker bringing many books in) doesn't keep another writer out for
    // good (#200): SQLite's own wait only tries again now and then, and could
    // miss every moment between two batches, so the other was refused as
    // "library busy". The other is let in once the batch it waited for is done.
    void aWriterGetsInBetweenTheBatchesOfAnother() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        Library window(root);
        QVERIFY(window.open());
        const qint64 theirs = entry(window, "Two Days");
        QVERIFY(theirs > 0);

        std::atomic<bool> done = false;
        std::atomic<int> batches = 0;
        const std::jthread worker([&root, theirs, &done, &batches] {
            Library other(root);
            if (!other.open())
                return;
            for (int at = 0; !done; ++at) {
                const Library::Batch batch(other);
                std::ignore = other.setIdentifier(theirs, "goodreads", QString::number(at));
                ++batches;
                std::this_thread::sleep_for(20ms);
            }
        });
        const auto stop = qScopeGuard([&done] { done = true; });
        QTRY_VERIFY(batches.load() > 0);
        for (int target = 1; target <= 10; ++target) {
            const auto set = ReadingGoal::set(window, {.year = 2026, .kind = ReadingGoal::Kind::Books, .target = target});
            QVERIFY2(set, qPrintable(Fixtures::whyNot(set)));
        }
        QCOMPARE(ReadingGoal::of(window, 2026).target, 10);
    }
    // A writer that waited longer than a batch can take gives up and says
    // so, rather than wait for good or write anyway (#200).
    void aWriterWaitingTooLongGivesUp() {
        std::latch held(1);
        std::latch done(1);
        const std::jthread holder([&held, &done] {
            const WriteTurn turn("/library/omabook.db", "holder");
            held.count_down();
            done.wait();
        });
        held.wait();
        const auto stop = qScopeGuard([&done] { done.count_down(); });
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("for longer than 0.1 seconds; this write is given up"));
        const WriteTurn late("/library/omabook.db", "late", 100ms);
        QVERIFY(!late.had());
        QVERIFY(WriteTurn("/other/omabook.db", "late", 100ms).had()); // another library's line is its own
    }

    // One thread writing a library through a second connection while its
    // first holds a batch would only ever wait for itself: it is refused at
    // once, never a hang (#200). Through the first, a batch inside a batch.
    void aSecondConnectionOnTheSameThreadIsRefusedAtOnce() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        Library first(root);
        Library second(root);
        QVERIFY(first.open() && second.open());
        const qint64 one = entry(first, "One Day");
        QVERIFY(one > 0);
        const Library::Batch batch(first);
        QVERIFY(batch.begun());
        QVERIFY(Library::Batch(first).begun());
        QElapsedTimer took;
        took.start();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression("through two connections at once on one thread"));
        QVERIFY(!Library::Batch(second).begun());
        QVERIFY2(took.elapsed() < 5000, "refused at once, not after a wait");
        QVERIFY(first.setIdentifier(one, "goodreads", "7"));
    }
};

QTEST_MAIN(LibraryWritesTests)
#include "librarywrites_tests.moc"
