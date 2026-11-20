// Every reader of the knowledge graph stays within one tenant (ADR 0003,
// #95): two tenants, Fiction and Study, share a book, an author and a theme;
// nothing of the other tenant's own things shows in either, the shared book's
// things show in both, the one-tenant setting shows all, and what is made by
// hand stays in the tenant it was made in.

#include <QJsonArray>
#include <QSet>
#include <QtTest>
#include <algorithm>

#include "chronicle.h"
#include "chronology.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcontext.h"
#include "graphcut.h"
#include "graphtools.h"
#include "knowledgesearch.h"
#include "settings.h"
#include "tenancy.h"
#include "tenants.h"

namespace {

[[nodiscard]] QSet<qint64> idsOf(const QList<GraphNode> &nodes) {
    QSet<qint64> ids;
    for (const GraphNode &node : nodes)
        ids << node.id;
    return ids;
}

[[nodiscard]] QSet<qint64> idsOf(const QJsonArray &nodes) {
    QSet<qint64> ids;
    for (const QJsonValue &node : nodes)
        ids << node.toObject().value("id").toInteger();
    return ids;
}

// The split library, every book read, as the readers that go by the reader's place want it.
struct Read : Split {
    Read() {
        for (const qint64 book : {sea, both, waves, loose})
            library.setReading(book, "read", 0, {});
    }
    [[nodiscard]] Graph fictionGraph() const { return Graph::forTenant(library, tenantOf(fiction)); }
    [[nodiscard]] Graph studyGraph() const { return Graph::forTenant(library, tenantOf(study)); }
};

// The one tenant for the whole library, for as long as it lives.
struct OneTenant {
    Settings settings;
    OneTenant() { settings.setGraphOneTenant(true); }
    ~OneTenant() { settings.setGraphOneTenant(false); }
    OneTenant(const OneTenant &) = delete;
    OneTenant &operator=(const OneTenant &) = delete;
    OneTenant(OneTenant &&) = delete;
    OneTenant &operator=(OneTenant &&) = delete;
};

} // namespace

class TenantReadersTests : public QObject {
    Q_OBJECT

private slots:
    void theViewsKeepToTheirTenant() {
        const Read split;
        const Graph fiction = split.fictionGraph(), study = split.studyGraph();
        const QSet<qint64> ofFiction = idsOf(fiction.whole({}, 1000).nodes), ofStudy = idsOf(study.whole({}, 1000).nodes);
        // Each tenant's own, and what the shared book, author and theme are.
        for (const qint64 id : {split.mara, split.bell, split.theme, split.ann})
            QVERIFY2(ofFiction.contains(id), qPrintable(QString::number(id)));
        for (const qint64 id : {split.viscosity, split.bell, split.theme, split.ann, split.bo})
            QVERIFY2(ofStudy.contains(id), qPrintable(QString::number(id)));
        // Never the other's own.
        QVERIFY(!ofFiction.contains(split.viscosity) && !ofFiction.contains(split.bo));
        QVERIFY(!ofStudy.contains(split.mara));
        QVERIFY(!fiction.node(split.viscosity) && !study.node(split.mara));
        QVERIFY(fiction.find("Viscosity").isEmpty() && study.find("Mara").isEmpty());
        QVERIFY(!idsOf(fiction.around(split.theme, 3).nodes).contains(split.viscosity));
        QVERIFY(!idsOf(study.around(split.theme, 3).nodes).contains(split.mara));
        // The shared book's things in both.
        QVERIFY(idsOf(fiction.ofBook(split.both).nodes).contains(split.bell));
        QVERIFY(idsOf(study.ofBook(split.both).nodes).contains(split.bell));
        QVERIFY(fiction.ofBook(split.waves).nodes.isEmpty() && study.ofBook(split.sea).nodes.isEmpty());
        // A connection across the two is in neither.
        for (const Graph *graph : {&fiction, &study}) {
            QVERIFY(!std::ranges::contains(graph->acrossBooks(100).edges, split.echoes, &GraphEdge::id));
            QVERIFY(!std::ranges::contains(graph->whole({}, 1000).edges, split.echoes, &GraphEdge::id));
        }
        QVERIFY(std::ranges::contains(fiction.around(split.mara, 1).edges, split.keeps, &GraphEdge::id));
        QVERIFY(!std::ranges::contains(study.whole({}, 1000).edges, split.keeps, &GraphEdge::id));
    }

