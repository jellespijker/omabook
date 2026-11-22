// The pages of a PDF in the window (PageReader.qml), as they are laid out
// anew: for another layout, or two side by side once the pages have been
// counted, which on a busy machine comes after the reader has begun to read.

#include <QtTest>

#include "desk.h"

namespace {

// The window with a PDF of three pages open, laid out and drawn.
struct Bench {
    Desk desk{"en", {}, false};

    Bench() {
        const QString path = desk.dir.filePath("paper.pdf");
        QVERIFY(Fixtures::pdf(path, "A Paper", {"First page", "Second page", "Third page"}));
        const qint64 book = desk.backend->library()->add(path).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(pages() != nullptr && pages()->property("pageCount").toInt() == 3);
        QTRY_VERIFY(pages()->property("settled").toBool());
    }
    [[nodiscard]] QObject *pages() const { return desk.find("pageReader"); }
    void key(Qt::Key key) const { QTest::keyClick(desk.window, key); }
};

} // namespace

class PageReaderTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // A page turned while the pages are laid out anew stays turned: keeping
    // the place through the change doesn't put the one before back.
    void aPageTurnedAsThePagesAreLaidOutAnewStaysTurned() {
        Bench bench;
        QCOMPARE(bench.desk.reader->section(), 0);
        bench.desk.settings->setOption("layout", "pages");
        QVERIFY(bench.pages()->property("turnsPages").toBool());
        // Before the new rows stand: the key's own events run them.
        bench.key(Qt::Key_End);
        QCOMPARE(bench.desk.reader->section(), 2);
        QTRY_VERIFY(bench.pages()->property("settled").toBool());
        bench.desk.settle();
        QCOMPARE(bench.desk.reader->section(), 2);
        QCOMPARE(bench.desk.find("readerPlace")->property("text").toString(), QString("3 of 3"));

        // And with nothing turned, the place is the one it was.
        bench.desk.settings->setOption("layout", "scroll");
        QTRY_VERIFY(bench.pages()->property("settled").toBool());
        bench.desk.settle();
        QCOMPARE(bench.desk.reader->section(), 2);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }
};

QTEST_MAIN(PageReaderTests)
#include "pagereader_tests.moc"
