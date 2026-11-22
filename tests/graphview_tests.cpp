#include <QElapsedTimer>
#include <QImage>
#include <QJsonDocument>
#include <QPainter>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

#include "graph.h"
#include "graphcanvas.h"
#include "graphlayout.h"
#include "graphmodel.h"
#include "library.h"

namespace {

// A graph of a given size with the shape real ones have: a few hubs that
// much hangs off, chains, and loose ends. The same every time.
QList<QPair<int, int>> madeUpEdges(int count) {
    QList<QPair<int, int>> edges;
    quint32 state = 12345;
    const auto next = [&] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (int node = 1; node < count; ++node) {
        // Most hang off an earlier node, the early ones more often.
        const int other = int(next() % quint32(node)) / (next() % 3 == 0 ? 1 : 4);
        edges.append({node, other});
        if (next() % 5 == 0)
            edges.append({node, int(next() % quint32(count))});
    }
    return edges;
}

double nearestPair(const QList<QPointF> &at) {
    double nearest = 1e9;
    for (int a = 0; a < at.size(); ++a)
        for (int b = a + 1; b < at.size(); ++b)
            nearest = std::min(nearest, std::hypot(at.at(a).x() - at.at(b).x(), at.at(a).y() - at.at(b).y()));
    return nearest;
}

// A small library with a story in it, as whatIsReadBecomesAGraph makes one.
struct Story {
    QTemporaryDir dir;
    Library library{dir.filePath("library")};
    qint64 first = 0, second = 0;
    qint64 mara = 0, guild = 0, singing = 0, harbour = 0, question = 0;

    Story() {
        library.open();
        BookMeta meta;
        meta.title = "The First Tide";
        meta.authors = {"Ann Author"};
        meta.series = "The Sea";
        meta.seriesIndex = 1;
        meta.tags = {"fantasy"};
        first = library.addEntry(meta, true).id;
        meta.title = "The Second Tide";
        meta.seriesIndex = 2;
        second = library.addEntry(meta, true).id;
        Graph graph(library);
        graph.syncLibrary();
        mara = graph.upsert({}, "character", "Mara", "A ferry pilot.", {{"aliases", QJsonArray{"The Pilot"}}}, GraphPlace{first, 1, 0.2});
        guild = graph.upsert({}, "group", "The Tide Guild", "Keeps the ferry lines.", {}, GraphPlace{first, 1, 0.5});
        harbour = graph.upsert({}, "place", "Glass Harbour", "Where the ferries berth.", {}, GraphPlace{first, 2, 0.5});
        singing = graph.upsert({}, "concept", "Tide-singing", "The guild's magic.", {}, GraphPlace{first, 6, 0.1});
        graph.link(mara, guild, "member of", {}, GraphPlace{first, 2, 0});
        graph.link(guild, harbour, "based in", {}, GraphPlace{first, 2, 0.5});
        graph.link(mara, singing, "can do", "Revealed late.", GraphPlace{first, 6, 0.1});
        question = graph.upsert({}, "question", "Why does the guild fear singers?", "Because a singer can move the tide.",
                                {}, GraphPlace{first, 3, 0});
        graph.link(question, guild, "asked about", {}, GraphPlace{first, 3, 0});
        graph.upsert({}, "character", "Mara", {}, {}, GraphPlace{second, 0, 0});
        graph.upsert("world:9", "character", "Ilse");
    }
};

QStringList names(const GraphBrowser &browser) {
    QStringList names;
    for (const QVariant &row : browser.rows())
        names << row.toMap().value("name").toString();
    return names;
}

} // namespace

// The graph as the window browses it and draws it: what the model shows of
// what the storage holds, and where the layout puts it.
class GraphViewTests : public QObject {
    Q_OBJECT

private slots:
    // --- The layout ---

    void theSameGraphIsTheSamePicture() {
        const QList<QPair<int, int>> edges = madeUpEdges(120);
        GraphLayout one, other;
        one.start(120, edges);
        other.start(120, edges);
        one.settle();
        // Stepped a little at a time, as the window does, it ends the same.
        while (other.step()) {
        }
        QVERIFY(one.settled() && other.settled());
        QCOMPARE(one.positions(), other.positions());
        QVERIFY(one.steps() > 10);
        // And it stops: a settled layout no longer moves.
        const QList<QPointF> settled = one.positions();
        QVERIFY(!one.step());
        QCOMPARE(one.positions(), settled);
    }

    void nothingPilesUp() {
        for (const int count : {1, 2, 40, 400}) {
            GraphLayout layout;
            layout.start(count, madeUpEdges(count));
            // Before a single step no two nodes share a spot.
            if (count > 1)
                QVERIFY2(nearestPair(layout.positions()) > 1, qPrintable(QString::number(count)));
            layout.settle();
            if (count > 1) {
                QVERIFY2(nearestPair(layout.positions()) > 12, qPrintable(QString("%1: %2").arg(count).arg(nearestPair(layout.positions()))));
                // Nor is it flung apart: the picture is about as wide as its nodes need.
                const QRectF box = layout.bounds();
                QVERIFY2(std::max(box.width(), box.height()) < GraphLayout::spacing * (4 + 3 * std::sqrt(double(count))),
                         qPrintable(QString("%1: %2 x %3").arg(count).arg(box.width()).arg(box.height())));
                // It stays around the middle.
                QVERIFY(std::abs(box.center().x()) < box.width() / 3 + 1 && std::abs(box.center().y()) < box.height() / 3 + 1);
            }
        }
        // Nodes with no edges at all still spread out.
        GraphLayout loose;
        loose.start(50, {});
        loose.settle();
        QVERIFY(nearestPair(loose.positions()) > 12);
    }