    void aSharedThingIsToldAsItsTenantKnowsIt() {
        const Read split;
        const Graph fiction = split.fictionGraph(), study = split.studyGraph();
        // The theme both share: what each tenant's book said of it, never the other's.
        QCOMPARE(fiction.node(split.theme)->summary, QString("Handed from keeper to keeper."));
        QCOMPARE(study.node(split.theme)->summary, QString("What a wave keeps of the one before."));
        const auto inWhole = [&](const Graph &graph) {
            const QList<GraphNode> nodes = graph.whole({}, 1000).nodes;
            return std::ranges::find(nodes, split.theme, &GraphNode::id)->summary;
        };
        QCOMPARE(inWhole(fiction), QString("Handed from keeper to keeper."));
        QCOMPARE(inWhole(study), QString("What a wave keeps of the one before."));
        QCOMPARE(fiction.facts(split.theme).size(), 1);
        QCOMPARE(fiction.facts(split.theme).first().id, split.seaFact);
        QCOMPARE(fiction.seen(split.theme).size(), 1);
        QCOMPARE(fiction.seen(split.theme).first().bookId, split.sea);
        for (const GraphReveal &step : fiction.timeline(split.theme))
            QVERIFY2(!step.text.contains("wave"), qPrintable(step.text));
        // The reading log of the tenant's books only.
        Graph(split.library).logReading(split.waves, 0, 10, "2026-11-01");
        Graph(split.library).logReading(split.sea, 0, 10, "2026-11-02");
        QCOMPARE(fiction.readingLog().size(), 1);
        QCOMPARE(fiction.readingLog().first().bookId, split.sea);
    }

    void theOneTenantSettingShowsAll() {
        const Read split;
        const OneTenant one;
        const Graph whole = Graph::ofLibrary(split.library);
        QCOMPARE(*whole.tenant(), Tenant::wholeLibrary());
        const QSet<qint64> all = idsOf(whole.whole({}, 1000).nodes);
        for (const qint64 id : {split.mara, split.viscosity, split.bell, split.theme, split.bo, split.note})
            QVERIFY(all.contains(id));
        QVERIFY(std::ranges::contains(whole.acrossBooks(100).edges, split.echoes, &GraphEdge::id));
        QCOMPARE(whole.facts(split.theme).size(), 2);
        QCOMPARE(Tenancy::chosen(split.library, {}).value_or(Tenant::unfiled()), Tenant::wholeLibrary());
    }

    void whatIsMadeByHandStaysInItsTenant() {
        Read split;
        Graph fiction = split.fictionGraph();
        const Graph study = split.studyGraph();
        const qint64 made = fiction.upsert({}, "topic", "Lighthouses", "Put in by hand while reading fiction.");
        QVERIFY(made > 0);
        QVERIFY(fiction.node(made) && !study.node(made));
        QVERIFY(!Graph::forTenant(split.library, Tenant::unfiled()).node(made));
        // Tied by hand to the book both share, it gains nothing from that.
        QVERIFY(fiction.link(made, Graph(split.library).bookNode(split.both), "mentions") > 0);
        QVERIFY(!study.node(made));
        // Nor can another tenant tie its things to it, or remove it.
        Graph other = split.studyGraph();
        QCOMPARE(other.link(split.viscosity, made, "mentions"), 0);
        QVERIFY(!other.remove(made));
        // And the rules say so whole as the writes did.
        const QList<Tenant> kept = Tenancy::ofNode(split.library, made);
        Tenancy::rebuild(split.library);
        QCOMPARE(Tenancy::ofNode(split.library, made), kept);
        QVERIFY(!study.node(made));
        // Tied to a book that then leaves the tenant, it stays where it was made (#97).
        QVERIFY(fiction.link(made, Graph(split.library).bookNode(split.sea), "mentions") > 0);
        QVERIFY(split.moveSeaToStudy());
        QVERIFY(split.fictionGraph().node(made) && !split.studyGraph().node(made));
        // The whole library's tenant sees it.
        const OneTenant one;
        QVERIFY(Graph::ofLibrary(split.library).node(made));
    }

