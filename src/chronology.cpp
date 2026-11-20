#include "chronology.h"

#include <QJsonArray>
#include <algorithm>
#include <ranges>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

using Chronology::Known;

constexpr auto eventKind = "event"_L1;

[[nodiscard]] bool everything([[maybe_unused]] const GraphPlace &place) { return true; }

// Whether a thing is known: met at a known place, or of no book at all.
[[nodiscard]] bool met(const Graph &graph, qint64 node, const GraphKnown &known) {
    const QList<GraphPlace> places = graph.seen(node);
    return places.isEmpty() || std::ranges::any_of(places, known);
}

// How a relation of order reads from one of its ends.
[[nodiscard]] QString readFrom(const QString &relation, bool outgoing) {
    if (outgoing || relation == "same time as"_L1)
        return relation;
    if (relation == "before"_L1)
        return u"after"_s;
    if (relation == "after"_L1)
        return u"before"_s;
    return u"includes"_s; // the other is during this one
}

[[nodiscard]] QJsonObject momentJson(const GraphMoment &moment) {
    QJsonObject object{{"book", moment.place.bookId}, {"section", moment.place.section}, {"fraction", moment.place.fraction}};
    if (!moment.anchor.isEmpty())
        object.insert("anchor", moment.anchor);
    return object;
}

[[nodiscard]] QJsonObject withRevealed(QJsonObject object, const GraphMoment &moment) {
    if (moment.place.bookId > 0)
        object.insert("revealed", momentJson(moment));
    return object;
}

// The times known of an event, in the order revealed: by book and place,
// what no book says last.
[[nodiscard]] QList<Chronology::Dated> timesOf(qint64 event, const QList<GraphFact> &facts, const GraphKnown &known) {
    QList<Chronology::Dated> times;
    for (const GraphFact &fact : facts)
        if (const auto time = fact.when(); time && fact.node == event && known(fact.revealed.place))
            times.append({.time = *time, .revealed = fact.revealed, .fact = fact.id});
    std::ranges::stable_sort(times, {}, [](const Chronology::Dated &dated) {
        const GraphPlace &at = dated.revealed.place;
        return std::tuple{at.bookId == 0, at.bookId, at.section, at.fraction};
    });
    return times;
}

// What is known of an event, from its facts, the relations it has, and the
// things at their other ends that are known (each as it is known).
[[nodiscard]] Known read(GraphNode event, const QList<GraphFact> &facts, const QList<GraphEdge> &edges,
                         const QHash<qint64, GraphNode> &others, const GraphKnown &known) {
    Known told{.event = std::move(event), .times = {}, .order = {}, .timelines = {}};
    told.times = timesOf(told.event.id, facts, known);
    for (const GraphEdge &edge : edges) {
        const bool outgoing = edge.source == told.event.id;
        const auto other = others.constFind(outgoing ? edge.target : edge.source);
        if ((!outgoing && edge.target != told.event.id) || other == others.cend() || !known(edge.learned))
            continue;
        const GraphMoment learned{edge.learned, edge.anchor};
        if (edge.kind == Chronology::onRelation() && outgoing && other->kind == Chronology::timelineKind())
            told.timelines.append({.edge = edge.id, .timeline = *other, .revealed = learned});
        else if (Chronology::orderRelations().contains(edge.kind) && other->kind == eventKind)
            told.order.append(
                {.edge = edge.id, .other = *other, .relation = readFrom(edge.kind, outgoing), .revealed = learned, .note = edge.note});
    }
    return told;
}

[[nodiscard]] std::expected<GraphNode, QString> ofKind(const Graph &graph, qint64 id, const QString &kind) {
    const auto node = graph.node(id);
    if (!node)
        return std::unexpected(u"There is no thing %1 in the graph."_s.arg(id));
    if (node->kind != kind)
        return std::unexpected(u"%1 is a %2, not a %3."_s.arg(node->name, node->kind, kind));
    return *node;
}

[[nodiscard]] std::expected<qint64, QString> linked(qint64 edge) {
    if (edge == 0)
        return std::unexpected(u"The relation could not be kept."_s);
    return edge;
}

} // namespace

const QStringList &Chronology::orderRelations() {
    static const QStringList relations{u"before"_s, u"after"_s, u"during"_s, u"same time as"_s};
    return relations;
}

QString Chronology::name(Whose whose) {
    switch (whose) {
    case Whose::Book:
        return u"book"_s;
    case Whose::Series:
        return u"series"_s;
    case Whose::World:
        return u"world"_s;
    case Whose::History:
        return u"history"_s;
    case Whose::Reader:
        return u"reader"_s;
    }
    return {};
}

std::optional<Chronology::Whose> Chronology::whose(const QString &name) {
    for (const Whose whose : {Whose::Book, Whose::Series, Whose::World, Whose::History, Whose::Reader})
        if (Chronology::name(whose) == name)
            return whose;
    return std::nullopt;
}

std::optional<Chronology::Dated> Known::latest() const { return times.isEmpty() ? std::nullopt : std::optional(times.last()); }