    void twoThousandNodesSettleInTime() {
        // How fast this machine and this build are, by a layout a tenth the
        // size: a sanitizer build is several times slower than a release
        // one, and what is asked is that two thousand nodes cost what they
        // should next to two hundred, not a number of seconds.
        QElapsedTimer clock;
        clock.start();
        GraphLayout small;
        small.start(200, madeUpEdges(200));
        small.settle();
        const double reference = std::max(1.0, clock.nsecsElapsed() / 1e6);
        const int count = 2000;
        GraphLayout layout, whole;
        whole.start(count, madeUpEdges(count));
        clock.restart();
        QVERIFY(whole.step());
        // One step is the least a slice can be.
        const double oneStep = clock.nsecsElapsed() / 1e6;
        whole.settle();
        clock.restart();
        layout.start(count, madeUpEdges(count));
        // No slice of it holds the window up for long.
        qint64 longest = 0;
        while (true) {
            QElapsedTimer slice;
            slice.start();
            const bool more = layout.run(6);
            longest = std::max(longest, slice.elapsed());
            if (!more)
                break;
        }
        const qint64 took = clock.elapsed();
        qInfo() << "2000 nodes settled in" << took << "ms over" << layout.steps() << "steps; longest slice" << longest
                << "ms; one step" << oneStep << "ms; 200 nodes in" << reference << "ms";
        QVERIFY(layout.settled());
        // Ten times the nodes cost a little more than ten times the work a
        // step (the pushing apart is n log n), over about as many steps.
        QVERIFY2(took < reference * 60 + 200, qPrintable(QString("%1 ms against %2 ms for 200 nodes").arg(took).arg(reference)));
        // A slice ends with the step that is running when its time is up.
        QVERIFY2(longest < 6 + oneStep * 4 + 20, qPrintable(QString("%1 ms, a step being %2 ms").arg(longest).arg(oneStep)));
        QVERIFY(layout.steps() <= 400);
        // The slices only decide when the steps happen: however the time
        // fell, it took the same steps to the same picture as in one go.
        QCOMPARE(layout.steps(), whole.steps());
        QCOMPARE(layout.positions(), whole.positions());
        QVERIFY2(nearestPair(layout.positions()) > 6, qPrintable(QString::number(nearestPair(layout.positions()))));
    }

    void aGraphThatGrowsKeepsItsShape() {
        QList<QPair<int, int>> edges = madeUpEdges(60);
        GraphLayout layout;
        layout.start(60, edges);
        layout.settle();
        const QList<QPointF> before = layout.positions();
        const double size = std::max(layout.bounds().width(), layout.bounds().height());
        // Two more nodes join, each on a node that was there.
        edges.append({60, 3});
        edges.append({61, 40});
        QList<QPointF> known = before;
        known << QPointF() << QPointF();
        QList<bool> placed(60, true);
        placed << false << false;
        layout.start(62, edges, known, placed);
        // They start beside what they hang off.
        QVERIFY(QLineF(layout.positions().at(60), before.at(3)).length() < GraphLayout::spacing);
        layout.settle();
        double moved = 0;
        for (int node = 0; node < 60; ++node)
            moved = std::max(moved, QLineF(layout.positions().at(node), before.at(node)).length());
        QVERIFY2(moved < size / 3, qPrintable(QString("%1 of %2").arg(moved).arg(size)));
    }

    void theRingStaysOutsideAndLeavesTheInsideAlone() {
        // Forty nodes, and five more around them, each tied to one inside.
        QList<QPair<int, int>> edges = madeUpEdges(40);
        GraphLayout alone;
        alone.start(40, edges);
        alone.settle();
        QList<bool> ring(40, false);
        for (int outer = 0; outer < 5; ++outer) {
            ring << true;
            edges.append({40 + outer, outer * 7});
        }
        edges.append({40, 12});
        GraphLayout ringed, again;
        ringed.start(45, edges, {}, {}, 1, ring);
        again.start(45, edges, {}, {}, 1, ring);
        ringed.settle();
        again.settle();
        QVERIFY(ringed.settled());
        QCOMPARE(ringed.positions(), again.positions());
        // The inside is the very picture it is without the ring.
        QCOMPARE(ringed.positions().mid(0, 40), alone.positions());
        // Every node of the ring is outside the box around the others, and none on another.
        QRectF inside(alone.positions().first(), QSizeF(0, 0));
        for (const QPointF &at : alone.positions())
            inside |= QRectF(at, QSizeF(0, 0)).adjusted(-1, -1, 1, 1);
        for (int outer = 40; outer < 45; ++outer)
            QVERIFY2(!inside.contains(ringed.positions().at(outer)), qPrintable(QString::number(outer)));
        QVERIFY(nearestPair(ringed.positions()) > 12);
        // Nor is it flung far off: an edge's length or three from the inside.
        const QRectF all = ringed.bounds();
        QVERIFY2(all.width() < inside.width() + GraphLayout::spacing * 8 && all.height() < inside.height() + GraphLayout::spacing * 8,
                 qPrintable(QString("%1 x %2 around %3 x %4").arg(all.width()).arg(all.height()).arg(inside.width()).arg(inside.height())));
        // Added to a picture that has settled, with the inside held, nothing inside moves at all.
        QList<QPointF> known = alone.positions();
        QList<bool> placed(40, true), fixed(40, true);
        for (int outer = 0; outer < 5; ++outer) {
            known << QPointF();
            placed << false;
            fixed << false;
        }
        GraphLayout added;
        added.start(45, edges, known, placed, 1, ring, fixed);
        QVERIFY(!added.settled());
        added.settle();
        QCOMPARE(added.positions().mid(0, 40), alone.positions());
        for (int outer = 40; outer < 45; ++outer)
            QVERIFY(!inside.contains(added.positions().at(outer)));
    }

    // --- The model ---

