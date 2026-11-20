// A picture of the graph cut at a limit says so (#64): the whole graph of a
// library larger than the window draws, and what is around a thing, count
// what they were cut from in the storage; the header of the graph's pane says
// "2,000 of 2,300 things", in the reader's language; `omabook graph show`,
// `graph node` and the MCP tools that run them answer how many there are in
// all; and a view of which nothing was left out says nothing of the kind.

#include <QElapsedTimer>
#include <QJsonArray>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <optional>

#include "customization.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcut.h"
#include "graphmodel.h"
#include "graphtools.h"
#include "library.h"
#include "theme.h"
#include "translator.h"

namespace {

// A graph of `count` things in a chain, with a hub every tenth thing ties
// to: more relations than things, and the hubs the most connected.
struct Made {
    QList<qint64> ids;
    int relations = 0;
};
Made chained(Graph &graph, int count, const QString &scope = {}) {
    Made made;
    for (int i = 0; i < count; ++i)
        made.ids << graph.upsert(scope, i % 10 == 0 ? "place" : "character", QString("Thing %1").arg(i));
    for (int i = 1; i < count; ++i) {
        made.relations += graph.link(made.ids.at(i - 1), made.ids.at(i), "next to") != 0;
        if (i % 10 != 0)
            made.relations += graph.link(made.ids.at(i - i % 10), made.ids.at(i), "holds") != 0;
    }
    return made;
}

// A graph of `count` things like a library's, made the same every time:
// each thing near a few made just before it, a hub every two hundred things
// that many tie to, and now and then a relation far across, so that the
// steps around a thing reach further and further, and hubs at most of them.
QList<qint64> likeALibrary(Library &library, Graph &graph, int count) {
    const Library::Batch batch(library);
    QRandomGenerator random(64);
    QList<qint64> ids;
    for (int i = 0; i < count; ++i) {
        ids << graph.upsert(QString(), i % 200 == 0 ? "place" : "character", QString("Thing %1").arg(i));
        if (i == 0)
            continue;
        graph.link(ids.at(i - 1 - static_cast<int>(random.bounded(std::min(i, 5)))), ids.at(i), "near");
        if (random.bounded(3) == 0)
            graph.link(ids.at(i - i % 200), ids.at(i), "holds");
        if (random.bounded(20) == 0)
            graph.link(ids.at(static_cast<int>(random.bounded(i))), ids.at(i), "knows");
    }
    return ids;
}

// What is around a thing as #64 counted it, in one recursive query: the
// things within `steps`, and the relations among them.
std::pair<qint64, qint64> countedInOneQuery(const Library &library, qint64 node, int steps) {
    const QString connection = "old-count-" + QUuid::createUuid().toString(QUuid::Id128);
    std::pair<qint64, qint64> counted;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(library.databasePath());
        if (database.open()) {
            QSqlQuery query(database);
            query.prepare("WITH RECURSIVE near(id, step) AS (SELECT ?, 0 "
                          "UNION SELECT target, step + 1 FROM graph_edges JOIN near ON source = near.id WHERE step < ? "
                          "UNION SELECT source, step + 1 FROM graph_edges JOIN near ON target = near.id WHERE step < ?), "
                          "reached AS (SELECT DISTINCT id FROM near) "
                          "SELECT (SELECT COUNT(*) FROM reached), (SELECT COUNT(*) FROM graph_edges WHERE source IN reached "
                          "AND target IN reached)");
            query.addBindValue(node);
            query.addBindValue(steps);
            query.addBindValue(steps);
            if (query.exec() && query.next())
                counted = {query.value(0).toLongLong(), query.value(1).toLongLong()};
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return counted;
}

// The count at the top of the graph's pane, as the window shows it, in a
// language of the window's.
struct Header {
    QTemporaryDir dir;
    std::optional<Translator> translator;
    std::optional<Theme> theme;
    std::optional<Customization> custom;
    std::optional<Look> look;
    std::optional<QQmlEngine> engine;
    std::unique_ptr<QObject> count;
    QStringList warnings;

    Header(const QByteArray &language, GraphBrowser &browser) {
        Fixtures::write(dir.filePath("language"), language + "\n");
        translator.emplace(dir.filePath("language"));
        QCoreApplication::installTranslator(&*translator);
        theme.emplace(dir.filePath("desktop"));
        custom.emplace(dir.filePath("custom"));
        look.emplace(&*theme, &*custom);
        engine.emplace();
        QObject::connect(&*engine, &QQmlEngine::warnings, &*engine, [this](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                warnings << error.toString();
        });
        engine->rootContext()->setContextProperty("translator", &*translator);
        engine->rootContext()->setContextProperty("theme", &*look);
        QQmlComponent component(&*engine, QUrl("qrc:/GraphCount.qml"));
        count.reset(component.createWithInitialProperties({{"browser", QVariant::fromValue(&browser)}}));
        if (!count)
            warnings << component.errorString();
    }
    ~Header() {
        count.reset();
        engine.reset();
        QCoreApplication::removeTranslator(&*translator);
    }
    [[nodiscard]] QString text() const { return count ? count->property("text").toString() : QString(); }
};

} // namespace

class GraphCutTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    }

    void aWholeScopeCutAtItsLimitCountsWhatItWasCutFrom() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const Made made = chained(graph, 60);
        // A world of its own is another scope, and not counted with it.
        chained(graph, 15, "world:1");

        const GraphCut cut = GraphCut::whole(library, {}, 20);
        QCOMPARE(cut.view.nodes.size(), qsizetype{20});
        QVERIFY(cut.cut());
        QCOMPARE(cut.things, qint64{60});
        QCOMPARE(cut.relations, qint64{made.relations});
        QVERIFY(cut.view.edges.size() < cut.relations);
        // The most connected first: every hub is among them.
        for (int hub = 0; hub < 60; hub += 10)
            QVERIFY(std::ranges::contains(cut.view.nodes, made.ids.at(hub), &GraphNode::id));
        const QJsonObject json = cut.toJson();
        QCOMPARE(json.value("total").toObject().value("nodes").toInteger(), qint64{60});
        QCOMPARE(json.value("total").toObject().value("edges").toInteger(), qint64{made.relations});
        QCOMPARE(json.value("cut").toString(), QString("most connected"));

        // Not cut, it is its own total and says no cut.
        const GraphCut whole = GraphCut::whole(library, "world:1", 20);
        QVERIFY(!whole.cut());
        QCOMPARE(whole.things, qint64{15});
        QCOMPARE(whole.relations, static_cast<qint64>(whole.view.edges.size()));
        QVERIFY(!whole.toJson().contains("cut"));
        QCOMPARE(whole.toJson().value("total").toObject().value("nodes").toInteger(), qint64{15});
        // Exactly at its limit nothing was left out either.
        QVERIFY(!GraphCut::whole(library, "world:1", 15).cut());
    }

    void whatIsAroundAThingCutAtItsLimitCountsWhatIsWithinReach() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const Made made = chained(graph, 60);
        // Two steps from the first hub: its nine, the next hub, and their
        // neighbours along the chain.
        const GraphCut whole = GraphCut::around(library, made.ids.at(0), 2, 400);
        QVERIFY(!whole.cut());
        const GraphCut cut = GraphCut::around(library, made.ids.at(0), 2, 5);
        QCOMPARE(cut.view.nodes.size(), qsizetype{5});
        QVERIFY(cut.cut());
        QCOMPARE(cut.by, GraphCut::By::Nearest);
        QCOMPARE(cut.things, static_cast<qint64>(whole.view.nodes.size()));
        QCOMPARE(cut.relations, static_cast<qint64>(whole.view.edges.size()));
        QCOMPARE(cut.toJson().value("cut").toString(), QString("nearest"));
    }

    void theCountAroundAThingIsTheOneTheRecursiveQueryGave() {
        // Thousands of things, counted around a hub, a thing among others
        // and a thing on its own, at every depth the view goes to (#100).
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        QVERIFY(!graph.sides().apart());
        const QList<qint64> ids = likeALibrary(library, graph, 4000);
        const qint64 alone = graph.upsert(QString(), "character", "On its own");
        for (const qint64 centre : {ids.at(1000), ids.at(2345), alone})
            for (int depth = 0; depth <= 6; ++depth) {
                // A limit of one is always reached, so it is always counted.
                const GraphCut cut = GraphCut::around(library, centre, depth, 1);
                const auto [things, relations] = countedInOneQuery(library, centre, depth);
                QVERIFY2(things > 0, "the recursive query answered");
                QCOMPARE(cut.things, things);
                QCOMPARE(cut.relations, relations);
            }
        // Farther is more, up to all of it.
        QVERIFY(GraphCut::around(library, ids.at(2345), 2, 1).things < GraphCut::around(library, ids.at(2345), 4, 1).things);
    }

    void theCountAroundAThingAtDepthFourTakesLittleTime() {
        // On a copy of a real library the recursive query took over a second
        // at depth four, every step reaching a hub; level by level it is tens
        // of milliseconds.
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const QList<qint64> ids = likeALibrary(library, graph, 6000);
        QElapsedTimer clock;
        clock.start();
        const GraphCut cut = GraphCut::around(library, ids.at(3000), 4, 1);
        const qint64 took = clock.elapsed();
        QVERIFY(cut.things > 100);
        QVERIFY2(took < 250, qPrintable(QString("%1 things counted in %2 ms").arg(cut.things).arg(took)));
    }

    void theHeaderSaysHowManyThereAreAndThatTheMostConnectedAreShown() {
        // More things than the window draws, in a library of their own.
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const Made made = chained(graph, 2300);
        GraphBrowser browser;
        browser.setLibrary(&library);
        browser.showScope();
        QCOMPARE(browser.nodeCount(), 2000);
        QCOMPARE(browser.wholeCount(), 2300);
        QCOMPARE(browser.wholeEdges(), made.relations);
        QCOMPARE(browser.cut(), QString("connected"));
        const QString edges = QLocale().toString(browser.edgeCount()), all = QLocale().toString(made.relations);
        {
            Header header("en", browser);
            QVERIFY2(header.warnings.isEmpty(), qPrintable(header.warnings.join('\n')));
            QCOMPARE(header.text(), QString("2,000 of 2,300 things · %1 of %2 relations").arg(edges, all));
        }
        {
            Header header("de", browser);
            QVERIFY2(header.warnings.isEmpty(), qPrintable(header.warnings.join('\n')));
            const QLocale german = QLocale();
            QCOMPARE(header.text(), QString("2.000 von 2.300 Dingen · %1 von %2 Beziehungen")
                                        .arg(german.toString(browser.edgeCount()), german.toString(made.relations)));
        }
        QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));

        // Centred on a thing, it is whole again: what is around it.
        browser.jumpTo(made.ids.at(0));
        browser.recentre();
        QVERIFY(browser.centre() != 0);
        QCOMPARE(browser.cut(), QString());
        Header header("en", browser);
        QVERIFY(header.text().endsWith(QString("things · %1 relations").arg(browser.edgeCount())));
        QVERIFY(!header.text().contains(" of "));
    }

    void aGraphAllShownSaysJustItsSize() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const Made made = chained(graph, 30);
        GraphBrowser browser;
        browser.setLibrary(&library);
        browser.showScope();
        QCOMPARE(browser.cut(), QString());
        QCOMPARE(browser.wholeCount(), 30);
        Header header("en", browser);
        QCOMPARE(header.text(), QString("30 things · %1 relations").arg(made.relations));
    }

    void theCommandsAndToolsSayHowManyThereAreInAll() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const Made made = chained(graph, 40, "world:7");
        // `omabook graph show --scope world:7 --limit 10`, and graph_scope.
        const QJsonObject shown = GraphOps::show(library, 0, "world:7", std::nullopt, 10);
        QCOMPARE(shown.value("nodes").toArray().size(), qsizetype{10});
        QCOMPARE(shown.value("total").toObject().value("nodes").toInteger(), qint64{40});
        QCOMPARE(shown.value("total").toObject().value("edges").toInteger(), qint64{made.relations});
        QCOMPARE(shown.value("cut").toString(), QString("most connected"));
        QVERIFY(!GraphOps::show(library, 0, "world:7", std::nullopt, 40).contains("cut"));
        // `omabook graph node`, and graph_node: what is around a thing, all of
        // it within the limit, and its total that.
        const QJsonObject around = GraphOps::node(library, made.ids.at(0), 2);
        QCOMPARE(around.value("total").toObject().value("nodes").toInteger(), static_cast<qint64>(around.value("nodes").toArray().size()));
        QVERIFY(!around.contains("cut"));
        // The scopes are counted, not loaded.
        const QJsonArray scopes = GraphOps::scopes(library).value("scopes").toArray();
        QVERIFY(std::ranges::any_of(scopes, [](const QJsonValue &scope) {
            return scope.toObject().value("scope").toString() == "world:7" && scope.toObject().value("nodes").toInteger() == 40;
        }));
    }
};

QTEST_MAIN(GraphCutTests)
#include "graphcut_tests.moc"
