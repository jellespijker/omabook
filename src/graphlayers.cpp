#include "graphlayers.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <optional>
#include <tuple>
#include <utility>

#include "hlc.h"
#include "spacereducer.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

using GraphLayers::Layer;

template <typename Thing> [[nodiscard]] bool alive(const Thing &thing) { return !SpaceReducer::isRemoved(thing.alive, thing.removed); }

[[nodiscard]] QString text(const QHash<QString, SpaceStamp> &fields, const QString &field) { return fields.value(field).value; }

// What the library itself puts in the graph: the owner's books, authors,
// series and tags are theirs, not something a reader comes to.
[[nodiscard]] bool ofTheLibrary(const QString &kind) {
    return kind == "book"_L1 || kind == "author"_L1 || kind == "series"_L1 || kind == "tag"_L1;
}

// An id below zero, the same for the same thing of the same space each time,
// so a picture of it keeps its places and nothing of it is taken for one's own.
[[nodiscard]] qint64 idOf(const QString &space, const QString &gid) {
    const QByteArray hash = QCryptographicHash::hash((space + u'\n' + gid).toUtf8(), QCryptographicHash::Sha1);
    quint64 value = 0;
    for (const char byte : hash.first(6))
        value = value << 8U | static_cast<quint64>(static_cast<quint8>(byte));
    return -static_cast<qint64>(value) - 1;
}

[[nodiscard]] bool everything([[maybe_unused]] const GraphPlace &place) { return true; }

[[nodiscard]] bool inOrder(const std::pair<GraphNode, GraphPlace> &a, const std::pair<GraphNode, GraphPlace> &b) {
    return std::tuple{a.second.section, a.second.fraction, a.first.id} < std::tuple{b.second.section, b.second.fraction, b.first.id};
}

// Reads one space's state as a layer: its books as they are here, and of
// what it says only what `known` says is known.
class Reading {
public:
    Reading(const Library &library, SubscribedSpace::Subscription space, std::span<const SpaceOperation> ops, const GraphKnown &known)
        : m_library(&library), m_known(&known), m_space(std::move(space)), m_books(SpaceReducer::sameAs(ops)),
          m_things(SpaceReducer::sameAs(ops, "node"_L1)) {}

    [[nodiscard]] Layer layer(const ReducedState &state) {
        Layer layer{
            .space = m_space.space, .name = m_space.name, .owner = m_space.ownerName, .nodes = {}, .facts = {}, .met = {}, .edges = {}};
        for (auto node = state.nodes.cbegin(); node != state.nodes.cend(); ++node)
            take(layer, node.key(), *node);
        for (auto fact = state.facts.cbegin(); fact != state.facts.cend(); ++fact)
            take(layer, *fact);
        for (auto edge = state.edges.cbegin(); edge != state.edges.cend(); ++edge)
            take(layer, edge.key(), *edge);
        // A thing goes by the names it was given at a known place, and no other.
        for (GraphNode &node : layer.nodes) {
            QStringList names;
            for (const GraphFact &fact : layer.facts.value(node.id))
                if (fact.kind == "alias"_L1 && !names.contains(fact.value, Qt::CaseInsensitive))
                    names << fact.value;
            if (!names.isEmpty())
                node.data.insert("aliases", QJsonArray::fromStringList(names));
        }
        return layer;
    }

private:
    // Where a moment of a book of the space is here, when it is known: of a
    // book the reader has, at a place they know.
    [[nodiscard]] std::optional<GraphMoment> here(const QString &bookGid, const SpaceMoment &moment) {
        if (bookGid.isEmpty())
            return std::nullopt;
        const QString book = m_books.value(bookGid, bookGid);
        if (!m_here.contains(book))
            m_here.insert(book, SubscribedSpace::bookHere(*m_library, m_space.space, book));
        const GraphMoment at{{m_here.value(book), moment.section, moment.fraction}, moment.anchor};
        return at.place.bookId > 0 && (*m_known)(at.place) ? std::optional(at) : std::nullopt;
    }

    [[nodiscard]] qint64 thing(const QString &gid) const { return m_ids.value(m_things.value(gid, gid)); }

    void take(Layer &layer, const QString &gid, const ReducedNode &reduced) {
        const QString kind = text(reduced.fields, u"kind"_s);
        const QString name = text(reduced.fields, u"name"_s);
        // A world a plugin keeps is no book's graph.
        if (!alive(reduced) || kind.isEmpty() || name.isEmpty() || ofTheLibrary(kind) || !text(reduced.fields, u"scope"_s).isEmpty())
            return;
        QList<GraphMoment> met;
        for (auto seen = reduced.seen.cbegin(); seen != reduced.seen.cend(); ++seen)
            if (const auto at = here(seen.key(), *seen))
                met << *at;
        if (met.isEmpty())
            return;
        QJsonObject data = QJsonDocument::fromJson(text(reduced.fields, u"data"_s).toUtf8()).object();
        data.remove("aliases");
        data.insert("layer", QJsonObject{{"space", m_space.space}, {"name", m_space.name}, {"owner", m_space.ownerName}});
        const GraphNode node{.id = idOf(m_space.space, gid),
                             .scope = m_space.space,
                             .kind = kind,
                             .name = name,
                             .summary = {},
                             .data = data,
                             .bookId = 0,
                             .updated = Hlc::seconds(std::max<qint64>(0, reduced.alive.hlc))};
        m_ids.insert(gid, node.id);
        layer.met.insert(node.id, met);
        layer.nodes << node;
    }