    void theModelShowsWhatTheGraphHolds() {
        Story story;
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showScope();
        // Two books, their author, series and tag, and what the story told.
        QCOMPARE(browser.nodeCount(), 5 + 5);
        QCOMPARE(browser.knownCount(), 10);
        QVERIFY(browser.storied());
        QCOMPARE(browser.shown().size(), browser.positions().size());
        // Rows by kind in the order kinds are listed, then by name.
        QCOMPARE(names(browser), (QStringList{"Mara", "The Tide Guild", "Glass Harbour", "Tide-singing", "Why does the guild fear singers?",
                                              "The First Tide", "The Second Tide", "Ann Author", "The Sea", "fantasy"}));
        QStringList kinds;
        for (const QVariant &kind : browser.kinds())
            kinds << QString("%1:%2").arg(kind.toMap().value("kind").toString()).arg(kind.toMap().value("count").toInt());
        QCOMPARE(kinds, (QStringList{"character:1", "group:1", "place:1", "concept:1", "question:1", "book:2", "author:1", "series:1", "tag:1"}));
        // Every edge joins two rows that are shown.
        QVERIFY(browser.edgeCount() >= 4 + 6);
        for (const GraphBrowser::Link &link : browser.links()) {
            QCOMPARE(browser.shown().at(link.from).node.id, link.edge.source);
            QCOMPARE(browser.shown().at(link.to).node.id, link.edge.target);
        }
        QCOMPARE(browser.shown().at(browser.rowOf(story.mara)).links, 2);
        // The worlds there are, the general graph first.
        QCOMPARE(browser.scopes().size(), 2);
        QCOMPARE(browser.scopes().at(1).toMap().value("scope").toString(), QString("world:9"));
        browser.showScope("world:9");
        QCOMPARE(names(browser), QStringList{"Ilse"});
        QCOMPARE(browser.scope(), QString("world:9"));
    }

    void everythingKnownOfANode() {
        Story story;
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showScope();
        QSignalSpy selected(&browser, &GraphBrowser::selectedChanged);
        browser.select(story.mara);
        QCOMPARE(selected.count(), 1);
        const QVariantMap details = browser.details();
        QCOMPARE(details.value("name").toString(), QString("Mara"));
        QCOMPARE(details.value("kind").toString(), QString("character"));
        QCOMPARE(details.value("aliases").toStringList(), QStringList{"The Pilot"});
        QCOMPARE(details.value("summary").toString(), QString("A ferry pilot."));
        // Both books she is in, each with where she was first met.
        const QVariantList places = details.value("places").toList();
        QCOMPARE(places.size(), 2);
        QStringList titles;
        for (const QVariant &place : places)
            titles << place.toMap().value("title").toString();
        titles.sort();
        QCOMPARE(titles, (QStringList{"The First Tide", "The Second Tide"}));
        // Her relations, each with the node at its other end.
        const QVariantList relations = details.value("relations").toList();
        QCOMPARE(relations.size(), 2);
        QCOMPARE(relations.at(0).toMap().value("relation").toString(), QString("member of"));
        QCOMPARE(relations.at(0).toMap().value("otherName").toString(), QString("The Tide Guild"));
        QCOMPARE(relations.at(0).toMap().value("other").toLongLong(), story.guild);
        QVERIFY(relations.at(0).toMap().value("outgoing").toBool());
        QCOMPARE(relations.at(1).toMap().value("note").toString(), QString("Revealed late."));
        // A relation seen from its other end points back.
        browser.select(story.guild);
        bool pointsBack = false;
        for (const QVariant &relation : browser.details().value("relations").toList())
            pointsBack = pointsBack || (relation.toMap().value("other").toLongLong() == story.mara && !relation.toMap().value("outgoing").toBool());
        QVERIFY(pointsBack);
        // A book says how it stands and the days it was read on.
        Graph graph(story.library);
        story.library.setReading(story.first, "read", 4, "2026-03-02");
        graph.logReading(story.first, 0, 40, "2026-02-27");
        graph.logReading(story.first, 40, 100, "2026-03-02");
        browser.refresh();
        browser.select(graph.bookNode(story.first));
        QCOMPARE(browser.details().value("status").toString(), QString("read"));
        QCOMPARE(browser.details().value("rating").toInt(), 4);
        QCOMPARE(browser.details().value("days").toList().size(), 2);
        // A question says what was asked and the gist of the answer.
        browser.select(story.question);
        QCOMPARE(browser.details().value("kind").toString(), QString("question"));
        QVERIFY(browser.details().value("summary").toString().contains("move the tide"));
        // Nothing selected, nothing said.
        browser.select(0);
        QVERIFY(browser.details().isEmpty());
        // A node that isn't shown can't be selected.
        browser.select(987654);
        QCOMPARE(browser.selected(), 0);
    }

