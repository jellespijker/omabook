// What is written by hand on a thing two tenants share, and what a relation
// is noted, stays in the tenant it was written or said in (ADR 0003, #109):
// a fact or a relation written by hand is its tenant's alone, a relation said
// by books of two tenants is noted per book, and a book in two tenants is
// offered the things of both to link to. A thing renamed by hand is called so
// in the tenant it was renamed in (#110).

#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "graph.h"
#include "graphbuilder.h"
#include "graphlinks.h"
#include "knowledgesearch.h"
#include "settings.h"
#include "spacereplay.h"
#include "tenancy.h"
#include "tenants.h"

namespace {

struct Written : Split {
    [[nodiscard]] Graph fictionGraph() const { return Graph::forTenant(library, tenantOf(fiction)); }
    [[nodiscard]] Graph studyGraph() const { return Graph::forTenant(library, tenantOf(study)); }
};

// The note a relation has as a graph reads it around one of its ends; "-" when it doesn't see it.
[[nodiscard]] QString noteOf(const Graph &graph, qint64 end, qint64 edge) {
    const QList<GraphEdge> edges = graph.around(end, 1).edges;
    const auto found = std::ranges::find(edges, edge, &GraphEdge::id);
    return found == edges.cend() ? QString("-") : found->note;
}

[[nodiscard]] Graph whole(const Split &split) { return Graph(split.library); }

// The ids of what a search found.
[[nodiscard]] QList<qint64> idsOf(const QList<GraphNode> &nodes) {
    QList<qint64> ids;
    for (const GraphNode &node : nodes)
        ids << node.id;
    return ids;
}

[[nodiscard]] bool hasFact(const Graph &graph, qint64 node, qint64 fact) {
    return std::ranges::contains(graph.facts(node), fact, &GraphFact::id);
}

// What one list of memberships has that the other hasn't, either way.
[[nodiscard]] QString differing(const QStringList &here, const QStringList &there) {
    QStringList lines;
    for (const QString &row : here)
        if (!there.contains(row))
            lines << "only here: " + row;
    for (const QString &row : there)
        if (!here.contains(row))
            lines << "only there: " + row;
    return lines.join('\n');
}

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

class TenantWritesTests : public QObject {
    Q_OBJECT

private slots:
    void aFactWrittenByHandOnASharedThingStaysInItsTenant() {
        Written split;
        Graph fiction = split.fictionGraph(), study = split.studyGraph();
        const qint64 said = fiction.learn(split.theme, "summary", "What the keepers leave.");
        QVERIFY(said > 0);
        QCOMPARE(fiction.node(split.theme)->summary, QString("What the keepers leave."));
        QCOMPARE(study.node(split.theme)->summary, QString("What a wave keeps of the one before."));
        QVERIFY(hasFact(fiction, split.theme, said) && !hasFact(study, split.theme, said));
        QVERIFY(!Tenancy::ofFact(split.library, said).contains(split.tenantOf(split.study)));
        // Another name by hand, likewise.
        QVERIFY(fiction.learn(split.theme, "alias", "The Legacy") > 0);
        QVERIFY(fiction.node(split.theme)->aliases().contains("The Legacy"));
        QVERIFY(!study.node(split.theme)->aliases().contains("The Legacy"));
        // The same words written in the other tenant are a fact of its own,
        // and what is edited there by hand is not the first tenant's word.
        const qint64 again = study.learn(split.theme, "summary", "What the keepers leave.");
        QVERIFY(again > 0 && again != said);
        QVERIFY(study.edit(split.theme, "Inheritance", "Study's own word on it."));
        QCOMPARE(study.node(split.theme)->summary, QString("Study's own word on it."));
        QCOMPARE(fiction.node(split.theme)->summary, QString("What the keepers leave."));
        QVERIFY(hasFact(fiction, split.theme, said) && !hasFact(fiction, split.theme, again));
        // The rules say so whole as the writes did, and the whole library sees both.
        const QStringList kept = memberships(split.library);
        QCOMPARE(rebuilt(split.library), kept);
        const OneTenant one;
        const Graph whole = Graph::ofLibrary(split.library);
        QVERIFY(hasFact(whole, split.theme, said) && hasFact(whole, split.theme, again));
    }

