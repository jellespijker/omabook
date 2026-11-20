#include "worldlayers.h"

#include <QHash>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <span>

#include "changelog.h"
#include "graphcontext.h"
#include "graphlayers.h"
#include "library.h"
#include "person.h"
#include "sharedspace.h"
#include "spacereducer.h"
#include "spaceworlds.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

using WorldLayers::Layer;

// A space as this library holds it, and who it is there.
struct Held {
    SubscribedSpace::Subscription space;
    QString kind;
    QString role;
    QString me; // whose worlds are this library's own, which are no layer here
    QList<SpaceOperation> ops;
};

[[nodiscard]] QString memberName(const Library &library, const QString &space, const QString &person) {
    QSqlQuery query = ChangeLog::query(library, "SELECT name FROM space_members WHERE space = ? AND person = ?", {space, person});
    return query.next() ? query.value(0).toString() : QString();
}

[[nodiscard]] QList<Held> held(const Library &library) {
    QList<Held> spaces;
    for (const SubscribedSpace::Subscription &space : SubscribedSpace::list(library))
        spaces << Held{.space = space,
                       .kind = u"published"_s,
                       .role = QString::fromLatin1(SubscribedSpace::readerRole),
                       .me = {},
                       .ops = SubscribedSpace::operations(library, space.space)};
    for (const QString &space : SharedSpace::spaces(library)) {
        SharedSpace::State state = SharedSpace::state(library, space);
        const QString me = SharedSpace::memberAs(library, state);
        spaces << Held{.space = {.space = space, .name = state.name, .owner = state.founder, .ownerName = {}, .collection = 0},
                       .kind = u"shared"_s,
                       .role = SharedSpace::roleName(state.roleOf(me)),
                       .me = me.isEmpty() && library.person() != nullptr ? library.person()->id() : me,
                       .ops = std::move(state.accepted)};
    }
    return spaces;
}

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
            from.ownerName = memberName(library, space.space.space, world.author);
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
    for (const Held &space : held(library))
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

QStringList WorldLayers::scopes(const Library &library) {
    QStringList scopes = Graph(library).scopes();
    for (const Layer &layer : list(library, false))
        if (!scopes.contains(layer.scope))
            scopes << layer.scope;
    return scopes;
}

GraphView WorldLayers::whole(const Library &library, const QString &scope, int limit) {
    const std::optional<Layer> layer = named(library, scope);
    if (!layer)
        return Graph(library).whole(scope, limit);
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