    void aBookIsNeverAheadOfItsReader() {
        Story story;
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        // Up to the third chapter: no tide-singing yet, nor that she can.
        browser.showBookUpTo(story.first, 2, 0.6);
        QVERIFY(browser.bounded());
        QCOMPARE(browser.bookTitle(), QString("The First Tide"));
        QCOMPARE(names(browser), (QStringList{"Mara", "The Tide Guild", "Glass Harbour"}));
        QCOMPARE(browser.edgeCount(), 2);
        // Neither the search nor her relations know more than the page.
        QVERIFY(browser.find("tide-singing").isEmpty());
        QCOMPARE(browser.find("tide").size(), 1);
        browser.select(story.mara);
        QCOMPARE(browser.details().value("relations").toList().size(), 1);
        // What lies ahead can't be jumped to either.
        browser.jumpTo(story.singing);
        QCOMPARE(browser.selected(), story.mara);
        // Around her, still nothing from ahead.
        browser.recentre();
        QCOMPARE(names(browser), (QStringList{"Mara", "The Tide Guild"}));
        browser.setDepth(3);
        QCOMPARE(names(browser), (QStringList{"Mara", "The Tide Guild", "Glass Harbour"}));
        QVERIFY(browser.back());

        // Reading on lets more in, and what was there stays where it was.
        browser.settleNow();
        const QPointF maraWas = browser.positions().at(browser.rowOf(story.mara));
        QSignalSpy reframed(&browser, &GraphBrowser::reframed);
        browser.showBookUpTo(story.first, 6, 0.5);
        QCOMPARE(reframed.count(), 0);
        QCOMPARE(browser.nodeCount(), 5);
        QVERIFY(names(browser).contains("Tide-singing"));
        browser.settleNow();
        QVERIFY(QLineF(browser.positions().at(browser.rowOf(story.mara)), maraWas).length() < GraphLayout::spacing * 1.5);

        // From the library, a book never opened here (read elsewhere, or its
        // graph built whole on request) shows all that is known of it ...
        browser.showBook(story.first);
        QVERIFY(!browser.bounded());
        QCOMPARE(browser.nodeCount(), 5);
        QCOMPARE(browser.hiddenCount(), 0);
        // ... one being read is shown as far as it was left, and says how
        // much more is known past that place, to be shown on request.
        story.library.setProgress(story.first, {1, 0.6, 20, 0});
        browser.showBook(story.first);
        QVERIFY(browser.bounded());
        QCOMPARE(names(browser), (QStringList{"Mara", "The Tide Guild"}));
        QCOMPARE(browser.hiddenCount(), 3);
        browser.showWhole();
        QVERIFY(!browser.bounded());
        QCOMPARE(browser.nodeCount(), 5);
        QCOMPARE(browser.hiddenCount(), 0);
        // Left at the very start, nothing is shown, but it is said what is
        // known. (A library that never read it further: no furthest place.)
        story.library.setNotes(story.first, "furthest", {0, 0, QString(""), 0});
        story.library.setProgress(story.first, {0, 0.01, 0.5, 0});
        browser.showBook(story.first);
        QCOMPARE(browser.knownCount(), 0);
        QCOMPARE(browser.hiddenCount(), 5);
        story.library.setProgress(story.first, {1, 0.6, 20, 0});
        browser.showBook(story.first);
        // ... and all of it once it is finished.
        story.library.setReading(story.first, "read", 0, {});
        browser.showBook(story.first);
        QVERIFY(!browser.bounded());
        QCOMPARE(browser.nodeCount(), 5);
        // A series is its books together.
        browser.showSeries("The Sea");
        QCOMPARE(browser.showing(), QString("series"));
        QCOMPARE(browser.nodeCount(), 5);
        // A book nothing is known of is an empty graph, not an error.
        browser.showBook(story.second + 50);
        QCOMPARE(browser.nodeCount(), 0);
        QCOMPARE(browser.knownCount(), 0);
    }

    void filtersNarrowIt() {
        Story story;
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showScope();
        QSignalSpy changed(&browser, &GraphBrowser::changed);
        browser.setKindShown("book", false);
        browser.setKindShown("tag", false);
        QCOMPARE(changed.count(), 2);
        QCOMPARE(browser.nodeCount(), 7);
        QVERIFY(!names(browser).contains("The First Tide"));
        // A kind that is left out is still listed, to be put back.
        bool listed = false;
        for (const QVariant &kind : browser.kinds())
            if (kind.toMap().value("kind").toString() == "book")
                listed = !kind.toMap().value("shown").toBool() && kind.toMap().value("count").toInt() == 2;
        QVERIFY(listed);
        // Only what was asked about: the question and what it is about.
        browser.setOnlyAsked(true);
        QCOMPARE(names(browser), (QStringList{"The Tide Guild", "Why does the guild fear singers?"}));
        browser.showEveryKind();
        QCOMPARE(browser.nodeCount(), 10);
        QVERIFY(!browser.onlyAsked());

        // Around a node, as far as the depth says.
        browser.select(story.mara);
        browser.recentre();
        QCOMPARE(browser.centre(), story.mara);
        QCOMPARE(browser.depth(), 1);
        QCOMPARE(names(browser), (QStringList{"Mara", "The Tide Guild", "Tide-singing"}));
        browser.setDepth(2);
        QVERIFY(names(browser).contains("Glass Harbour") && names(browser).contains("Why does the guild fear singers?"));
        QVERIFY(!names(browser).contains("Ann Author"));
        // On to a neighbour, and back the way it came.
        browser.setDepth(1);
        browser.select(story.guild);
        browser.recentre();
        QVERIFY(names(browser).contains("Glass Harbour") && !names(browser).contains("Tide-singing"));
        QVERIFY(browser.canGoBack());
        QVERIFY(browser.back());
        QCOMPARE(browser.centre(), story.mara);
        QCOMPARE(browser.selected(), story.guild);
        QVERIFY(browser.back());
        QCOMPARE(browser.nodeCount(), 10);
        QVERIFY(!browser.back());
        // A node the filters hide comes back when it is jumped to.
        browser.setKindShown("place", false);
        browser.jumpTo(story.harbour);
        QCOMPARE(browser.selected(), story.harbour);
        // The search goes by names and other names, the best match first.
        QCOMPARE(browser.find("pilot").size(), 1);
        QCOMPARE(browser.find("tide").first().toMap().value("name").toString(), QString("Tide-singing"));
        QVERIFY(browser.find("nobody by this name").isEmpty());
    }