    void aRelationWrittenByHandOnSharedThingsStaysInItsTenant() {
        Written split;
        Graph fiction = split.fictionGraph(), study = split.studyGraph();
        // The bell and the theme are both tenants'.
        QVERIFY(fiction.node(split.bell) && study.node(split.bell) && fiction.node(split.theme) && study.node(split.theme));
        const qint64 rings = fiction.link(split.bell, split.theme, "rings for", "Rung by hand in Fiction.");
        QVERIFY(rings > 0);
        QCOMPARE(noteOf(fiction, split.bell, rings), QString("Rung by hand in Fiction."));
        QCOMPARE(noteOf(study, split.bell, rings), QString("-"));
        QVERIFY(!Tenancy::ofEdge(split.library, rings).contains(split.tenantOf(split.study)));
        // Written in the other tenant too, each reads its own note.
        QCOMPARE(study.link(split.bell, split.theme, "rings for", "Rung by hand in Study."), rings);
        QCOMPARE(noteOf(study, split.bell, rings), QString("Rung by hand in Study."));
        QCOMPARE(noteOf(fiction, split.bell, rings), QString("Rung by hand in Fiction."));
        // Tied by hand to the node of the book both have, a thing of one gains no other tenant from it.
        QVERIFY(fiction.link(split.mara, whole(split).bookNode(split.both), "rings at") > 0);
        QVERIFY(!study.node(split.mara));
        const QStringList kept = memberships(split.library);
        QCOMPARE(rebuilt(split.library), kept);
    }

    void aRelationsNoteIsTheOneItsTenantWasTold() {
        Written split;
        Graph whole(split.library);
        const Graph fiction = split.fictionGraph(), study = split.studyGraph();
        // Learned in The Sea Wall (Fiction's), and said again in Wave Mechanics (Study's).
        const qint64 tolls = whole.link(split.theme, split.bell, "tolls for", "As The Sea Wall tells it.", GraphPlace{split.sea, 2, 0.2});
        QVERIFY(tolls > 0);
        QCOMPARE(noteOf(study, split.bell, tolls), QString("-"));
        QCOMPARE(whole.link(split.theme, split.bell, "tolls for", "As Wave Mechanics tells it.", GraphPlace{split.waves, 0, 0.5}), tolls);
        // Still The Sea Wall's, and seen from both, each with its own book's note.
        const QList<GraphEdge> all = whole.around(split.bell, 1).edges;
        QCOMPARE(std::ranges::find(all, tolls, &GraphEdge::id)->learned.bookId, split.sea);
        QCOMPARE(noteOf(fiction, split.bell, tolls), QString("As The Sea Wall tells it."));
        QCOMPARE(noteOf(study, split.bell, tolls), QString("As Wave Mechanics tells it."));
        // Said again in The Sea Wall, or written by hand in Fiction: Study reads what it did.
        QCOMPARE(whole.link(split.theme, split.bell, "tolls for", "The Sea Wall, again.", GraphPlace{split.sea, 2, 0.1}), tolls);
        QCOMPARE(noteOf(fiction, split.bell, tolls), QString("The Sea Wall, again."));
        Graph byHand = split.fictionGraph();
        QCOMPARE(byHand.link(split.theme, split.bell, "tolls for", "Fiction's own note."), tolls);
        QCOMPARE(noteOf(fiction, split.bell, tolls), QString("Fiction's own note."));
        QCOMPARE(noteOf(study, split.bell, tolls), QString("As Wave Mechanics tells it."));
        const QStringList kept = memberships(split.library);
        QCOMPARE(rebuilt(split.library), kept);
        // Wave Mechanics out of Study takes its saying along.
        QVERIFY(split.library.putInCollection(split.library.collectionNamed("Study/Physics")->id, split.waves, false));
        QVERIFY(!Tenancy::ofEdge(split.library, tolls).contains(split.tenantOf(split.study)));
        QCOMPARE(noteOf(split.studyGraph(), split.bell, tolls), QString("-"));
        const QStringList left = memberships(split.library);
        QCOMPARE(rebuilt(split.library), left);
    }

