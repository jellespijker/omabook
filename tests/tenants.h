#pragma once

// A library split in two tenants for the tests of tenancy (#94) and of what
// reads the graph in one (#95).

#include <QTemporaryDir>
#include <algorithm>
#include <initializer_list>

#include "graph.h"
#include "library.h"
#include "spaces.h"
#include "tenancy.h"

// A library split in two at the top, Fiction and Study (with Physics inside
// it), a book in both, one in neither; an author who wrote in both, a theme
// both share, and what each book taught.
struct Split {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    qint64 sea = 0, both = 0, waves = 0, loose = 0;
    qint64 fiction = 0, study = 0;
    qint64 mara = 0, bell = 0, theme = 0, viscosity = 0, note = 0, ann = 0, bo = 0;
    qint64 keeps = 0, echoes = 0, wrote = 0, seaFact = 0, wavesFact = 0, noteFact = 0, bothFact = 0;

    Split() {
        library.open();
        sea = book("The Sea Wall", "Ann Author");
        both = book("Both Shores", "Ann Author");
        waves = book("Wave Mechanics", "Bo Writer");
        loose = book("Loose Leaves", "Cy Guide");
        fiction = *library.addCollection("Fiction");
        study = *library.addCollection("Study");
        const qint64 physics = *library.addCollection("Study/Physics");
        library.putInCollection(fiction, sea, true);
        library.putInCollection(fiction, both, true);
        library.putInCollection(physics, both, true);
        library.putInCollection(physics, waves, true);

        Graph graph(library);
        for (const qint64 each : {sea, both, waves, loose})
            graph.bookNode(each);
        mara = graph.upsert({}, "character", "Mara", "Walks the sea wall.", {}, GraphPlace{sea, 1, 0.5});
        bell = graph.upsert({}, "item", "The bell", {}, {}, GraphPlace{both, 0, 0.2});
        theme = graph.upsert({}, "theme", "Inheritance", {}, {}, GraphPlace{sea, 2, 0.1});
        graph.upsert({}, "theme", "Inheritance", {}, {}, GraphPlace{waves, 0, 0.3});
        seaFact = graph.learn(theme, "summary", "Handed from keeper to keeper.", GraphMoment{{sea, 2, 0.1}, "handed"});
        wavesFact = graph.learn(theme, "summary", "What a wave keeps of the one before.", GraphMoment{{waves, 0, 0.3}, "keeps"});
        viscosity = graph.upsert({}, "concept", "Viscosity", "How thick a flow is.", {}, GraphPlace{waves, 1, 0.0});
        note = graph.upsert({}, "topic", "Tides in general", "Put in by hand.");
        noteFact = graph.learn(note, "alias", "Tides");
        keeps = graph.link(mara, bell, "keeps", {}, GraphPlace{sea, 1, 0.6});
        echoes = graph.link(viscosity, mara, GraphEdge::acrossMark() + "echoes", {}, GraphPlace{waves, 1, 0.0});
        ann = graph.named({}, "author", "Ann Author");
        bo = graph.named({}, "author", "Bo Writer");
        wrote = edgeBetween(ann, graph.bookNode(sea));
        bothFact = graph.learn(bell, "alias", "The old bell", GraphMoment{{both, 0, 0.3}, "old bell"});
    }
    qint64 book(const QString &title, const QString &author) {
        BookMeta meta;
        meta.title = title;
        meta.authors = {author};
        return library.addEntry(meta, true).id;
    }
    [[nodiscard]] qint64 edgeBetween(qint64 source, qint64 target) const {
        for (const GraphEdge &edge : Graph(library).around(source, 1).edges)
            if (edge.source == source && edge.target == target)
                return edge.id;
        return 0;
    }
    [[nodiscard]] Tenant tenantOf(qint64 collection) const { return Tenant::ofCollection(Spaces::collectionId(library, collection)); }
    // These collections' tenants, in the order Tenancy::ofNode and the others answer them.
    [[nodiscard]] QList<Tenant> in(std::initializer_list<qint64> collections) const {
        QList<Tenant> all;
        for (const qint64 collection : collections)
            all << tenantOf(collection);
        std::ranges::sort(all, {}, &Tenant::globalId);
        return all;
    }
    // Sea Wall out of Fiction and into Study.
    [[nodiscard]] bool moveSeaToStudy() {
        return library.putInCollection(fiction, sea, false) && library.putInCollection(study, sea, true);
    }
    // Both Shores out of Physics, and so of Study: in Fiction alone.
    [[nodiscard]] bool takeBothOutOfStudy() { return library.putInCollection(library.collectionNamed("Study/Physics")->id, both, false); }
};
