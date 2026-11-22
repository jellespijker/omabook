// The chronology in the window (#44): opened from the shelf, the graph and
// the page; a tap on an event shows its card, which goes back to the page
// it was revealed on and to it in the graph; and all of it at every size,
// in German, right to left and under fingers, as the layout tests look at
// the rest of the window (desk.h).
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QtTest>

#include "chronology.h"
#include "chronologybranch.h"
#include "desk.h"
#include "eventtime.h"
#include "graph.h"

namespace {

[[nodiscard]] EventTime timeOf(const QString &json) {
    const auto time = EventTime::parse(json);
    if (!time)
        qFatal("%s", qPrintable(time.error()));
    return *time;
}

// What is known of when the events of a book happen, as hard on a layout
// as it gets: long names, a span that runs on, one during another, one
// only in order, one put on another calendar, one with no time at all, and
// two that contradict each other; a branch of real history taken, and one
// proposed. The book is read to its fourth chapter.
struct Story {
    qint64 book = 0, siege = 0, sortie = 0, flood = 0, unplaced = 0, keeper = 0;
};

[[nodiscard]] Story tell(Desk &desk, qint64 book) {
    Story story;
    story.book = book;
    Library &library = *desk.backend->library();
    library.setReading(book, "reading", 0, {});
    library.setProgress(book, {3, 0.5, 60, 0});
    Graph graph(library);
    const auto at = [book](int section, double fraction) { return GraphPlace{book, section, fraction}; };
    const qint64 history = Chronology::timeline(graph, Chronology::Whose::History, "Real history", GraphMoment{at(0, 0.1), {}});
    const qint64 harbour =
        Chronology::timeline(graph, Chronology::Whose::World, "The Harbour Chronicle of the Lantern Keepers", GraphMoment{at(0, 0.1), {}});
    const auto event = [&](const QString &name, qint64 timeline, const GraphPlace &place) {
        const qint64 id =
            graph.upsert({}, "event", name, "What the book says of it, at some length, as books do.", {}, place, name.left(12));
        std::ignore = Chronology::placeOn(graph, id, timeline, GraphMoment{place, {}}); // checked by what the tests see of it
        return id;
    };
    story.siege = event("The Long and Terrible Siege of the Northern Salt Road Harbour", history, at(0, 0.2));
    std::ignore = Chronology::setWhen(graph, story.siege, timeOf(R"({"calendar": "gregorian", "from": "1941-09-08", "approximate": true})"),
                                      GraphMoment{at(0, 0.2), "The Long and"});
    story.sortie = event("The sortie at dawn", history, at(1, 0.4));
    std::ignore = Chronology::order(graph, story.sortie, "during", story.siege, GraphMoment{at(1, 0.4), {}});
    std::ignore = Chronology::setWhen(
        graph, story.sortie, timeOf(R"({"calendar": "gregorian", "at": "1942-03-01T05:30", "said": "the dawn of the first of March"})"),
        GraphMoment{at(1, 0.4), "The sortie a"});
    story.flood = event("The flood", history, at(2, 0.1));
    std::ignore =
        Chronology::setWhen(graph, story.flood, timeOf(R"({"calendar": "gregorian", "at": "1944"})"), GraphMoment{at(2, 0.1), "The flood"});
    // Two that each say the other came first.
    const qint64 lantern = event("The lantern is lit", harbour, at(1, 0.6));
    const qint64 bells = event("The bells ring", harbour, at(2, 0.4));
    std::ignore = Chronology::order(graph, lantern, "before", bells, GraphMoment{at(1, 0.6), {}});
    std::ignore = Chronology::order(graph, bells, "before", lantern, GraphMoment{at(2, 0.4), {}});
    const qint64 tide = event("The tide turns", harbour, at(2, 0.5));
    std::ignore = Chronology::setWhen(graph, tide, timeOf(R"({"calendar": "Third Age of the Harbour", "at": "301-04-01"})"),
                                      GraphMoment{at(2, 0.5), "The tide tur"});
    // A branch of real history the reader took, drawn from the siege on, and
    // one the AI proposes, asked about over the lanes (#171).
    const qint64 held = Chronology::branch(graph, {.timeline = 0,
                                                   .name = "The harbour that held: an alternate history of the long siege",
                                                   .from = history,
                                                   .at = story.siege,
                                                   .revealed = GraphMoment{at(1, 0.2), {}},
                                                   .proposed = false})
                            .value_or(0);
    const qint64 lit = event("The lanterns never go out", held, at(1, 0.3));
    std::ignore = Chronology::setWhen(graph, lit, timeOf(R"({"calendar": "gregorian", "at": "1943"})"), GraphMoment{at(1, 0.3), {}});
    std::ignore = Chronology::branch(graph, {.timeline = 0,
                                             .name = "Alternate history from the sortie at dawn, in which the keepers win the war",
                                             .from = history,
                                             .at = story.sortie,
                                             .revealed = GraphMoment{at(2, 0.2), {}},
                                             .proposed = true});
    story.unplaced = graph.upsert({}, "event", "A rumour that nobody can place in time", {}, {}, at(1, 0.9), "A rumour");
    story.keeper = graph.upsert({}, "character", "Wilhelmina Overbeck-Saltonstall", {}, {}, at(0, 0.1), "Wilhelmina");
    graph.link(story.sortie, story.keeper, "led by", {}, at(1, 0.4));
    return story;
}

// A click or a tap in the middle of something that is on screen.
[[nodiscard]] QPoint middle(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

} // namespace

class ChronologyWindowTests : public QObject {
    Q_OBJECT