    void theKeyboardWalksTheGraph() {
        Story story;
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showBookUpTo(story.first, 9, 0);
        browser.settleNow();
        // The first step selects the best connected node.
        browser.step("right");
        QVERIFY(browser.selected() == story.mara || browser.selected() == story.guild);
        // A step lands on a node that really is that way, along an edge when one leads there.
        for (const char *way : {"left", "right", "up", "down"}) {
            browser.select(story.guild);
            const QPointF from = browser.positions().at(browser.rowOf(story.guild));
            browser.step(way);
            if (browser.selected() == story.guild)
                continue;
            const QPointF to = browser.positions().at(browser.rowOf(browser.selected()));
            const QString direction(way);
            QVERIFY2(direction == "left" ? to.x() < from.x() : direction == "right" ? to.x() > from.x()
                     : direction == "up" ? to.y() < from.y() : to.y() > from.y(), way);
        }
        // Every node can be reached from every other with the four keys.
        QSet<qint64> reached{story.mara};
        QList<qint64> frontier{story.mara};
        while (!frontier.isEmpty()) {
            const qint64 from = frontier.takeFirst();
            for (const char *way : {"left", "right", "up", "down"}) {
                browser.select(from);
                browser.step(way);
                if (!reached.contains(browser.selected())) {
                    reached << browser.selected();
                    frontier << browser.selected();
                }
            }
        }
        QCOMPARE(reached.size(), browser.nodeCount());
        // Tab goes through the rows in the order the list has them, and round.
        browser.select(0);
        QStringList walked;
        for (int i = 0; i < browser.nodeCount(); ++i) {
            browser.advance(1);
            walked << browser.details().value("name").toString();
        }
        QCOMPARE(walked, names(browser));
        browser.advance(1);
        QCOMPARE(browser.details().value("name").toString(), names(browser).first());
        browser.advance(-1);
        QCOMPARE(browser.details().value("name").toString(), names(browser).last());
    }

    void editingByHand() {
        Story story;
        GraphBrowser browser, other;
        browser.setLibrary(&story.library);
        other.setLibrary(&story.library);
        browser.showScope();
        other.showScope();
        Graph graph(story.library);

        // Renamed, it is still found by the name it had; renamed in the
        // library's tenant, it is called so there (#110).
        QVERIFY(browser.edit(story.mara, "Mara Venn", "A ferry pilot who sings tides."));
        QCOMPARE(Graph::ofLibrary(story.library).node(story.mara)->name, QString("Mara Venn"));
        QCOMPARE(graph.named({}, "character", "Mara"), story.mara);
        QVERIFY(names(browser).contains("Mara Venn"));
        // Not to a name another of its kind has: those two are to be merged.
        const qint64 twin = graph.upsert({}, "character", "Captain Venn");
        QVERIFY(!browser.edit(twin, "Mara Venn", ""));
        // The other view hears of it.
        QTRY_VERIFY(names(other).contains("Mara Venn"));

        // Two that are one are folded together.
        browser.refresh();
        graph.link(twin, story.harbour, "berths at");
        QVERIFY(browser.merge(story.mara, twin));
        QCOMPARE(browser.selected(), story.mara);
        QVERIFY(!names(browser).contains("Captain Venn"));
        QCOMPARE(browser.details().value("relations").toList().size(), 3);
        QVERIFY(browser.details().value("aliases").toStringList().contains("Captain Venn"));

        // A thing added by hand, related to the selected one.
        const qint64 boat = browser.add("item", "The Grey Ferry", "Her boat.", "pilots");
        QVERIFY(boat > 0);
        QCOMPARE(browser.selected(), boat);
        QCOMPARE(browser.details().value("relations").toList().size(), 1);
        QCOMPARE(browser.details().value("relations").toList().first().toMap().value("other").toLongLong(), story.mara);
        // Naming one that exists relates to it rather than making a second.
        browser.select(boat);
        QCOMPARE(browser.add("place", "glass harbour", "", "moored in"), story.harbour);
        QCOMPARE(graph.find("harbour", "place").size(), 1);
        // In a book's graph it belongs to the book, at the reader's place.
        browser.showBookUpTo(story.first, 2, 0.9);
        const qint64 note = browser.add("theme", "Debt", "What the guild is owed.", "");
        QVERIFY(note > 0 && names(browser).contains("Debt"));
        QCOMPARE(graph.seen(note).first().section, 2);
        browser.showBookUpTo(story.first, 1, 0);
        QVERIFY(!names(browser).contains("Debt"));

        // A relation, and then a node, taken out.
        browser.showScope();
        browser.select(boat);
        const qint64 edge = browser.details().value("relations").toList().first().toMap().value("edge").toLongLong();
        QVERIFY(browser.unlink(edge));
        QCOMPARE(browser.details().value("relations").toList().size(), 1);
        QVERIFY(browser.remove(boat));
        QCOMPARE(browser.selected(), 0);
        QVERIFY(!graph.node(boat));
        QVERIFY(!browser.remove(boat));
    }

    void whatIsShownFollowsTheGraph() {
        Story story;
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showScope();
        browser.settleNow();
        QVERIFY(!browser.settling());
        const QList<QPointF> before = browser.positions();
        // Read again with nothing new, nothing moves.
        QSignalSpy changed(&browser, &GraphBrowser::changed);
        GraphFeed::announce();
        QTRY_COMPARE(changed.count(), 1);
        QCOMPARE(browser.positions(), before);
        QVERIFY(!browser.settling());
        // Something new arrives where it belongs, and the picture settles again and stops.
        Graph graph(story.library);
        const qint64 storm = graph.upsert({}, "event", "The Winter Storm", "Sinks the old ferry.");
        graph.link(storm, story.harbour, "strikes");
        GraphFeed::announce();
        QTRY_VERIFY(names(browser).contains("The Winter Storm"));
        QTRY_VERIFY(!browser.settling());
        const int harbour = browser.rowOf(story.harbour);
        QVERIFY(QLineF(browser.positions().at(harbour), before.at(harbour)).length() < GraphLayout::spacing * 2);
    }