    void whatIsWrittenByHandTravelsWithIt() {
        Written split;
        Graph fiction = split.fictionGraph();
        const qint64 said = fiction.learn(split.theme, "summary", "What the keepers leave.");
        const qint64 rings = fiction.link(split.bell, split.theme, "rings for", "Rung by hand in Fiction.");
        Graph whole(split.library);
        const qint64 tolls = whole.link(split.theme, split.bell, "tolls for", "As The Sea Wall tells it.", GraphPlace{split.sea, 2, 0.2});
        whole.link(split.theme, split.bell, "tolls for", "As Wave Mechanics tells it.", GraphPlace{split.waves, 0, 0.5});
        QVERIFY(said > 0 && rings > 0 && tolls > 0);
        QTemporaryDir there;
        Library other(there.filePath("Library"));
        QVERIFY(other.open());
        QVERIFY(SpaceReplay::apply(other, SpaceReplay::operations(split.library)));
        const QStringList here = memberships(split.library), arrived = memberships(other);
        QVERIFY2(arrived == here, qPrintable(differing(here, arrived)));
        const Graph fictionThere = Graph::forTenant(other, split.tenantOf(split.fiction));
        const Graph studyThere = Graph::forTenant(other, split.tenantOf(split.study));
        const qint64 theme = Graph(other).named({}, "theme", "Inheritance"), bell = Graph(other).named({}, "item", "The bell");
        const auto saysIt = [&](const Graph &graph) {
            return std::ranges::contains(graph.facts(theme), QString("What the keepers leave."), &GraphFact::value);
        };
        QVERIFY(saysIt(fictionThere) && !saysIt(studyThere));
        QCOMPARE(studyThere.node(theme)->summary, QString("What a wave keeps of the one before."));
        const auto noteThere = [&](const Graph &graph, const QString &kind) {
            for (const GraphEdge &edge : graph.around(bell, 1).edges)
                if (edge.kind == kind)
                    return edge.note;
            return QString("-");
        };
        QCOMPARE(noteThere(fictionThere, "rings for"), QString("Rung by hand in Fiction."));
        QCOMPARE(noteThere(studyThere, "rings for"), QString("-"));
        QCOMPARE(noteThere(fictionThere, "tolls for"), QString("As The Sea Wall tells it."));
        QCOMPARE(noteThere(studyThere, "tolls for"), QString("As Wave Mechanics tells it."));
    }

    void aRenameByHandIsItsTenantsName() {
        Written split;
        Graph fiction = split.fictionGraph(), study = split.studyGraph();
        QVERIFY(fiction.edit(split.theme, "Legacy", fiction.node(split.theme)->summary));
        QCOMPARE(fiction.node(split.theme)->name, QString("Legacy"));
        QVERIFY(fiction.node(split.theme)->aliases().contains("Inheritance"));
        // The other tenant, and the thing itself, keep the name the books taught.
        QCOMPARE(study.node(split.theme)->name, QString("Inheritance"));
        QVERIFY(!study.node(split.theme)->aliases().contains("Legacy"));
        QCOMPARE(whole(split).node(split.theme)->name, QString("Inheritance"));
        QCOMPARE(study.node(split.theme)->summary, QString("What a wave keeps of the one before."));
        // Study's own name for it, and Fiction keeps its.
        QVERIFY(study.edit(split.theme, "Heritage", study.node(split.theme)->summary));
        QCOMPARE(study.node(split.theme)->name, QString("Heritage"));
        QCOMPARE(fiction.node(split.theme)->name, QString("Legacy"));
        QVERIFY(!fiction.node(split.theme)->aliases().contains("Heritage"));
        // Renamed again: the latest is its name, the one before one of its names.
        QVERIFY(fiction.edit(split.theme, "Bequest", fiction.node(split.theme)->summary));
        QCOMPARE(fiction.node(split.theme)->name, QString("Bequest"));
        QVERIFY(fiction.node(split.theme)->aliases().contains("Legacy") && fiction.node(split.theme)->aliases().contains("Inheritance"));
        // Not to a name another thing goes by in the tenant; one only the other tenant gives is free.
        Graph all = whole(split);
        const qint64 memory = all.upsert({}, "theme", "Memory", {}, {}, GraphPlace{split.sea, 3, 0.1});
        QVERIFY(!fiction.edit(memory, "Bequest", {}));
        QVERIFY(fiction.edit(memory, "Heritage", {}));
        // Found again by a tenant's name, for what a book of that tenant says next.
        QCOMPARE(all.named({}, "theme", "Bequest"), split.theme);
        const QStringList kept = memberships(split.library);
        QCOMPARE(rebuilt(split.library), kept);
        // In the one tenant of the whole library it goes by its own name, and
        // the tenants' are other names of it; renamed there, that is its name.
        const OneTenant one;
        Graph library = Graph::ofLibrary(split.library);
        QCOMPARE(library.node(split.theme)->name, QString("Inheritance"));
        QVERIFY(library.node(split.theme)->aliases().contains("Bequest"));
        QVERIFY(library.edit(split.theme, "The keepers' legacy", library.node(split.theme)->summary));
        QCOMPARE(whole(split).node(split.theme)->name, QString("The keepers' legacy"));
        QCOMPARE(library.node(split.theme)->name, QString("The keepers' legacy"));
    }

