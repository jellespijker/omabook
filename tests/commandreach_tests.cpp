// Omabook is the keyboard's first, and nothing is the keyboard's alone:
// every command in the commands table, and every command a plugin adds, has
// a control a mouse can click and a finger can tap, somewhere on the shelf,
// a book, the page or the graph, or in the list a ⋯ there opens. A control
// says which commands it is the way to with `runs` (see AppButton.qml); an
// entry of a list says it of the one it runs. Under fingers each is a
// fingertip on its smaller side.
//
// A new command fails here until it has one. There is no command that is
// the keyboard's by nature; one that ever is goes in `keyboardOnly` with the
// reason. (Out of layout_tests.cpp, which had reached its size, #151.)

#include <QPointingDevice>
#include <QQuickStyle>
#include <QtTest>
#include <functional>

#include "desk.h"

class CommandReachTests : public QObject {
    Q_OBJECT

    // The window being walked, under fingers or a mouse, and the largest of
    // the smaller sides of the controls found for each command.
    Desk *m_desk = nullptr;
    bool m_touch = false;
    QHash<QString, qreal> m_reached;
    QPointingDevice *m_finger = nullptr;

    [[nodiscard]] QString mode() const { return m_touch ? "touch" : "mouse"; }

    void collect(QQuickItem *item) {
        if (!item->isVisible() || item->opacity() <= 0.01)
            return;
        for (const QString &id : item->property("runs").toString().split(' ', Qt::SkipEmptyParts))
            m_reached[id] = std::max(m_reached.value(id), std::min(item->width(), item->height()));
        for (QQuickItem *child : item->childItems())
            collect(child);
    }
    void gather(const QString &state) {
        m_desk->settle();
        collect(m_desk->window->contentItem());
        m_desk->shot("commands-" + mode() + "-" + state);
    }
    void press(const QPoint &at, Qt::MouseButton button = Qt::LeftButton) {
        if (m_touch && button == Qt::LeftButton) {
            QTest::touchEvent(m_desk->window, m_finger).press(0, at, m_desk->window);
            QTest::touchEvent(m_desk->window, m_finger).release(0, at, m_desk->window);
        } else {
            QTest::mouseClick(m_desk->window, button, Qt::NoModifier, at);
        }
    }
    void pressItem(const QString &name) {
        QQuickItem *item = m_desk->item(name);
        QVERIFY2(item && m_desk->shows(name), qPrintable(mode() + ": " + name + " is not on screen"));
        press(item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    void inMenu(const QString &opener, const QString &state) {
        pressItem(opener);
        QVERIFY2(m_desk->waitForPopup(), qPrintable(mode() + ": " + opener + " opened nothing"));
        QVERIFY(m_desk->find("commandMenu")->property("opened").toBool());
        gather(state);
        QVERIFY(m_desk->closePopups());
    }

    // The shelf, a book selected and its details beside it, what can be done
    // with a book (a right click on it, a long press for a finger: touch_tests,
    // and the details' "More"), and a book the search found by what is known of it.
    void shelf() {
        Desk &desk = *m_desk;
        QTRY_VERIFY(desk.shows("details"));
        gather("shelf");
        inMenu("libraryMore", "library-menu");
        if (QTest::currentTestFailed())
            return;
        if (!m_touch) {
            QQuickItem *grid = desk.item("grid");
            QVERIFY(grid);
            press(grid->mapToScene(QPointF(grid->property("cellWidth").toReal() / 2, 120)).toPoint(), Qt::RightButton);
            QVERIFY2(desk.waitForPopup(), "a right click on a book opened nothing");
            QCOMPARE(desk.find("commandMenu")->property("kind").toString(), QString("book"));
            gather("book-menu");
            QVERIFY(desk.closePopups());
        }
        inMenu("bookMore", "book-more");
        if (QTest::currentTestFailed())
            return;
        BookListModel *books = desk.backend->books();
        books->setQuery("overbeck");
        QTRY_COMPARE(books->count(), 1);
        QTRY_VERIFY(desk.shows("detailsFoundBy"));
        gather("found");
        books->setQuery({});
        recording();
        desk.backend->setView("stacks");
        QTRY_VERIFY(desk.find("shelfLoader")->property("status").toInt() == 1);
        gather("stacks");
        desk.backend->setView("covers");
        QTRY_VERIFY(desk.find("shelfLoader")->property("status").toInt() == 1);
    }

    // A recording (#151): its details write it out.
    void recording() {
        Desk &desk = *m_desk;
        const QString file = desk.dir.filePath("Harbour Talk - Mira Okafor.wav");
        // A WAV of 16 kHz mono with two samples of silence.
        QVERIFY(Fixtures::write(file, QByteArray::fromHex("524946462800000057415645666d7420100000000100010080"
                                                          "3e0000007d0000020010006461746104000000")
                                          + QByteArray(4, '\0')));
        const qint64 talk = desk.backend->library()->add(file).id;
        QVERIFY(talk > 0);
        desk.backend->reload();
        // Selected once the shelf has it, which the reload brings a moment later.
        const auto selected = [&desk, talk] {
            desk.call("selectOnShelf", talk);
            return desk.find("library")->property("selectedId").toLongLong() == talk;
        };
        QTRY_VERIFY(selected());
        QTRY_VERIFY(desk.shows("transcribeButton"));
        gather("recording");
    }

    // The graph of a book, a thing in it selected; the book's chronology, the
    // event selected: its card, and the list behind each one's ⋯.
    void graphAndChronology(qint64 book, qint64 storm) {
        Desk &desk = *m_desk;
        desk.call("showGraph", book);
        QTRY_VERIFY(desk.shows("libraryGraph"));
        QObject *browser = desk.find("libraryGraph")->property("browser").value<QObject *>();
        QVERIFY(browser);
        QTRY_VERIFY(!browser->property("rows").toList().isEmpty());
        const QVariantList rows = browser->property("rows").toList();
        QMetaObject::invokeMethod(browser, "select", Q_ARG(qint64, rows.first().toMap().value("id").toLongLong()));
        gather("graph");
        inMenu("graphMore", "graph-menu");
        if (QTest::currentTestFailed())
            return;
        QMetaObject::invokeMethod(desk.find("libraryGraph"), "done");

        QMetaObject::invokeMethod(desk.item("chronologyLayer"), "show", Q_ARG(QVariant, "book"), Q_ARG(QVariant, book));
        QTRY_VERIFY(desk.shows("chronologyCanvas"));
        QMetaObject::invokeMethod(desk.find("chronology")->property("browser").value<QObject *>(), "select", Q_ARG(qint64, storm));
        QTRY_VERIFY(desk.shows("chronologyCard"));
        gather("chronology");
        inMenu("chronologyMore", "chronology-menu");
        if (QTest::currentTestFailed())
            return;
        QMetaObject::invokeMethod(desk.find("chronology"), "done");
    }

    // The plugin's view in place of the shelf, and the list behind its ⋯; the
    // open book, read aloud with its control over the page, the bar a finger
    // brings up, and the list behind its "More" or the ⋯ in the corner.
    void viewAndPage(qint64 book) {
        Desk &desk = *m_desk;
        QVERIFY(desk.run("library.shelver:shelf"));
        QTRY_VERIFY(desk.shows("pluginViewFull"));
        gather("view");
        inMenu("pluginViewMore", "view-menu");
        QVERIFY(desk.run("view.close"));
        QTRY_VERIFY(desk.shows("library"));

        QVERIFY(readingAloud(desk, book));
        gather("page");
        if (m_touch) {
            pressItem("menuButton");
            QTRY_VERIFY(desk.shows("readerBar"));
            gather("bar");
            inMenu("barMore", "reader-menu");
        } else {
            QVERIFY(!desk.shows("menuButton"));
            inMenu("readerMore", "reader-menu");
        }
        if (QTest::currentTestFailed())
            return;
        desk.reader->close();
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void everyCommandIsThereForAMouseAndAFinger() {
        static const QHash<QString, QString> keyboardOnly{};
        for (const bool touch : {false, true}) {
            Desk desk("en", {}, touch);
            const QList<qint64> ids = stock(desk);
            QVERIFY(!ids.isEmpty());
            QVERIFY(desk.backend->library()->addCollection("Harbour books", "tag:Fantasy"));
            const qint64 storm = forEveryCommand(desk, ids.first());
            QVERIFY(storm > 0);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.backend->reload();
            QVERIFY(desk.resize(QSize(1180, 780)));
            desk.quicken();
            m_desk = &desk;
            m_touch = touch;
            m_reached.clear();
            m_finger = QTest::createTouchDevice();

            shelf();
            if (QTest::currentTestFailed())
                return;
            graphAndChronology(ids.first(), storm);
            if (QTest::currentTestFailed())
                return;
            viewAndPage(ids.first());
            if (QTest::currentTestFailed())
                return;

            const QStringList problems = unreached(desk, m_reached, keyboardOnly);
            QVERIFY2(problems.isEmpty(), qPrintable(mode() + ": " + problems.join("; ")));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
            m_desk = nullptr;
        }
    }
};

QTEST_MAIN(CommandReachTests)
#include "commandreach_tests.moc"