    void whatABookConnectsToInOtherBooks() {
        Story story;
        Graph graph(story.library);
        // Another book altogether, with a magic of its own that this one's echoes.
        BookMeta meta;
        meta.title = "The Salt Clock";
        meta.authors = {"Bea Other"};
        const qint64 clock = story.library.addEntry(meta, true).id;
        story.library.setReading(clock, "read", 0, {});
        graph.syncLibrary();
        const qint64 binding = graph.upsert({}, "concept", "Tide-binding", "Knots that hold water.", {}, GraphPlace{clock, 2, 0});
        const qint64 knot = graph.upsert({}, "item", "The Drowned Knot", "The first binding.", {}, GraphPlace{clock, 3, 0});
        graph.link(binding, knot, "made with", {}, GraphPlace{clock, 3, 0});
        QVERIFY(graph.link(story.singing, binding, GraphEdge::acrossMark() + "echoes", "Both move the sea by a craft that costs its maker.",
                           GraphPlace{story.first, 6, 0.2}) > 0);

        GraphBrowser browser;
        browser.setLibrary(&story.library);
        QVERIFY(browser.beyond());
        browser.showBookUpTo(story.first, 9, 0);
        // The book's own five things, and around them the one from elsewhere.
        QCOMPARE(browser.beyondCount(), 1);
        QCOMPARE(browser.nodeCount(), 6);
        QCOMPARE(browser.knownCount(), 5);
        const GraphBrowser::Shown outer = browser.shown().at(browser.rowOf(binding));
        QVERIFY(outer.outer);
        QCOMPARE(outer.from, QString("The Salt Clock"));
        QCOMPARE(outer.fromBook, clock);
        QVERIFY(!browser.shown().at(browser.rowOf(story.singing)).outer);
        // In the list it comes last, under a heading of its own, with its book.
        QCOMPARE(names(browser).last(), QString("Tide-binding"));
        QVERIFY(browser.rows().last().toMap().value("outer").toBool());
        QCOMPARE(browser.rows().last().toMap().value("from").toString(), QString("The Salt Clock"));
        QVERIFY(!browser.rows().first().toMap().value("outer").toBool());
        // The kinds count the book's own things only.
        for (const QVariant &kind : browser.kinds())
            if (kind.toMap().value("kind").toString() == "concept")
                QCOMPARE(kind.toMap().value("count").toInt(), 1);
        // The edge that ties it is one across books.
        bool across = false;
        for (const GraphBrowser::Link &link : browser.links())
            across = across || (link.edge.across() && browser.shown().at(link.to).node.id == binding);
        QVERIFY(across);

        // From this book's thing: the relation in words, the book, and why.
        browser.select(story.singing);
        QVariantMap tie;
        for (const QVariant &relation : browser.details().value("relations").toList())
            if (relation.toMap().value("other").toLongLong() == binding)
                tie = relation.toMap();
        QCOMPARE(tie.value("relation").toString(), QString("echoes"));
        QVERIFY(tie.value("across").toBool() && tie.value("outgoing").toBool());
        QCOMPARE(tie.value("otherFrom").toString(), QString("The Salt Clock"));
        QVERIFY(tie.value("note").toString().contains("costs its maker"));
        // From the other end: which book it is from, and the same tie back.
        browser.select(binding);
        QVERIFY(browser.details().value("outer").toBool());
        QCOMPARE(browser.details().value("from").toString(), QString("The Salt Clock"));
        QCOMPARE(browser.details().value("relations").toList().size(), 1);
        QCOMPARE(browser.details().value("relations").toList().first().toMap().value("other").toLongLong(), story.singing);
        QCOMPARE(browser.details().value("places").toList().first().toMap().value("bookId").toLongLong(), clock);

        // Switched off and on again, the book's own picture doesn't move.
        browser.settleNow();
        QHash<qint64, QPointF> before;
        for (const GraphBrowser::Shown &shown : browser.shown())
            if (!shown.outer)
                before.insert(shown.node.id, browser.positions().at(browser.rowOf(shown.node.id)));
        QSignalSpy changed(&browser, &GraphBrowser::changed);
        browser.setBeyond(false);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(browser.nodeCount(), 5);
        QCOMPARE(browser.beyondCount(), 1);
        QCOMPARE(browser.selected(), 0);
        browser.settleNow();
        for (auto place = before.cbegin(); place != before.cend(); ++place)
            QCOMPARE(browser.positions().at(browser.rowOf(place.key())), place.value());
        browser.setBeyond(true);
        browser.settleNow();
        QCOMPARE(browser.nodeCount(), 6);
        for (auto place = before.cbegin(); place != before.cend(); ++place)
            QCOMPARE(browser.positions().at(browser.rowOf(place.key())), place.value());
        // The ring is outside the book's own things.
        QRectF inside(before.cbegin().value(), QSizeF(0, 0));
        for (const QPointF &at : std::as_const(before))
            inside |= QRectF(at, QSizeF(0, 0)).adjusted(-1, -1, 1, 1);
        QVERIFY(!inside.contains(browser.positions().at(browser.rowOf(binding))));

        // It is never ahead of the reader: before tide-singing is met, and
        // before the connection was noticed, there is nothing beyond.
        browser.showBookUpTo(story.first, 2, 0.6);
        QCOMPARE(browser.beyondCount(), 0);
        QVERIFY(!names(browser).contains("Tide-binding"));
        browser.showBookUpTo(story.first, 6, 0.15);
        QVERIFY(names(browser).contains("Tide-singing") && !names(browser).contains("Tide-binding"));
        browser.showBookUpTo(story.first, 6, 0.3);
        QVERIFY(names(browser).contains("Tide-binding"));
        // With the kind it hangs on left out, it goes too.
        browser.setKindShown("concept", false);
        QVERIFY(!names(browser).contains("Tide-binding"));
        browser.setKindShown("concept", true);

        // Enter on it goes to its book's graph, around it; back returns.
        browser.select(binding);
        browser.recentre();
        QCOMPARE(browser.bookId(), clock);
        QCOMPARE(browser.bookTitle(), QString("The Salt Clock"));
        QCOMPARE(browser.centre(), binding);
        QCOMPARE(browser.selected(), binding);
        QVERIFY(names(browser).contains("The Drowned Knot"));
        // From there, this book's thing is the one beyond.
        QVERIFY(browser.shown().at(browser.rowOf(story.singing)).outer);
        QVERIFY(browser.canGoBack());
        QVERIFY(browser.back());
        QCOMPARE(browser.bookId(), story.first);
        QVERIFY(browser.bounded());
        QCOMPARE(browser.selected(), binding);
        QVERIFY(names(browser).contains("Mara"));
        QVERIFY(!browser.back());

        // In the library: only what connects books, with the books it joins.
        browser.showScope();
        QVERIFY(names(browser).size() > 6);
        browser.setOnlyAcross(true);
        QStringList joined = names(browser);
        joined.sort();
        QCOMPARE(joined, (QStringList{"The First Tide", "The Salt Clock", "Tide-binding", "Tide-singing"}));
        QCOMPARE(browser.edgeCount(), 3);
        browser.select(story.singing);
        bool said = false;
        for (const QVariant &relation : browser.details().value("relations").toList())
            said = said || (relation.toMap().value("across").toBool() && relation.toMap().value("otherFrom").toString() == "The Salt Clock");
        QVERIFY(said);
        browser.setOnlyAcross(false);
        QVERIFY(names(browser).contains("Mara"));

        // In two collections at the top, the two books are kept apart:
        // nothing joins them in the library, and nothing is beyond.
        const qint64 sea = *story.library.addCollection("Zee"), land = *story.library.addCollection("Land");
        QVERIFY(story.library.putInCollection(sea, story.first, true) && story.library.putInCollection(land, clock, true));
        browser.setOnlyAcross(true);
        QCOMPARE(browser.edgeCount(), 0);
        browser.setOnlyAcross(false);
        QVERIFY(names(browser).contains("Tide-binding"));
        browser.showBookUpTo(story.first, 9, 0);
        QCOMPARE(browser.beyondCount(), 0);
        QVERIFY(!names(browser).contains("Tide-binding"));
        // Put in the same one, they meet again: nothing was lost.
        QVERIFY(story.library.putInCollection(sea, clock, true));
        browser.refresh();
        QCOMPARE(browser.beyondCount(), 1);
        QVERIFY(names(browser).contains("Tide-binding"));
    }

