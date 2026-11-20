// The graph's Between books view says when it shows part of what there is
// (#91), as the header of a cut picture does (#64): "300 of 700 connections
// between books", counted in the tenant shown and in the reader's language;
// the connections between the most connected things come first; a button
// over the picture and an entry under Filter show more of them; it is read
// quickly from a large library; and the note and its button keep to the
// window at every size, in German and under fingers (desk.h).
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size.

#include <QElapsedTimer>
#include <QQuickStyle>
#include <QRandomGenerator>
#include <QSet>
#include <QtTest>
#include <algorithm>
#include <string_view>
#include <tuple>

#include "desk.h"
#include "graph.h"
#include "graphcut.h"
#include "graphmodel.h"
#include "library.h"
#include "spaces.h"
#include "tenancy.h"

namespace {

// `perBook` things in each book, the first of each its hub, tied to every
// other thing of its book; then the one connection between the first two
// hubs, and `connections` in all between things of different books that
// are no hubs, made after it. The order of before (#91), the latest first,
// left the hubs' connection out of any cut.
struct Across {
    QList<qint64> things;
    qint64 hubs = 0;
    qint64 connections = 0;
};

Across across(Library &library, const QList<qint64> &books, int perBook, int connections) {
    const Library::Batch batch(library);
    Graph graph(library);
    Across made;
    for (int b = 0; b < books.size(); ++b) {
        graph.bookNode(books.at(b));
        for (int i = 0; i < perBook; ++i)
            made.things << graph.upsert({}, i == 0 ? "place" : "character", QString("Thing %1 of book %2").arg(i).arg(b), {}, {},
                                        GraphPlace{books.at(b), 0, double(i) / perBook});
        for (int i = 1; i < perBook; ++i)
            graph.link(made.things.at(b * perBook), made.things.at(b * perBook + i), "knows", {}, GraphPlace{books.at(b), 0, 0.5});
    }
    const QString echoes = GraphEdge::acrossMark() + "echoes";
    QSet<qint64> ids;
    made.hubs = graph.link(made.things.at(0), made.things.at(perBook), echoes, {}, GraphPlace{books.at(1), 0, 0.9});
    ids << made.hubs;
    QRandomGenerator random(91);
    const auto count = static_cast<quint32>(made.things.size());
    while (ids.size() < connections) {
        const int from = static_cast<int>(random.bounded(count)), to = static_cast<int>(random.bounded(count));
        if (from / perBook == to / perBook || from % perBook == 0 || to % perBook == 0)
            continue;
        if (const qint64 id = graph.link(made.things.at(from), made.things.at(to), echoes, {}, GraphPlace{books.at(to / perBook), 0, 0.9}))
            ids << id;
    }
    made.connections = ids.size();
    return made;
}

QList<qint64> books(Library &library, int count, const QString &prefix = "Book") {
    QList<qint64> ids;
    for (int i = 0; i < count; ++i) {
        BookMeta meta;
        meta.title = QString("%1 %2").arg(prefix).arg(i);
        meta.authors = {QString("Author %1").arg(i % 5)};
        ids << library.addEntry(meta, true).id;
    }
    return ids;
}

qsizetype acrossIn(const GraphView &view) { return std::ranges::count_if(view.edges, &GraphEdge::across); }

} // namespace

class GraphAcrossTests : public QObject {
    Q_OBJECT

    // Every problem found, by where it was found: "de 480x800 cut".
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
        desk.shot(QString("across-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

    static GraphBrowser *browser(Desk &desk) {
        return qobject_cast<GraphBrowser *>(desk.find("libraryGraph")->property("browser").value<QObject *>());
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    }

    void theConnectionsBetweenTheMostConnectedThingsComeFirst() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        const Across made = across(library, books(library, 6), 20, 80);
        // In the tenant, by the degree it keeps, and in the graph of no
        // tenant, by a count: the hubs' connection, the oldest, first.
        for (const Graph &graph : {Graph::forTenant(library, Tenant::unfiled()), Graph(library)}) {
            const GraphView first = graph.acrossBooks(1);
            QCOMPARE(first.edges.size(), qsizetype{1});
            QCOMPARE(first.edges.first().id, made.hubs);
        }
        const GraphCut cut = GraphCut::across(Graph::forTenant(library, Tenant::unfiled()), 10);
        QCOMPARE(cut.view.edges.first().id, made.hubs);
    }

    void theCutSaysHowManyConnectionsThereAreInTheTenant() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        const Across made = across(library, books(library, 12), 40, 700);
        // Another tenant's connections are not counted in this one's.
        const QList<qint64> elsewhere = books(library, 2, "Elsewhere");
        const qint64 shelf = *library.addCollection("Elsewhere");
        for (const qint64 book : elsewhere)
            QVERIFY(library.putInCollection(shelf, book, true));
        std::ignore = across(library, elsewhere, 10, 30); // the other tenant's, counted apart below
        const Graph unfiled = Graph::forTenant(library, Tenant::unfiled());

        const GraphCut cut = GraphCut::across(unfiled, 300);
        QVERIFY(cut.cut());
        QCOMPARE(cut.by, GraphCut::By::Across);
        QCOMPARE(cut.connections, qint64{300});
        QCOMPARE(acrossIn(cut.view), qsizetype{300});
        QCOMPARE(cut.relations, made.connections);
        QCOMPARE(cut.toJson().value("cut").toString(), QString("across"));
        // Each thing with the books it is in, tied to them.
        QVERIFY(std::ranges::any_of(cut.view.nodes, [](const GraphNode &node) { return node.kind == "book"; }));
        QVERIFY(std::ranges::any_of(cut.view.edges, [](const GraphEdge &edge) { return edge.kind == "appears in"; }));
        QVERIFY(std::ranges::none_of(cut.view.nodes, [](const GraphNode &node) { return node.name.startsWith("Elsewhere"); }));
        // All of them shown, nothing is cut.
        QVERIFY(!GraphCut::across(unfiled, 1000).cut());
        QCOMPARE(GraphCut::across(Graph::forTenant(library, Tenant::ofCollection(Spaces::collectionId(library, shelf))), 1000).relations,
                 qint64{30});

        // The window's browser: 300 of them, then more, a step at a time.
        GraphBrowser browser;
        browser.setLibrary(&library);
        browser.setTenant(Tenant::unfiled().globalId());
        browser.showScope();
        browser.setOnlyAcross(true);
        QCOMPARE(browser.cut(), QString("across"));
        QCOMPARE(browser.acrossCount(), 300);
        QCOMPARE(browser.wholeEdges(), static_cast<int>(made.connections));
        QVERIFY(browser.moreAcross());
        browser.showMoreAcross();
        QCOMPARE(browser.acrossCount(), 600);
        QCOMPARE(browser.cut(), QString("across"));
        browser.showMoreAcross();
        // All 700 are fewer than the next step: nothing is cut, nothing more to show.
        QCOMPARE(browser.acrossCount(), static_cast<int>(made.connections));
        QCOMPARE(browser.cut(), QString());
        QVERIFY(!browser.moreAcross());
        // Shown again, it starts at the first step.
        browser.setOnlyAcross(false);
        browser.setOnlyAcross(true);
        QCOMPARE(browser.acrossCount(), 300);
    }

