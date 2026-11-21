#include "worldlayers.h"

#include <QHash>
#include <QJsonArray>
#include <algorithm>
#include <span>

#include "graphcontext.h"
#include "graphlayers.h"
#include "heldspaces.h"
#include "library.h"
#include "spacereducer.h"
#include "spaceworlds.h"
#include "subscribedspace.h"
#include "tenancy.h"

using namespace Qt::StringLiterals;

namespace {

using WorldLayers::Layer;

using HeldSpaces::Held;

// The layers of one space's worlds.
void layersOf(const Library &library, const Held &space, bool known, const GraphKnown &knows, QList<Layer> &layers) {
    const QList<SpaceWorlds::Declared> worlds = SpaceWorlds::declared(space.ops);
    if (worlds.isEmpty())
        return;
    const QHash<QString, QString> books = SpaceReducer::sameAs(space.ops);
    const ReducedState state = SpaceReducer::reduce(space.ops);
    for (const SpaceWorlds::Declared &world : worlds) {
        if (world.scope.isEmpty() || (!space.me.isEmpty() && world.author == space.me))
            continue;
        const QString book = books.value(world.book, world.book);
        const qint64 here = SubscribedSpace::bookHere(library, space.space.space, book);
        const std::optional<Book> copy = here > 0 ? library.book(here) : std::nullopt;
        SubscribedSpace::Subscription from = space.space;
        if (space.kind == "shared"_L1)
            from.ownerName = space.nameOf(world.author);
        Layer layer{.scope = SpaceWorlds::layerName(space.space.space, world.scope),
                    .space = space.space.space,
                    .kind = space.kind,
                    .role = space.role,
                    .owner = from.ownerName,
                    .plugin = world.plugin,
                    .of = world.scope,
                    .book = copy && !copy->formats.isEmpty() ? here : 0,
                    .title = state.books.value(book).fields.value(u"title"_s).value,
                    .view = {}};
        if (known && here > 0)
            layer.view = GraphLayers::readWorld(library, from, space.ops, knows, {.scope = world.scope, .named = layer.scope}).ofBook(here);
        layers << layer;
    }
}

} // namespace

QJsonObject WorldLayers::Layer::toJson() const {
    return {{"scope", scope},   {"space", space}, {"kind", kind}, {"role", role},  {"owner", owner},
            {"plugin", plugin}, {"of", of},       {"book", book}, {"title", title}};
}

QList<Layer> WorldLayers::list(const Library &library, bool known) {
    QList<Layer> layers;
    const GraphKnown knows = known ? GraphContext::readerKnows(library) : GraphKnown();
    for (const Held &space : HeldSpaces::held(library))
        layersOf(library, space, known, knows, layers);
    return layers;
}

std::optional<Layer> WorldLayers::named(const Library &library, const QString &scope) {
    if (!SpaceWorlds::isLayerName(scope))
        return std::nullopt;
    for (Layer &layer : list(library))
        if (layer.scope == scope)
            return std::move(layer);
    return std::nullopt;
}

QStringList WorldLayers::scopes(const Graph &graph) {
    QStringList scopes = graph.scopes();
    for (const Layer &layer : list(graph.library(), false))
        if (!scopes.contains(layer.scope))
            scopes << layer.scope;
    return scopes;
}

QStringList WorldLayers::scopes(const Library &library) {
    QStringList all = scopes(Graph::ofLibrary(library));
    for (const Tenant &tenant : Tenancy::tenants(library))
        for (const QString &scope : Graph::forTenant(library, tenant).scopes())
            if (!all.contains(scope))
                all << scope;
    return all;
}

Graph WorldLayers::writing(const Library &library, const QJsonObject &thing) {
    const QString scope = thing.value("scope").toString();
    return scope.isEmpty() ? Graph::ofReader(library, thing.value("book").toInteger()) : Graph::ofScope(library, scope);
}

Graph WorldLayers::linking(const Library &library, qint64 one, qint64 other, qint64 bookId) {
    const Graph whole(library);
    const auto first = whole.node(one);
    const auto second = whole.node(other);
    if (first && second && !first->scope.isEmpty() && first->scope == second->scope)
        return Graph::ofScope(library, first->scope);
    return bookId > 0 ? Graph::ofReader(library, bookId) : Graph::ofThing(library, one);
}

bool WorldLayers::merge(const Library &library, qint64 keep, qint64 other) { return linking(library, keep, other, 0).merge(keep, other); }

QJsonArray WorldLayers::find(const Library &library, const Search &search) {
    const bool general = search.everywhere || search.scope.isEmpty();
    const auto inItsTenant = [&] { return search.tenant ? Graph::forTenant(library, *search.tenant) : Graph::ofLibrary(library); };
    const Graph graph = general ? inItsTenant() : Graph(library);
    QJsonArray nodes;
    for (const GraphNode &node : graph.find(search.words, search.kind, search.everywhere ? std::nullopt : std::optional(search.scope),
                                            std::max(1, search.limit)))
        nodes << node.toJson();
    return nodes;
}

GraphView WorldLayers::whole(const Graph &graph, const QString &scope, int limit) {
    const std::optional<Layer> layer = named(graph.library(), scope);
    if (!layer)
        return graph.whole(scope, limit);
    GraphView view = layer->view;
    if (view.nodes.size() > limit)
        view.nodes.resize(limit);
    return view;
}

bool WorldLayers::copy(const Library &library, const QString &from, const QString &to) {
    Graph graph(library);
    if (SpaceWorlds::isLayerName(to))
        return false;
    const std::optional<Layer> layer = named(library, from);
    if (!layer)
        return graph.copyScope(from, to);
    const Library::Batch batch(library);
    if (to.isEmpty() || graph.scopes().contains(to))
        return false;
    QHash<qint64, qint64> copies;
    for (const GraphNode &node : layer->view.nodes) {
        QJsonObject data = node.data;
        data.remove("layer");
        copies.insert(node.id, graph.upsert(to, node.kind, node.name, node.summary, data));
    }
    for (const GraphEdge &edge : layer->view.edges)
        if (copies.contains(edge.source) && copies.contains(edge.target))
            graph.link(copies.value(edge.source), copies.value(edge.target), edge.kind, edge.note);
    return !layer->view.nodes.isEmpty();
}