    // --- The picture ---

    void thePictureIsDrawnWithinAFrame() {
        // Two thousand nodes in a library of their own, painted as the
        // window paints them.
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        const int count = 2000;
        QList<qint64> ids;
        const QStringList kinds = GraphBrowser::knownKinds().mid(0, 9);
        for (int i = 0; i < count; ++i)
            ids << graph.upsert({}, kinds.at(i % kinds.size()), QString("Thing number %1").arg(i));
        for (const auto &edge : madeUpEdges(count))
            graph.link(ids.at(edge.first), ids.at(edge.second), "relates to");
        GraphBrowser browser;
        browser.setLibrary(&library);
        QElapsedTimer clock;
        clock.start();
        browser.showScope();
        const qint64 loaded = clock.elapsed();
        QCOMPARE(browser.nodeCount(), count);
        browser.settleNow();

        GraphCanvas canvas;
        canvas.setSize(QSizeF(1180, 700));
        canvas.setColors({{"blue", QColor("#7aa2f7")}, {"green", QColor("#9ece6a")}, {"red", QColor("#f7768e")},
                          {"muted", QColor("#5c5c66")}});
        canvas.setBrowser(&browser);
        canvas.fit(false);
        QImage frame(1180, 700, QImage::Format_ARGB32_Premultiplied);
        frame.fill(Qt::black);
        double whole = 0, near = 0;
        {
            QPainter painter(&frame);
            canvas.paint(&painter);
            whole = canvas.paintTime();
            // Zoomed in on a selected node, most of it is off the page.
            browser.select(ids.at(0));
            // About a point, which zooms at once rather than gliding there.
            canvas.zoomBy(8, 590, 350);
            canvas.paint(&painter);
            near = canvas.paintTime();
        }
        qInfo() << "2000 nodes: read in" << loaded << "ms, painted whole in" << whole << "ms, zoomed in" << near << "ms";
        // Something was drawn, and well within what a smooth pan allows on
        // a machine several times slower than this one.
        int drawn = 0;
        for (int y = 0; y < frame.height(); y += 2)
            for (int x = 0; x < frame.width(); x += 2)
                drawn += frame.pixel(x, y) != QColor(Qt::black).rgba();
        QVERIFY2(drawn > 2000, qPrintable(QString::number(drawn)));
        QVERIFY2(whole < 120, qPrintable(QString::number(whole)));
        QVERIFY2(near < 60, qPrintable(QString::number(near)));
        // A point on a node finds the node.
        const QPointF at = canvas.pointOf(ids.at(0));
        QCOMPARE(canvas.nodeAt(at.x(), at.y()), ids.at(0));
        QCOMPARE(canvas.nodeAt(-500000, -500000), 0);
    }

    // --- When knowledge is revealed, and spoilers ---

    // A book read back to its very start, none of the way (#159), keeps
    // what was read past it as read, and what lies further on as ahead.
    void spoilersStayAheadOfABookReadBackToItsStart() {
        Story story;
        story.library.setProgress(story.first, {2, 0.6, 30, 0});
        story.library.setProgress(story.first, {0, 0.0, 0, 0});
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showBook(story.first);
        QCOMPARE(browser.spoilersAfter().value("section").toInt(), 2);
    }

