// The line at the bottom of the window (Toast.qml) goes by itself after the
// time it takes to read it, so that it never lies over a page or the shelf for
// good; what went wrong beside the window waits for the person instead (#233).

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "fixtures.h"
#include "library.h"

namespace {

// Longer than a short message is shown, with room for a slow machine.
constexpr int shortMessageGone = 9000;

} // namespace

class ToastTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aMessageGoesByItself() {
        Desk desk("en", {}, false);
        QVERIFY(desk.show());
        desk.backend->say("Bookmarked");
        QTRY_VERIFY(desk.shows("toast"));
        desk.shot("toast-said");
        QTRY_VERIFY_WITH_TIMEOUT(!desk.shows("toast"), shortMessageGone);
        QVERIFY(desk.backend->message().isEmpty());
        desk.shot("toast-gone");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void saidAgainIsCountedAfresh() {
        Desk desk("en", {}, false);
        QVERIFY(desk.show());
        desk.backend->say("Bookmarked");
        QTest::qWait(3000);
        desk.backend->say("Bookmarked");
        QTest::qWait(2500);
        QVERIFY(desk.shows("toast"));
        QTRY_VERIFY_WITH_TIMEOUT(!desk.shows("toast"), shortMessageGone);
    }

    void aRestingPointerHoldsIt() {
        Desk desk("en", {}, false);
        QVERIFY(desk.show());
        desk.backend->say("Bookmarked");
        QTRY_VERIFY(desk.shows("toast"));
        QQuickItem *toast = desk.item("toast");
        const QPoint on = toast->mapToScene(QPointF(8, toast->height() / 2)).toPoint();
        QTest::mouseMove(desk.window, on);
        QTRY_VERIFY(!desk.find("toastTimer")->property("running").toBool());
        QTest::qWait(shortMessageGone / 2 + 1500);
        QVERIFY(desk.shows("toast"));
        QTest::mouseMove(desk.window, QPoint(4, 4));
        QTRY_VERIFY_WITH_TIMEOUT(!desk.shows("toast"), shortMessageGone);
    }

    void longerWordsStayLonger() {
        Desk desk("en", {}, false);
        desk.backend->say("Bookmarked");
        const int little = desk.backend->messageLasts();
        desk.backend->say(QString(150, 'a'));
        const int more = desk.backend->messageLasts();
        desk.backend->say(QString(5000, 'a'));
        QVERIFY(little >= 4000);
        QVERIFY(more > little);
        QCOMPARE(desk.backend->messageLasts(), 20000);
        desk.backend->dismissMessage();
        QCOMPARE(desk.backend->messageLasts(), 0);
    }

    void whatWentWrongWaitsForThePerson() {
        Desk desk("en", {}, false);
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        QVERIFY(desk.show());
        desk.backend->addUrls({QUrl::fromLocalFile(desk.dir.filePath("one.epub"))});
        QTRY_VERIFY(!desk.backend->message().isEmpty());
        QVERIFY(desk.backend->messageLasts() > 0);
        QVERIFY(!desk.shows("toastClose"));

        desk.backend->addUrls({QUrl::fromLocalFile(desk.dir.filePath("gone.epub"))});
        QTRY_COMPARE(desk.backend->messageLasts(), 0);
        QTRY_VERIFY(desk.shows("toastClose"));
        QVERIFY(!desk.find("toastTimer")->property("running").toBool());
        desk.shot("toast-went-wrong");
        QVERIFY(QMetaObject::invokeMethod(desk.find("toastClose"), "clicked"));
        QVERIFY(desk.backend->message().isEmpty());

        // Opening a book puts it away too: it isn't left over the page.
        desk.backend->sayOutcome("Something went wrong", true);
        QTRY_VERIFY(desk.shows("toast"));
        desk.reader->openBook(desk.backend->library()->books().constFirst().id);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(!desk.shows("toast"));
        QTest::qWait(500); // the page drawn, for the picture
        desk.shot("toast-book-opened");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(ToastTests)
#include "toast_tests.moc"