    void aLibraryQuestionStaysWithTheTenantItWasAskedIn() {
        const Read split;
        const qint64 asked = GraphContext::noteQuestion(split.fictionGraph(), 0, std::nullopt,
                                                        "What do The Sea Wall and Both Shores share?", "A keeper, and a bell.");
        QVERIFY(asked > 0);
        QVERIFY(split.fictionGraph().node(asked));
        QVERIFY(!split.studyGraph().node(asked));
        // The same words asked in the other tenant are a question of its own.
        const qint64 again = GraphContext::noteQuestion(split.studyGraph(), 0, std::nullopt, "What do The Sea Wall and Both Shores share?",
                                                        "Nothing of the sea here.");
        QVERIFY(again > 0 && again != asked);
        QVERIFY(split.studyGraph().node(again) && !split.fictionGraph().node(again));
        QVERIFY(!GraphContext::picture(split.studyGraph()).contains("A keeper, and a bell."));
    }

    void whatTheAiIsToldKeepsToTheTenant() {
        const Read split;
        const QString fiction = GraphContext::picture(split.fictionGraph(), "inheritance viscosity");
        QVERIFY2(fiction.contains("Inheritance"), qPrintable(fiction));
        for (const char *theirs : {"Viscosity", "Wave Mechanics", "What a wave keeps"})
            QVERIFY2(!fiction.contains(theirs), qPrintable(fiction));
        const QString study = GraphContext::picture(split.studyGraph(), "inheritance mara");
        QVERIFY2(!study.contains("Mara") && !study.contains("The Sea Wall") && !study.contains("Handed from keeper"), qPrintable(study));
        const QString asked = GraphContext::forQuestion(split.fictionGraph(), split.sea, {split.sea, 9, 1.0}, "What of Inheritance?");
        QVERIFY2(asked.contains("Handed from keeper") && !asked.contains("wave"), qPrintable(asked));
        const QString digest = GraphContext::digest(split.studyGraph(), split.both, {split.both, 9, 1.0}, {"item", "theme"});
        QVERIFY2(digest.contains("The bell") && !digest.contains("Mara"), qPrintable(digest));
    }

    void knowsKeepsEachBookToItsTenant() {
        const Read split;
        const KnowledgeIndex index = KnowledgeIndex::build(split.library);
        // What a wave keeps is Study's to say: it finds the book of Study, not the novel the theme is in too.
        const QHash<qint64, KnownMatch> deep = index.find("wave", true);
        QVERIFY(deep.contains(split.waves));
        QVERIFY(!deep.contains(split.sea));
        QVERIFY(index.find("Inheritance").contains(split.sea) && index.find("Inheritance").contains(split.waves));
        QVERIFY(!index.find("Viscosity").contains(split.sea) && !index.find("Mara").contains(split.waves));
        // Built in one tenant, only its books are found.
        const KnowledgeIndex fiction = KnowledgeIndex::build(split.library, {split.tenantOf(split.fiction)});
        QVERIFY(fiction.find("Inheritance").contains(split.sea) && !fiction.find("Inheritance").contains(split.waves));
    }

    void theCutsCountTheTenant() {
        const Read split;
        const GraphCut fiction = GraphCut::whole(split.fictionGraph(), {}, 2);
        QCOMPARE(fiction.things, qint64(split.fictionGraph().whole({}, 1000).nodes.size()));
        QVERIFY(fiction.things < GraphCut::whole(Graph(split.library), {}, 2).things);
        const GraphCut around = GraphCut::around(split.studyGraph(), split.theme, 3, 1);
        QCOMPARE(around.things, qint64(split.studyGraph().around(split.theme, 3, 1000).nodes.size()));
    }