    // Every problem found, by where it was found: "de 480x800 chronology".
    QStringList found;
    int looked = 0;

    void look(Desk &desk, const QString &state) {
        desk.settle(1);
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        ++looked;
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        QSet<QString> once;
        for (const QString &problem : std::as_const(survey.problems))
            if (!once.contains(problem)) {
                once.insert(problem);
                found << where + " — " + problem;
            }
        desk.shot(QString("chronology-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }
    void verdict() {
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }

    static QObject *chronology(Desk &desk) {
        QQuickItem *pane = desk.item("chronology");
        return pane ? pane->property("browser").value<QObject *>() : nullptr;
    }
    static bool shown(Desk &desk) { return desk.shows("chronologyCanvas"); }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void itOpensFromTheShelfTheGraphAndThePage() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty());
        const Story story = tell(desk, ids.first());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        // From the shelf, of the selected book.
        QMetaObject::invokeMethod(desk.item("library"), "select", Q_ARG(QVariant, story.book));
        QTRY_COMPARE(desk.item("library")->property("selectedId").toLongLong(), story.book);
        QVERIFY(desk.run("library.chronology_book"));
        QTRY_VERIFY(shown(desk));
        QCOMPARE(chronology(desk)->property("showing").toString(), QString("book"));
        QCOMPARE(chronology(desk)->property("eventCount").toInt(), 7);
        QVERIFY(!desk.shows("library"));
        // Escape goes back to the shelf.
        QVERIFY(desk.run("chronology.close"));
        QTRY_VERIFY(desk.shows("library"));
        QVERIFY(!shown(desk));
        // From the graph: of what it shows, a book's here.
        desk.call("showGraph", story.book);
        QTRY_VERIFY(desk.shows("libraryGraph"));
        QVERIFY(desk.run("graph.chronology"));
        QTRY_VERIFY(shown(desk));
        QCOMPARE(chronology(desk)->property("showing").toString(), QString("book"));
        QVERIFY(!desk.shows("libraryGraph"));
        QVERIFY(desk.run("chronology.close"));
        QTRY_VERIFY(desk.shows("libraryGraph"));
        QMetaObject::invokeMethod(desk.find("libraryGraph"), "done");
        // From the page, of the book that is open.
        desk.reader->openBook(story.book);
        QTRY_VERIFY(desk.reader->isOpen());
        QVERIFY(desk.run("reader.chronology"));
        QTRY_VERIFY(shown(desk));
        QVERIFY(!desk.shows("readerView"));
        QVERIFY(desk.run("chronology.close"));
        QTRY_VERIFY(desk.shows("readerView"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aTapShowsTheCardThatGoesBackToThePageAndToTheGraph() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        const Story story = tell(desk, ids.first());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        QMetaObject::invokeMethod(desk.item("chronologyLayer"), "show", Q_ARG(QVariant, "book"), Q_ARG(QVariant, story.book));
        QTRY_VERIFY(shown(desk));
        desk.settle();
        // A click on the flood's mark.
        QQuickItem *canvas = desk.item("chronologyCanvas");
        QPointF at;
        QMetaObject::invokeMethod(canvas, "pointOf", Q_RETURN_ARG(QPointF, at), Q_ARG(qint64, story.flood));
        QVERIFY(!at.isNull());
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, canvas->mapToScene(at).toPoint());
        QTRY_COMPARE(chronology(desk)->property("selected").toLongLong(), story.flood);
        QTRY_VERIFY(desk.shows("chronologyCard"));
        QTRY_COMPARE(desk.item("chronologyCardName")->property("text").toString(), QString("The flood"));
        QCOMPARE(desk.item("chronologyCardWhen")->property("text").toString(), QString("1944"));
        // Back to the page it was revealed on: the third chapter.
        desk.settle();
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(desk.item("chronologyBackToPage")));
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->bookId(), story.book);
        QTRY_COMPARE(desk.reader->section(), 2);
        QTRY_VERIFY(!shown(desk));
        // Again from the page, the keys this time: to the sortie, and to it in the graph beside the page.
        QVERIFY(desk.run("reader.chronology"));
        QTRY_VERIFY(shown(desk));
        QMetaObject::invokeMethod(chronology(desk), "select", Q_ARG(qint64, story.sortie));
        QTRY_VERIFY(desk.shows("chronologyCard"));
        QVERIFY(desk.run("chronology.graph"));
        QTRY_VERIFY(!shown(desk));
        QTRY_VERIFY(desk.find("readerView")->property("showGraph").toBool());
        // The keys step along a lane, and the filter by who an event involves.
        QVERIFY(desk.run("reader.chronology"));
        QTRY_VERIFY(shown(desk));
        QObject *browser = chronology(desk);
        QMetaObject::invokeMethod(browser, "setFilter", Q_ARG(QString, "involved"), Q_ARG(QVariant, story.keeper), Q_ARG(bool, true));
        QCOMPARE(browser->property("shownCount").toInt(), 1);
        QTRY_VERIFY(desk.shows("chronologyFilters"));
        QVERIFY(desk.run("chronology.filter_clear"));
        QCOMPARE(browser->property("shownCount").toInt(), 7);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theChronologyAtEverySizeInGermanRightToLeftAndUnderFingers() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            const QList<qint64> ids = stock(desk);
            const Story story = tell(desk, ids.first());
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            QObject *layer = desk.item("chronologyLayer");
            QMetaObject::invokeMethod(layer, "show", Q_ARG(QVariant, "book"), Q_ARG(QVariant, story.book));
            QTRY_VERIFY(shown(desk));
            QObject *browser = chronology(desk);
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                QMetaObject::invokeMethod(browser, "select", Q_ARG(qint64, 0));
                look(desk, "chronology");
                // An event's card, beside or under the lanes, or in their place.
                QMetaObject::invokeMethod(browser, "select", Q_ARG(qint64, story.siege));
                look(desk, "chronology-card");
                // Zoomed in, with filters on.
                QMetaObject::invokeMethod(desk.item("chronologyCanvas"), "zoomBy", Q_ARG(double, 40.0), Q_ARG(double, -1.0));
                QMetaObject::invokeMethod(browser, "setFilter", Q_ARG(QString, "involved"), Q_ARG(QVariant, story.keeper),
                                          Q_ARG(bool, true));
                QMetaObject::invokeMethod(browser, "setFilter", Q_ARG(QString, "met"), Q_ARG(QVariant, true), Q_ARG(bool, true));
                look(desk, "chronology-filtered");
                // Nothing left.
                bool took = false;
                QMetaObject::invokeMethod(browser, "setRange", Q_RETURN_ARG(bool, took), Q_ARG(QString, "1800"), Q_ARG(QString, "1801"));
                QVERIFY(took);
                look(desk, "chronology-nothing");
                QMetaObject::invokeMethod(browser, "clearFilters");
                QMetaObject::invokeMethod(desk.item("chronologyCanvas"), "fit");
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                for (const char *command : {"chronology.filter", "chronology.find", "chronology.range"}) {
                    QVERIFY(desk.run(command));
                    if (!desk.waitForPopup())
                        found << QString("%1 %2x%3 %4 — it did not open")
                                     .arg(QString::fromLatin1(way.language))
                                     .arg(size.width())
                                     .arg(size.height())
                                     .arg(command);
                    look(desk, QString(command).replace('.', '-'));
                    QVERIFY(desk.closePopups());
                }
                desk.menu("chronology");
                QVERIFY(desk.waitForPopup());
                look(desk, "chronology-menu");
                QVERIFY(desk.closePopups());
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }
};

QTEST_MAIN(ChronologyWindowTests)
#include "chronologywindow_tests.moc"
