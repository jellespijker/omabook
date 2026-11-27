// The library's header keeps Settings and Filter a click or a tap away at
// every width (#268), under fingers as with a mouse: in words where there is
// room, as drawings otherwise, and the other touch buttons give way one at a
// time rather than all at once.
//
// OMABOOK_SHOTS=<folder> saves a picture of each size, to look at by eye.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QtTest>

#include "desk.h"

namespace {

// The one of a thing's two forms that is on screen, inside the window; none
// when neither is, or both are.
QQuickItem *onlyForm(const Desk &desk, const char *words, const char *drawing) {
    QQuickItem *found = nullptr;
    int forms = 0;
    for (const char *name : {words, drawing}) {
        if (!desk.shows(name))
            continue;
        ++forms;
        found = desk.item(name);
    }
    if (forms != 1 || !QRectF(QPointF(), QSizeF(desk.window->size())).contains(found->mapRectToScene(found->boundingRect())))
        return nullptr;
    return found;
}

} // namespace

class HeaderTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void settingsAndFilterStayAtEveryWidth_data() {
        QTest::addColumn<QByteArray>("language");
        QTest::addColumn<bool>("touch");
        for (const char *language : {"en", "de"}) {
            QTest::addRow("%s, a mouse", language) << QByteArray(language) << false;
            QTest::addRow("%s, fingers", language) << QByteArray(language) << true;
        }
    }

    void settingsAndFilterStayAtEveryWidth() {
        QFETCH(const QByteArray, language);
        QFETCH(const bool, touch);
        Desk desk(language, {}, touch);
        QVERIFY(!stock(desk).isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        QPointingDevice *finger = QTest::createTouchDevice();
        const auto press = [&desk, finger, touch](const QQuickItem *item) {
            const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
            if (!touch) {
                QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, at);
                return;
            }
            QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
            QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
        };
        for (const QSize &size : {QSize(2560, 900), QSize(1200, 800), QSize(1100, 800), QSize(960, 1536), QSize(900, 600), QSize(700, 800),
                                  QSize(480, 800), QSize(420, 360)}) {
            QVERIFY(desk.resize(size));
            const QByteArray at = QByteArray::number(size.width()) + 'x' + QByteArray::number(size.height());
            desk.shot(QString("header-%1%2-%3").arg(QString::fromLatin1(language), touch ? "-touch" : "", QString::fromLatin1(at)));
            QQuickItem *settings = onlyForm(desk, "touchSettings", "settingsChoice");
            QVERIFY2(settings, at);
            press(settings);
            QTRY_VERIFY2(desk.find("settings")->property("opened").toBool(), at);
            QVERIFY(desk.closePopups());
            QQuickItem *filter = onlyForm(desk, "touchFilter", "filterLabel");
            QVERIFY2(filter, at);
            press(filter);
            QVERIFY2(desk.waitForPopup(), at);
            QCOMPARE(desk.find("palette")->property("mode").toString(), QString("filter"));
            QVERIFY(desk.closePopups());
        }
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Where the row of touch buttons runs out of room, they give way one at a
    // time: never all of them while one would still fit.
    void theTouchButtonsGiveWayOneAtATime() {
        Desk desk("en", {}, true);
        QVERIFY(!stock(desk).isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        const auto shown = [&desk] {
            int count = 0;
            for (const char *name : {"touchAdd", "touchFilter", "touchSuggest", "touchLibraries", "touchSettings"})
                count += desk.shows(name) ? 1 : 0;
            return count;
        };
        QList<int> seen;
        for (int width = 1600; width >= 420; width -= 10) {
            QVERIFY(desk.resize(QSize(width, 700)));
            const int now = shown();
            QVERIFY2(seen.isEmpty() || now <= seen.constLast(), QByteArray::number(width));
            if (seen.isEmpty() || now != seen.constLast())
                seen << now;
        }
        // All five, then Add, Suggest and Libraries beside the two drawings, then fewer.
        QCOMPARE(seen, (QList<int>{5, 3, 2, 1, 0}));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(HeaderTests)
#include "header_tests.moc"