    void take(Layer &layer, const ReducedFact &fact) {
        const qint64 node = thing(fact.nodeGid);
        if (!alive(fact) || node == 0 || (fact.kind != "summary"_L1 && fact.kind != "alias"_L1) || fact.value.isEmpty())
            return;
        if (const auto at = here(fact.bookGid, fact.moment))
            layer.facts[node] << GraphFact{.id = ++m_facts, .node = node, .kind = fact.kind, .value = fact.value, .revealed = *at};
    }

    void take(Layer &layer, const QString &gid, const ReducedEdge &edge) {
        const qint64 source = thing(edge.sourceGid);
        const qint64 target = thing(edge.targetGid);
        if (!alive(edge) || source == 0 || target == 0 || source == target || edge.kind.isEmpty())
            return;
        if (const auto at = here(edge.bookGid, edge.learned))
            layer.edges << GraphEdge{.id = idOf(m_space.space, gid),
                                     .source = source,
                                     .target = target,
                                     .kind = edge.kind,
                                     .note = edge.note.value,
                                     .learned = at->place,
                                     .anchor = at->anchor};
    }

    // Both outlive the reading (read() makes one per space and is done with it).
    const Library *m_library;
    const GraphKnown *m_known;
    SubscribedSpace::Subscription m_space;
    QHash<QString, QString> m_books;  // a book of the space, by the id its same-as links stand for
    QHash<QString, QString> m_things; // and a thing
    QHash<QString, qint64> m_here;    // a book of the space, here (0 when it isn't)
    QHash<QString, qint64> m_ids;     // a thing of the space, in the layer
    qint64 m_facts = 0;
};

} // namespace

QList<std::pair<GraphNode, GraphPlace>> Layer::metIn(qint64 bookId) const {
    QList<std::pair<GraphNode, GraphPlace>> list;
    for (const GraphNode &node : nodes)
        for (const GraphMoment &at : met.value(node.id))
            if (at.place.bookId == bookId)
                list.append({Graph::asKnown(node, facts.value(node.id), bookId, everything), at.place});
    std::ranges::sort(list, inOrder);
    return list;
}

GraphView Layer::ofBook(qint64 bookId) const {
    GraphView view;
    QSet<qint64> ids;
    for (auto &[node, place] : metIn(bookId)) {
        ids << node.id;
        view.nodes << std::move(node);
    }
    std::ranges::copy_if(edges, std::back_inserter(view.edges),
                         [&ids](const GraphEdge &edge) { return ids.contains(edge.source) && ids.contains(edge.target); });
    return view;
}

QList<Graph::Sighting> Layer::sightings() const {
    QList<Graph::Sighting> found;
    for (const GraphNode &node : nodes)
        for (const GraphMoment &at : met.value(node.id))
            found.append({.node = node, .bookId = at.place.bookId, .at = at});
    return found;
}

QList<Layer> GraphLayers::read(const Library &library, const GraphKnown &known) {
    QList<Layer> layers;
    const GraphKnown ofABook = [&known](const GraphPlace &at) { return at.bookId > 0 && known(at); };
    for (const SubscribedSpace::Subscription &space : SubscribedSpace::list(library)) {
        const QList<SpaceOperation> ops = SubscribedSpace::operations(library, space.space);
        layers << Reading(library, space, ops, ofABook).layer(SpaceReducer::reduce(ops));
    }
    return layers;
}

QString GraphLayers::from(const GraphNode &node) {
    const QJsonObject layer = node.data.value("layer").toObject();
    if (layer.isEmpty())
        return {};
    const QString owner = layer.value("owner").toString();
    return owner.isEmpty() ? u"the shared graph of %1"_s.arg(layer.value("name").toString()) : u"%1's shared graph"_s.arg(owner);
}

GraphView GraphLayers::with(GraphView view, const QList<Layer> &layers, qint64 bookId) {
    for (const Layer &layer : layers) {
        const GraphView part = layer.ofBook(bookId);
        view.nodes += part.nodes;
        view.edges += part.edges;
    }
    return view;
}

void GraphLayers::addTo(QList<std::pair<GraphNode, GraphPlace>> &met, QHash<qint64, QList<GraphFact>> &facts, const QList<Layer> &layers,
                        qint64 bookId) {
    if (layers.isEmpty())
        return;
    for (const Layer &layer : layers) {
        for (const auto &[node, place] : layer.metIn(bookId)) {
            met.append({node, place});
            facts.insert(node.id, layer.facts.value(node.id));
        }
    }
    std::ranges::stable_sort(met, inOrder);
}

QJsonArray GraphLayers::toJson(const QList<Layer> &layers, qint64 bookId) {
    QJsonArray list;
    for (const Layer &layer : layers) {
        QJsonObject object = layer.ofBook(bookId).toJson();
        object.insert("space", layer.space);
        object.insert("name", layer.name);
        object.insert("owner", layer.owner);
        list << object;
    }
    return list;
}