    void theChronologyIsReadInEachTenant() {
        const Read split;
        Graph graph(split.library);
        const qint64 storm = graph.upsert({}, "event", "The storm", {}, {}, GraphPlace{split.sea, 0, 0.1});
        const qint64 flood = graph.upsert({}, "event", "The flood", {}, {}, GraphPlace{split.waves, 0, 0.1});
        const QList<Chronology::Tenant> parts = Chronology::tenants(split.library);
        QCOMPARE(parts.size(), 3);
        const GraphKnown all = [](const GraphPlace &) { return true; };
        const Chronology::Facts fiction = Chronology::read(split.library, all, {.scope = {}, .in = split.tenantOf(split.fiction)});
        QVERIFY(std::ranges::contains(fiction.events, storm, &GraphNode::id)
                && !std::ranges::contains(fiction.events, flood, &GraphNode::id));
        QVERIFY(!Chronology::of(split.studyGraph(), storm, all));
        QVERIFY(Chronology::of(split.fictionGraph(), storm, all));
    }

    void theCommandsAskWhichTenantRatherThanMix() {
        Read split;
        const Library &library = split.library;
        const auto none = Tenancy::chosen(library, {});
        QVERIFY(!none);
        QVERIFY2(none.error().contains("Fiction") && none.error().contains("Study") && none.error().contains("Unfiled"),
                 qPrintable(Fixtures::whyNot(none)));
        QCOMPARE(*Tenancy::chosen(library, {}, split.sea), split.tenantOf(split.fiction));
        QVERIFY(!Tenancy::chosen(library, {}, split.both));
        QCOMPARE(*Tenancy::chosen(library, "Study/Physics"), split.tenantOf(split.study));
        QCOMPARE(*Tenancy::chosen(library, "unfiled"), Tenant::unfiled());
        QVERIFY(!Tenancy::chosen(library, "Nowhere"));
        // What the tools answer, in the tenant asked.
        const QJsonObject found = GraphOps::find(Graph::forTenant(split.library, split.tenantOf(split.study)), "Inheritance");
        QCOMPARE(found.value("tenant").toObject().value("name").toString(), QString("Study"));
        QCOMPARE(idsOf(found.value("nodes").toArray()), QSet<qint64>{split.theme});
        QVERIFY(idsOf(GraphOps::show(Graph::forTenant(split.library, split.tenantOf(split.fiction)), 0).value("nodes").toArray())
                    .contains(split.mara));
        QVERIFY(!idsOf(GraphOps::show(Graph::forTenant(split.library, split.tenantOf(split.study)), 0).value("nodes").toArray())
                     .contains(split.mara));
        QVERIFY(GraphOps::failed(GraphOps::show(Graph::forTenant(split.library, split.tenantOf(split.study)), split.sea)));
        QVERIFY(GraphOps::failed(GraphOps::node(Graph::forTenant(split.library, split.tenantOf(split.study)), split.mara)));
        const QJsonArray seen =
            GraphOps::node(Graph::forTenant(split.library, split.tenantOf(split.fiction)), split.theme).value("seen").toArray();
        QCOMPARE(seen.size(), 1);
        const QJsonObject written =
            GraphOps::exported(Graph::forTenant(split.library, split.tenantOf(split.fiction)), 0, {}, std::nullopt, "json");
        QVERIFY(!written.value("content").toString().contains("Viscosity"));
        QVERIFY(GraphOps::failed(GraphOps::remove(Graph::forTenant(split.library, split.tenantOf(split.study)), split.mara)));
        QVERIFY(Graph(split.library).node(split.mara));
    }
};

QTEST_GUILESS_MAIN(TenantReadersTests)
#include "tenantreaders_tests.moc"