QJsonObject Known::toJson() const {
    QJsonArray when;
    QJsonArray ordered;
    QJsonArray on;
    for (const Dated &dated : times) {
        QJsonObject time = dated.time.toJson();
        time.insert("precision", EventTime::name(dated.time.precision()));
        when << withRevealed({{"time", time}}, dated.revealed);
    }
    for (const Ordered &other : order) {
        QJsonObject object{{"relation", other.relation}, {"other", other.other.id}, {"other_name", other.other.name}};
        if (!other.note.isEmpty())
            object.insert("note", other.note);
        ordered << withRevealed(object, other.revealed);
    }
    for (const Placed &placed : timelines)
        on << withRevealed({{"timeline", placed.timeline.id},
                            {"name", placed.timeline.name},
                            {"whose", placed.timeline.data.value("timeline").toString()}},
                           placed.revealed);
    return {{"event", event.toJson()}, {"when", when}, {"order", ordered}, {"timelines", on}};
}

std::optional<Known> Chronology::of(const Library &library, qint64 event, const GraphKnown &known) {
    const Graph graph(library);
    const auto node = graph.node(event);
    if (!node || node->kind != eventKind || !met(graph, event, known))
        return std::nullopt;
    const GraphView near = graph.around(event, 1, 1000);
    const auto ids = near.nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>();
    const QHash<qint64, QList<GraphFact>> facts = graph.factsOf(ids);
    QHash<qint64, GraphNode> others;
    for (const GraphNode &other : near.nodes)
        if (other.id != event && (other.kind == eventKind || other.kind == timelineKind()) && met(graph, other.id, known))
            others.insert(other.id, Graph::asKnown(other, facts.value(other.id), 0, known));
    return read(Graph::asKnown(*node, facts.value(event), 0, known), facts.value(event), near.edges, others, known);
}

std::optional<Known> Chronology::of(const GraphLayers::Layer &layer, qint64 event) {
    QHash<qint64, GraphNode> nodes;
    for (const GraphNode &node : layer.nodes)
        nodes.insert(node.id, Graph::asKnown(node, layer.facts.value(node.id), 0, everything));
    if (nodes.value(event).kind != eventKind)
        return std::nullopt;
    return read(nodes.take(event), layer.facts.value(event), layer.edges, nodes, everything);
}

std::expected<qint64, QString> Chronology::setWhen(Graph &graph, qint64 event, const EventTime &time,
                                                   const std::optional<GraphMoment> &revealed) {
    if (const auto node = ofKind(graph, event, eventKind); !node)
        return std::unexpected(node.error());
    // Read back as it is kept, so a time that isn't one is never written.
    if (const auto checked = EventTime::parse(time.toJson()); !checked)
        return std::unexpected(checked.error());
    if (const qint64 fact = graph.learn(event, whenFact(), time.written(), revealed); fact > 0)
        return fact;
    return std::unexpected(u"The time could not be kept."_s);
}

std::expected<qint64, QString> Chronology::order(Graph &graph, qint64 event, const QString &relation, qint64 other,
                                                 const std::optional<GraphMoment> &learned) {
    const QString said = relation.simplified().toLower();
    if (!orderRelations().contains(said))
        return std::unexpected(u"\"%1\" is no relation of order (%2)."_s.arg(relation, orderRelations().join(", "_L1)));
    for (const qint64 end : {event, other})
        if (const auto node = ofKind(graph, end, eventKind); !node)
            return std::unexpected(node.error());
    if (event == other)
        return std::unexpected(u"An event is in no order with itself."_s);
    const std::optional<GraphPlace> place = learned ? std::optional(learned->place) : std::nullopt;
    return linked(graph.link(event, other, said, {}, place, learned ? learned->anchor : QString()));
}

std::expected<qint64, QString> Chronology::placeOn(Graph &graph, qint64 event, qint64 timeline, const std::optional<GraphMoment> &learned) {
    if (const auto node = ofKind(graph, event, eventKind); !node)
        return std::unexpected(node.error());
    if (const auto node = ofKind(graph, timeline, timelineKind()); !node)
        return std::unexpected(node.error());
    const std::optional<GraphPlace> place = learned ? std::optional(learned->place) : std::nullopt;
    return linked(graph.link(event, timeline, onRelation(), {}, place, learned ? learned->anchor : QString()));
}

qint64 Chronology::timeline(Graph &graph, Whose whose, const QString &called, const std::optional<GraphMoment> &seen) {
    QString name = called.simplified();
    // Real history is the one every book set in it shares, by whatever name.
    if (whose == Whose::History)
        for (const GraphNode &known : graph.find({}, timelineKind(), QString(), 1000))
            if (known.data.value("timeline").toString() == Chronology::name(whose)) {
                name = known.name;
                break;
            }
    if (name.isEmpty())
        name = whose == Whose::History ? u"Real history"_s : QString();
    const std::optional<GraphPlace> place = seen ? std::optional(seen->place) : std::nullopt;
    return graph.upsert({}, timelineKind(), name, {}, {{"timeline", Chronology::name(whose)}}, place, seen ? seen->anchor : QString(),
                        seen);
}