    void spoilersAreKeptApartInTheDetails() {
        Story story;
        Graph graph(story.library);
        // Who Mara is turns out late in the book, with a name of her own.
        graph.learn(story.mara, "summary", "A ferry pilot, and the guild's lost singer.", GraphMoment{{story.first, 6, 0.1}, "the lost singer"});
        graph.learn(story.mara, "alias", "The Singer", GraphMoment{{story.first, 6, 0.2}, "The Singer"});
        story.library.setProgress(story.first, {2, 0.6, 30, 0});
        GraphBrowser browser;
        browser.setLibrary(&story.library);
        browser.showBook(story.first);
        QVERIFY(browser.bounded());
        QCOMPARE(browser.spoilersAfter().value("section").toInt(), 2);
        browser.select(story.mara);
        const auto words = [&browser] { return QString::fromUtf8(QJsonDocument::fromVariant(browser.details()).toJson()); };
        QVariantMap details = browser.details();
        QCOMPARE(details.value("summary").toString(), QString("A ferry pilot."));
        QCOMPARE(details.value("aliases").toStringList(), QStringList{"The Pilot"});
        // When what is known was revealed, each with its moment.
        const QVariantList timeline = details.value("timeline").toList();
        QVERIFY(!timeline.isEmpty());
        QCOMPARE(timeline.first().toMap().value("section").toInt(), 1);
        QCOMPARE(timeline.first().toMap().value("text").toString(), QString("A ferry pilot."));
        // Closed by default: how many, and nothing of what they say.
        QCOMPARE(details.value("aheadCount").toInt(), 3);
        QVERIFY(!details.value("aheadShown").toBool());
        QVERIFY(details.value("ahead").toList().isEmpty());
        QVERIFY(!words().contains("lost singer"));
        QVERIFY(!words().contains("The Singer"));
        QVERIFY(!words().contains("can do"));
        QVERIFY(browser.find("Singer").isEmpty());
        QVERIFY(!QJsonDocument::fromVariant(browser.rows()).toJson().contains("lost singer"));

        // Opened for her, it holds each one with its moment, marked as ahead.
        browser.showAhead(story.mara);
        details = browser.details();
        QVERIFY(details.value("aheadShown").toBool());
        const QVariantList ahead = details.value("ahead").toList();
        QCOMPARE(ahead.size(), 3);
        QStringList what;
        for (const QVariant &step : ahead) {
            what << step.toMap().value("what").toString();
            QCOMPARE(step.toMap().value("after").toMap().value("section").toInt(), 2);
            QCOMPARE(step.toMap().value("section").toInt(), 6);
        }
        // In the order the reader would come to them.
        QCOMPARE(what, (QStringList{"summary", "relation", "alias"}));
        QCOMPARE(ahead.at(0).toMap().value("text").toString(), QString("A ferry pilot, and the guild's lost singer."));
        QCOMPARE(ahead.at(0).toMap().value("anchor").toString(), QString("the lost singer"));
        // The summary above it is still the one of the reader's place.
        QCOMPARE(details.value("summary").toString(), QString("A ferry pilot."));
        // For her only, and for as long as the program runs.
        browser.select(story.guild);
        QVERIFY(!browser.details().value("aheadShown").toBool());
        browser.select(story.mara);
        QVERIFY(browser.details().value("aheadShown").toBool());
        browser.showAhead(story.mara, false);
        QVERIFY(!browser.details().value("aheadShown").toBool());

        // Shown whole: what is met later is in the picture, and its details
        // say it is met later and nothing more until asked.
        browser.showWhole();
        QVERIFY(names(browser).contains("Tide-singing"));
        browser.select(story.singing);
        QVERIFY(browser.details().value("nodeAhead").toBool());
        QVERIFY(browser.details().value("summary").toString().isEmpty());
        QVERIFY(browser.details().value("places").toList().isEmpty());
        QVERIFY(browser.details().value("aheadCount").toInt() >= 1);
        browser.select(story.mara);
        QCOMPARE(browser.details().value("summary").toString(), QString("A ferry pilot."));
        QVERIFY(!words().contains("can do"));
        QVERIFY(!QJsonDocument::fromVariant(browser.rows()).toJson().contains("lost singer"));
        // The graph of everything read is as careful.
        browser.showScope("");
        browser.select(story.mara);
        QCOMPARE(browser.details().value("summary").toString(), QString("A ferry pilot."));
        QVERIFY(!words().contains("lost singer"));
        // A name she is given later finds her nowhere (the question asked
        // of singers is found by its own words).
        for (const QVariant &found : browser.find("Singer"))
            QVERIFY(found.toMap().value("id").toLongLong() != story.mara);
        QCOMPARE(browser.find("Pilot").size(), 1);

        // Going back to an earlier page keeps what was read: no spoilers there.
        story.library.setProgress(story.first, {1, 0.1, 5, 0});
        browser.showBook(story.first);
        QCOMPARE(browser.spoilersAfter().value("section").toInt(), 2);
        QVERIFY(names(browser).contains("Glass Harbour"));

        // A book marked as read has no spoilers: everything, nothing ahead.
        story.library.setReading(story.first, "read", 0, {});
        browser.showBook(story.first);
        QVERIFY(browser.spoilersAfter().isEmpty());
        browser.select(story.mara);
        QCOMPARE(browser.details().value("summary").toString(), QString("A ferry pilot, and the guild's lost singer."));
        QVERIFY(browser.details().value("aliases").toStringList().contains("The Singer"));
        QCOMPARE(browser.details().value("aheadCount").toInt(), 0);
    }
};

QTEST_MAIN(GraphViewTests)
#include "graphview_tests.moc"
