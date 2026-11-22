// Two writers of one library at once (#161): the window, and a worker with a
// connection of its own (a sync bringing a reading site's shelves in), or
// another program. What one reads in a batch before it writes (the clock, at
// least) can't be changed by the other under it: the other waits for the
// batch, and both writes are kept, never one refused as "database is locked".

#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <chrono>
#include <thread>

#include "bookmeta.h"
#include "library.h"

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
};

QTEST_MAIN(LibraryWritesTests)
#include "librarywrites_tests.moc"