    void theConnectionsOfALargeLibraryAreReadQuickly() {
        // A hundred and fifty books of forty things, 5,850 relations within
        // the books and 10,000 connections between them.
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        std::ignore = across(library, books(library, 150), 40, 10000); // what it made is counted by the cut
        const Graph graph = Graph::forTenant(library, Tenant::unfiled());
        QElapsedTimer clock;
        clock.start();
        const GraphCut first = GraphCut::across(graph, 300);
        const qint64 tookFirst = clock.restart();
        const GraphCut most = GraphCut::across(graph, 900);
        const qint64 tookMost = clock.elapsed();
        QCOMPARE(first.relations, qint64{10000});
        QCOMPARE(most.connections, qint64{900});
        QVERIFY2(tookFirst < 300 && tookMost < 500,
                 qPrintable(QString("300 connections in %1 ms, 900 in %2 ms").arg(tookFirst).arg(tookMost)));
    }

    void theWindowSaysItsCutAndShowsMore() {
        for (const QByteArray &language : {QByteArray("en"), QByteArray("de")}) {
            Desk desk(language, {}, false);
            const Across made = across(*desk.backend->library(), stock(desk), 30, 400);
            desk.backend->reload();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QVERIFY(desk.resize(QSize(1180, 780)));
            desk.quicken();
            desk.call("showGraph", 0);
            QTRY_VERIFY(desk.shows("libraryGraph"));
            GraphBrowser *graph = browser(desk);
            graph->setOnlyAcross(true);
            QTRY_COMPARE(graph->acrossCount(), 300);
            const QLocale here;
            const QString count =
                language == "de" ? QString("300 von %1 Verbindungen zwischen Büchern") : QString("300 of %1 connections between books");
            QTRY_COMPARE(desk.find("graphCount")->property("text").toString(), count.arg(here.toString(made.connections)));
            QTRY_VERIFY(desk.shows("graphCut"));
            QVERIFY(desk.find("graphCut")->property("text").toString().contains("300"));
            QTRY_VERIFY(desk.shows("graphMoreAcross"));
            look(desk, "cut");

            // By the button over the picture: all 400, and nothing more to say.
            QTest::mouseClick(
                desk.window, Qt::LeftButton, {},
                desk.item("graphMoreAcross")
                    ->mapToScene(QPointF(desk.item("graphMoreAcross")->width() / 2, desk.item("graphMoreAcross")->height() / 2))
                    .toPoint());
            QTRY_COMPARE(graph->acrossCount(), static_cast<int>(made.connections));
            QTRY_VERIFY(!desk.shows("graphMoreAcross"));
            QVERIFY(!desk.shows("graphCut"));

            // By the entry under Filter.
            graph->setOnlyAcross(false);
            graph->setOnlyAcross(true);
            QTRY_COMPARE(graph->acrossCount(), 300);
            QVERIFY(desk.run("graph.filter"));
            QVERIFY(desk.waitForPopup());
            for (const char letter : std::string_view(language == "de" ? "mehr verbindungen" : "more connections"))
                QTest::keyClick(desk.window, letter);
            QTest::keyClick(desk.window, Qt::Key_Return);
            QTRY_COMPARE(graph->acrossCount(), static_cast<int>(made.connections));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    void theNoteAndItsButtonAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            std::ignore = across(*desk.backend->library(), stock(desk), 30, 400); // only its cut is looked at
            desk.backend->reload();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            desk.call("showGraph", 0);
            QTRY_VERIFY(desk.shows("libraryGraph"));
            browser(desk)->setOnlyAcross(true);
            QTRY_COMPARE(browser(desk)->cut(), QString("across"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                QTRY_VERIFY(!browser(desk)->settling());
                look(desk, "cut");
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(GraphAcrossTests)
#include "graphacross_tests.moc"
