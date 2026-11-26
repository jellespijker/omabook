// A book removed while it is read (#229): by the command line, an agent over
// MCP, a sync's own connection to the library, or this window. The reader says
// it was removed and where it went, with the way back to the library, and
// nothing says it did anything to the book.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "marginnotes.h"
#include "mcpharness.h"

namespace {

// The window, a book of three chapters open in it.
struct Bench {
    Desk desk{"en", {}, false};
    qint64 book = 0;

    // Makes the book, and shows the window.
    void stock() {
        const QString file = desk.dir.filePath("The Ferry.epub");
        QVERIFY(Fixtures::chapteredEpub(file, "The Ferry",
                                        {{"One", "<p>The ferry came back to the island.</p>"},
                                         {"Two", "<p>The harbour master counted the crates.</p>"},
                                         {"Three", "<p>Mira watched the lantern.</p>"}}));
        book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
    }
    void open() {
        stock();
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("contentLoader")->property("item").value<QObject *>());
        desk.settle();
    }
    // As a sync does it: through a connection of its own.
    [[nodiscard]] bool removeBesideTheWindow() const {
        Library other(desk.backend->libraryPath());
        return other.open() && other.remove(book).has_value();
    }
    [[nodiscard]] QString said() const { return desk.backend->message(); }
    void invoke(const char *object, const char *method, const QVariant &argument = {}) const {
        QObject *target = desk.find(object);
        QVERIFY(target);
        if (argument.isValid())
            QVERIFY(QMetaObject::invokeMethod(target, method, Q_ARG(QVariant, argument)));
        else
            QVERIFY(QMetaObject::invokeMethod(target, method));
    }
    // The page says the book is gone, and takes the reader back.
    void saysItWasRemoved() {
        QTRY_VERIFY(!desk.reader->isOpen());
        const QString said = desk.reader->error();
        QVERIFY2(said.contains("The Ferry") && said.contains("trash"), qPrintable(said));
        QTRY_VERIFY(desk.shows("readerError"));
        QCOMPARE(desk.find("readerError")->property("text").toString(), said);
        QVERIFY(desk.shows("backToLibrary"));
        QCOMPARE(desk.reader->bookId(), 0);
        desk.reader->close();
        QTRY_VERIFY(!desk.window->property("inReader").toBool());
        QVERIFY(desk.reader->error().isEmpty());
    }
};

} // namespace

class RemovedBookTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void theReaderSaysTheBookWasRemoved_data() {
        QTest::addColumn<QString>("by");
        for (const char *by : {"the command line", "an agent", "a sync", "the window"})
            QTest::newRow(by) << by;
    }
    void theReaderSaysTheBookWasRemoved() {
        QFETCH(QString, by);
        Bench bench;
        bench.open();
        McpHarness::Machine machine;
        machine.library = bench.desk.backend->libraryPath();
        const QString id = QString::number(bench.book);
        if (by == "the command line") {
            const auto ran = machine.omabook({"remove", id, "--library", machine.library});
            QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        } else if (by == "an agent") {
            McpHarness::Server server(machine);
            QCOMPARE(server.data("book_remove", {{"id", bench.book}}).value("removed").toInteger(), bench.book);
        } else if (by == "a sync") {
            QVERIFY(bench.removeBesideTheWindow());
        } else {
            bench.desk.backend->remove(bench.book);
        }
        bench.saysItWasRemoved();
        // The keys of the page do nothing, and claim nothing.
        QTest::keyClick(bench.desk.window, Qt::Key_B);
        QVERIFY2(bench.said() != "Bookmarked", qPrintable(bench.said()));
        QVERIFY(bench.desk.backend->library()->annotations(bench.book).isEmpty());
    }

    // Before the window has heard of it.
    void bookmarkingItClaimsNothing() {
        Bench bench;
        bench.open();
        bench.invoke("readerView", "bookmark");
        QCOMPARE(bench.said(), "Bookmarked");
        bench.desk.backend->dismissMessage();
        QVERIFY(bench.removeBesideTheWindow());
        bench.invoke("readerView", "bookmark");
        QCOMPARE(bench.said(), QString());
        bench.saysItWasRemoved();
    }

    void removingAMarginNoteClaimsNothing() {
        Bench bench;
        bench.open();
        auto *notes = bench.desk.find("readingNotes")->property("notes").value<MarginNotes *>();
        QVERIFY(notes);
        const auto made = notes->write("The ferry came back to the island.", 4, 9, "A boat.");
        QVERIFY(made);
        QVERIFY(bench.removeBesideTheWindow());
        bench.invoke("readingNotes", "removeNote", *made);
        QCOMPARE(bench.said(), QString());
        bench.saysItWasRemoved();
    }

    void goingRoundTheLayoutClaimsNothing() {
        Bench bench;
        bench.open();
        QVERIFY(bench.removeBesideTheWindow());
        QVERIFY(bench.desk.run("reader.layout"));
        QCOMPARE(bench.said(), QString());
        bench.saysItWasRemoved();
    }

    // Leaving it first is leaving it: nothing is written, nothing more is said.
    void closingItWritesNothing() {
        Bench bench;
        bench.open();
        bench.desk.reader->setSection(1);
        QVERIFY(bench.removeBesideTheWindow());
        bench.desk.reader->close();
        QCoreApplication::processEvents();
        QVERIFY(!bench.desk.window->property("inReader").toBool());
        QVERIFY(bench.desk.reader->error().isEmpty());
        QCOMPARE(bench.desk.backend->library()->progress(bench.book).place.section, 0);
        QVERIFY(!bench.desk.backend->library()->book(bench.book));
    }

    void oneRemovedWhileItOpensSaysSo() {
        Bench bench;
        bench.stock();
        bench.desk.reader->openBook(bench.book);
        QVERIFY(bench.desk.reader->loading());
        QVERIFY(bench.removeBesideTheWindow());
        QTRY_VERIFY(!bench.desk.reader->loading());
        bench.saysItWasRemoved();
    }
};

QTEST_MAIN(RemovedBookTests)
#include "removedbook_tests.moc"
