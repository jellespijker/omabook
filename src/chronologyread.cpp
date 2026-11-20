#include "chronologyread.h"

#include <QCoreApplication>
#include <algorithm>
#include <ranges>

#include "chronicle.h"
#include "graphcontext.h"
#include "graphlayers.h"
#include "tenancy.h"

using namespace Chronology;
using namespace Qt::StringLiterals;
using Reading::From;

namespace {

// As many as a graph has: what an event involves is read of all of it.
constexpr int everyThing = 1'000'000;

// In the catalogs: bin/i18n reads what tr() is handed.
const auto tr = [](const char *text) { return QCoreApplication::translate("Chronology", text); };

// The books of what the chronology is opened from.
[[nodiscard]] QSet<qint64> booksOf(const Library &library, const From &from) {
    QSet<qint64> books;
    if (from.what == From::What::Book) {
        books.insert(from.bookId);
        return books;
    }
    if (from.what == From::What::Scope)
        return books;
    const auto collection = from.what == From::What::Collection ? library.collectionNamed(from.name) : std::nullopt;
    for (const Book &book : library.books())
        if (from.what == From::What::Collection ? collection && collection->contains(book)
                                                : book.meta.series.compare(from.name, Qt::CaseInsensitive) == 0)
            books.insert(book.id);
    return books;
}

// The name of a tenant, when a whole graph is shown a tenant at a time.
[[nodiscard]] QString nameOf(const Library &library, const ::Tenant &tenant) {
    return tenant == ::Tenant::unfiled() ? tr("Books in no collection") : Tenancy::nameOf(library, tenant);
}

// The tenant a book, a series or a collection is read in: the collection's,
// or that of its first book.
[[nodiscard]] ::Tenant tenantOf(const Library &library, const From &from, const QSet<qint64> &books) {
    if (from.what == From::What::Collection)
        if (const auto collection = library.collectionNamed(from.name))
            if (const auto tenant = Tenancy::ofCollection(library, collection->id))
                return *tenant;
    QList<qint64> sorted = books.values();
    std::ranges::sort(sorted);
    return from.what == From::What::Book ? Tenancy::ofReader(library, from.bookId) : Tenancy::ofReader(library, sorted.value(0));
}

void merge(Facts &into, const Facts &more) {
    into.events += more.events;
    into.timelines += more.timelines;
    for (auto times = more.times.cbegin(); times != more.times.cend(); ++times)
        into.times.insert(times.key(), *times);
    for (auto met = more.met.cbegin(); met != more.met.cend(); ++met)
        into.met.insert(met.key(), *met);
    into.edges += more.edges;
}

// Of these things and relations, what each event involves, of those known.
void involve(Reading::Read &read, const QList<GraphNode> &nodes, const QList<GraphEdge> &edges, const GraphKnown &known) {
    QHash<qint64, GraphNode> things;
    for (const GraphNode &node : nodes)
        if (Reading::involvedKinds().contains(node.kind))
            things.insert(node.id, node);
    for (const GraphEdge &edge : edges) {
        if (!known(edge.learned))
            continue;
        const auto tie = [&](qint64 event, qint64 thing) {
            const auto found = read.events.find(event);
            if (found == read.events.end() || !things.contains(thing) || found->involved.contains(thing))
                return;
            found->involved.append(thing);
            read.involved.insert(thing, things.value(thing));
        };
        tie(edge.source, edge.target);
        tie(edge.target, edge.source);
    }
}

void keepEvents(Reading::Read &read, const Facts &facts) {
    for (const GraphNode &event : facts.events) {
        Reading::Event &kept = read.events[event.id];
        kept.node = event;
        kept.from = GraphLayers::from(event);
        kept.met = facts.met.value(event.id);
        for (const GraphPlace &place : std::as_const(kept.met))
            if (place.bookId > 0)
                kept.books.insert(place.bookId);
    }
}

} // namespace

const QStringList &Reading::involvedKinds() {
    static const QStringList kinds{u"character"_s, u"place"_s, u"group"_s, u"theme"_s};
    return kinds;
}

Reading::Read Reading::read(const Library &library, const From &from) {
    const GraphKnown known = GraphContext::readerKnows(library);
    Read told;
    told.books = booksOf(library, from);
    const QString scope = from.what == From::What::Scope ? from.scope : QString();
    // A whole graph a tenant at a time; a book, a series or a collection in the tenant of its books.
    const QList<Chronology::Tenant> tenants =
        from.what == From::What::Scope
            ? Chronology::tenants(library, scope)
            : QList<Chronology::Tenant>{Chronology::Tenant{.scope = scope, .in = tenantOf(library, from, told.books)}};
    for (const Chronology::Tenant &tenant : tenants) {
        const Graph graph = Graph::forTenant(library, tenant.in);
        Facts facts = Chronology::read(library, known, tenant);
        // The shared graphs are of the general graph, and are the reader's as far as they read, in the tenant's books.
        const QList<GraphLayers::Layer> layers = scope.isEmpty() ? GraphContext::layers(graph) : QList<GraphLayers::Layer>{};
        const QSet<qint64> books = Tenancy::booksOf(library, tenant.in);
        for (const GraphLayers::Layer &layer : layers)
            merge(facts, within(Chronology::read(layer), books));
        keepEvents(told, facts);
        told.parts.append({.name = tenants.size() > 1 ? nameOf(library, tenant.in) : QString(), .chronicle = chronicle(facts)});
        const GraphView whole = graph.whole(scope.isNull() ? u""_s : scope, everyThing);
        involve(told, whole.nodes, whole.edges, known);
        for (const GraphLayers::Layer &layer : layers)
            involve(told, layer.nodes, layer.edges, [](const GraphPlace &) { return true; });
    }
    return told;
}