    void aRenameIsFoundByTheNameItsTenantGaveIt() {
        Written split;
        for (const qint64 book : {split.sea, split.both, split.waves, split.loose})
            QVERIFY(split.library.setReading(book, "read", 0, {}));
        const KnowledgeIndex before = KnowledgeIndex::build(split.library);
        Graph fiction = split.fictionGraph();
        const Graph study = split.studyGraph();
        QVERIFY(fiction.edit(split.theme, "Legacy", fiction.node(split.theme)->summary));
        // Searched: by the tenant's name there, and by the name it had; never in the other.
        QCOMPARE(idsOf(fiction.find("legacy")), QList<qint64>{split.theme});
        QCOMPARE(fiction.find("legacy").value(0).name, QString("Legacy"));
        QCOMPARE(idsOf(fiction.find("inheritance")), QList<qint64>{split.theme});
        QVERIFY(study.find("legacy").isEmpty());
        QCOMPARE(study.find("inheritance").value(0).name, QString("Inheritance"));
        // What is known: a book of Fiction is found by it, one of Study's not.
        const auto knows = [&split](const KnowledgeIndex &index) {
            const QHash<qint64, KnownMatch> found = index.find("legacy");
            return found.contains(split.sea) && !found.contains(split.waves) && found.value(split.sea).name == "Legacy"
                   && index.find("inheritance").contains(split.waves);
        };
        QVERIFY(knows(KnowledgeIndex::build(split.library)));
        QVERIFY(knows(KnowledgeIndex::since(split.library, &before)));
        QVERIFY(knows(KnowledgeIndex::forSearch(split.library, "legacy")));
    }

    void aRenameTravelsWithIt() {
        Written split;
        Graph fiction = split.fictionGraph();
        QVERIFY(fiction.edit(split.theme, "Legacy", fiction.node(split.theme)->summary));
        QTemporaryDir there;
        Library other(there.filePath("Library"));
        QVERIFY(other.open());
        QVERIFY(SpaceReplay::apply(other, SpaceReplay::operations(split.library)));
        const QStringList here = memberships(split.library), arrived = memberships(other);
        QVERIFY2(arrived == here, qPrintable(differing(here, arrived)));
        const qint64 theme = Graph(other).named({}, "theme", "Inheritance");
        QCOMPARE(Graph::forTenant(other, split.tenantOf(split.fiction)).node(theme)->name, QString("Legacy"));
        QCOMPARE(Graph::forTenant(other, split.tenantOf(split.study)).node(theme)->name, QString("Inheritance"));
        QCOMPARE(idsOf(Graph::forTenant(other, split.tenantOf(split.fiction)).find("legacy")), QList<qint64>{theme});
    }

    void aBookInTwoTenantsIsOfferedTheThingsOfBoth() {
        Written split;
        Graph whole(split.library);
        const qint64 founding = whole.upsert({}, "concept", "Bell-founding", "Casting a bell in sand.", {}, GraphPlace{split.both, 0, 0.1});
        QVERIFY(founding > 0);
        for (const qint64 book : {split.sea, split.both, split.waves}) {
            GraphBuild::setBuiltTo(split.library, book, {book, 9, 1.0});
            split.library.setReading(book, "read", 5, {});
        }
        const auto offered = [&](qint64 book) {
            QSet<qint64> ids;
            for (const GraphNode &node : GraphLinks::offer(split.library, book, {book, 9, 1.0}).others)
                ids << node.id;
            return ids;
        };
        // The book both tenants have: the things of each tenant's other books.
        const QSet<qint64> ofBoth = offered(split.both);
        QVERIFY(ofBoth.contains(split.mara) && ofBoth.contains(split.viscosity));
        // A book of one tenant: never the other's.
        const QSet<qint64> ofSea = offered(split.sea);
        QVERIFY(ofSea.contains(founding) && !ofSea.contains(split.viscosity));
        QVERIFY(!offered(split.waves).contains(split.mara));
        // The same offer each time.
        QCOMPARE(GraphLinks::offer(split.library, split.both, {split.both, 9, 1.0}).mark(),
                 GraphLinks::offer(split.library, split.both, {split.both, 9, 1.0}).mark());
    }
};

QTEST_GUILESS_MAIN(TenantWritesTests)
#include "tenantwrites_tests.moc"
