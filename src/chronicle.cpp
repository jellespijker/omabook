#include "chronicle.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <numeric>
#include <ranges>

#include "chronology.h"

using namespace Qt::StringLiterals;
using Chronology::Facts;
using Chronology::Lane;

namespace {

// As many as a graph has: a chronology leaves out no event it knows.
constexpr int everyThing = 1'000'000;
constexpr auto eventKind = "event"_L1;

[[nodiscard]] bool everything([[maybe_unused]] const GraphPlace &place) { return true; }

// The scope as it is kept: the general graph's is empty, never null.
[[nodiscard]] QString kept(const QString &scope) { return scope.isNull() ? u""_s : scope; }

[[nodiscard]] bool ofChronology(const GraphEdge &edge) {
    return edge.kind == Chronology::onRelation() || Chronology::orderRelations().contains(edge.kind) || Chronology::ofBranch(edge.kind);
}

// The timeline of a book's own order, which is none of the graph's.
[[nodiscard]] GraphNode ownTimeline(qint64 book) {
    GraphNode timeline;
    timeline.kind = Chronology::timelineKind();
    timeline.bookId = book;
    timeline.data = {{"timeline", Chronology::name(Chronology::Whose::Book)}};
    return timeline;
}

// Which lanes are one group, as lanes are joined.
class Groups {
public:
    explicit Groups(qsizetype count) : m_parent(count) { std::ranges::iota(m_parent, 0); }
    [[nodiscard]] int top(int at) {
        while (m_parent[at] != at)
            at = m_parent[at] = m_parent[m_parent[at]];
        return at;
    }
    void join(int a, int b) {
        const int one = top(a);
        const int other = top(b);
        m_parent[std::max(one, other)] = std::min(one, other);
    }

private:
    QList<int> m_parent;
};

// The lanes that line up, by the first of them: through an event in both, a
// relation between their events, or a calendar they share.
[[nodiscard]] QMap<int, QList<int>> together(const Chronology::Orderer &orderer, const QList<Lane> &lanes) {
    Groups groups(lanes.size());
    QHash<qint64, int> laneOf; // the first lane each event is in
    for (const auto [at, lane] : std::views::enumerate(lanes))
        for (const Chronology::Item &item : lane.items)
            if (const auto first = laneOf.constFind(item.event.id); first != laneOf.cend())
                groups.join(static_cast<int>(at), *first);
            else
                laneOf.insert(item.event.id, static_cast<int>(at));
    for (const auto &[first, second] : orderer.ties())
        if (laneOf.contains(first) && laneOf.contains(second))
            groups.join(laneOf.value(first), laneOf.value(second));
    for (const auto [at, lane] : std::views::enumerate(lanes))
        for (const auto [other, next] : std::views::enumerate(lanes) | std::views::drop(at + 1))
            if (!lane.calendar.isEmpty() && !next.calendar.isEmpty() && orderer.calendars().tied(lane.calendar, next.calendar))
                groups.join(static_cast<int>(at), static_cast<int>(other));
    QMap<int, QList<int>> groupsOf;
    for (const int at : std::views::iota(0, static_cast<int>(lanes.size())))
        groupsOf[groups.top(at)].append(at);
    return groupsOf;
}

[[nodiscard]] QList<Chronology::Axis> axesOf(const Chronology::Orderer &orderer, const QList<Lane> &lanes) {
    QList<Chronology::Axis> axes;
    for (const QList<int> &members : together(orderer, lanes)) {
        if (members.size() == 1) {
            axes.append({.lanes = members, .order = lanes.at(members.first())});
            continue;
        }
        QList<qint64> events;
        for (const int at : members)
            for (const Chronology::Item &item : lanes.at(at).items)
                events.append(item.event.id);
        axes.append({.lanes = members, .order = orderer.order({}, std::move(events))});
    }
    return axes;
}

[[nodiscard]] QList<Chronology::Contradiction> calendarsDisagree(const Chronology::Orderer &orderer, const Facts &facts) {
    QHash<qint64, qint64> eventOf; // of each time
    for (auto times = facts.times.cbegin(); times != facts.times.cend(); ++times)
        for (const Chronology::Dated &dated : *times)
            eventOf.insert(dated.fact, times.key());
    QList<Chronology::Contradiction> found;
    for (const auto &[kept, other] : orderer.calendars().disagreements()) {
        const auto source = [&eventOf](const Chronology::Dated &dated) {
            return Chronology::Source{.edge = 0,
                                      .fact = dated.fact,
                                      .event = eventOf.value(dated.fact),
                                      .said = dated.time.written(),
                                      .revealed = dated.revealed};
        };
        QList<qint64> events{eventOf.value(kept.fact), eventOf.value(other.fact)};
        std::ranges::sort(events);
        events.erase(std::ranges::unique(events).begin(), events.end());
        found.append({.kind = Chronology::Contradiction::Kind::Calendars,
                      .events = events,
                      .sources = {source(kept), source(other)},
                      .setAside = source(other)});
    }
    return found;
}

[[nodiscard]] QList<Chronology::Dated> timesOf(const QList<GraphFact> &facts, const GraphKnown &known) {
    QList<Chronology::Dated> times;
    for (const GraphFact &fact : facts)
        if (const auto time = fact.when(); time && known(fact.revealed.place))
            times.append({.time = *time, .revealed = fact.revealed, .fact = fact.id});
    return times;
}

// Keeps a thing as known, with its times and places, unless it isn't met yet.
void take(Facts &facts, const GraphNode &node, const QList<GraphFact> &told, QList<GraphPlace> places, const GraphKnown &known) {
    const bool ofNoBook = places.isEmpty();
    places.removeIf([&known](const GraphPlace &place) { return !known(place); });
    if (!ofNoBook && places.isEmpty())
        return;
    const GraphNode asKnown = Graph::asKnown(node, told, 0, known);
    if (node.kind == Chronology::timelineKind()) {
        facts.timelines.append(asKnown);
        if (Chronology::hasStanding(told))
            facts.standing.insert(node.id, Chronology::standingOf(told, known));
        return;
    }
    facts.events.append(asKnown);
    facts.met.insert(node.id, std::move(places));
    facts.times.insert(node.id, timesOf(told, known));
}

// Only the relations whose two ends are kept.
void keepEdges(Facts &facts, QList<GraphEdge> edges, const GraphKnown &known) {
    QSet<qint64> things;
    for (const GraphNode &node : std::as_const(facts.events) + facts.timelines)
        things.insert(node.id);
    edges.removeIf([&](const GraphEdge &edge) {
        return !ofChronology(edge) || !known(edge.learned) || !things.contains(edge.source) || !things.contains(edge.target);
    });
    facts.edges = std::move(edges);
}

// The lanes of the timelines, each branch's with what it shares of its
// parent's: made once each, a parent's before its branches'.
class Branches {
public:
    Branches(const Chronology::Orderer &orderer, const QHash<qint64, GraphNode> &timelines, const QMap<qint64, QList<qint64>> &on,
             const QHash<qint64, Chronology::Split> &splits)
        : m_orderer(&orderer), m_timelines(&timelines), m_on(&on), m_splits(&splits) {}

    // Every lane there is: of each timeline with an event, and of each
    // branch whose split is placed; a branch right after its parent.
    [[nodiscard]] QList<Lane> lanes() {
        QList<qint64> roots;
        for (const qint64 id : m_timelines->keys())
            if (!branches(id))
                roots.append(id);
        std::ranges::sort(roots);
        QList<Lane> all;
        QSet<qint64> done;
        for (const qint64 root : std::as_const(roots))
            add(root, all, done);
        // What is left goes round in a circle of branches: each a lane of its own.
        QList<qint64> rest = m_timelines->keys();
        std::ranges::sort(rest);
        for (const qint64 id : std::as_const(rest))
            add(id, all, done);
        return all;
    }

private:
    // Whether a timeline branches off another that is drawn, at an event that is placed.
    [[nodiscard]] bool branches(qint64 id) const {
        const auto split = m_splits->constFind(id);
        return split != m_splits->cend() && m_timelines->contains(split->parent.id) && m_orderer->placed(split->at.id);
    }

    void add(qint64 id, QList<Lane> &all, QSet<qint64> &done) {
        if (done.contains(id))
            return;
        done.insert(id);
        if (const Lane lane = of(id); !lane.items.isEmpty() && (m_on->contains(id) || lane.split))
            all.append(lane);
        QList<qint64> children;
        for (auto split = m_splits->cbegin(); split != m_splits->cend(); ++split)
            if (split->parent.id == id && branches(split.key()))
                children.append(split.key());
        std::ranges::sort(children);
        for (const qint64 child : std::as_const(children))
            add(child, all, done);
    }

    // A timeline's lane: its own events, and a branch's parent's up to the split.
    Lane of(qint64 id) {
        if (const auto made = m_made.constFind(id); made != m_made.cend())
            return *made;
        QList<qint64> events = m_on->value(id);
        QSet<qint64> shared;
        std::optional<Chronology::Split> split;
        if (branches(id) && !m_making.contains(id)) {
            m_making.insert(id);
            const Lane parent = of(m_splits->value(id).parent.id);
            m_making.remove(id);
            split = m_splits->value(id);
            shared = sharedUpTo(parent, split->at.id);
            for (const qint64 event : std::as_const(shared))
                if (!events.contains(event))
                    events.append(event);
        }
        Lane lane = m_orderer->order(m_timelines->value(id), events);
        lane.split = std::move(split);
        lane.shared = std::move(shared);
        m_made.insert(id, lane);
        return lane;
    }

    // What a branch shares of its parent's lane: everything up to and at the
    // time of the event it splits at; that event alone when the parent hasn't it.
    [[nodiscard]] static QSet<qint64> sharedUpTo(const Lane &parent, qint64 at) {
        const auto split = std::ranges::find(parent.items, at, [](const Chronology::Item &item) { return item.event.id; });
        QSet<qint64> shared{at};
        if (split == parent.items.cend())
            return shared;
        for (const Chronology::Item &item : parent.items)
            if (item.rank <= split->rank)
                shared.insert(item.event.id);
        return shared;
    }

    const Chronology::Orderer *m_orderer;
    const QHash<qint64, GraphNode> *m_timelines;
    const QMap<qint64, QList<qint64>> *m_on;
    const QHash<qint64, Chronology::Split> *m_splits;
    QHash<qint64, Lane> m_made;
    QSet<qint64> m_making;
};

// The things a split can tie: the timelines and the events.
[[nodiscard]] QHash<qint64, GraphNode> thingsOf(const Facts &facts) {
    QHash<qint64, GraphNode> things;
    for (const GraphNode &node : std::as_const(facts.events) + facts.timelines)
        things.insert(node.id, node);
    return things;
}

} // namespace

QList<Chronology::Tenant> Chronology::tenants(const Library &library, const QString &scope) {
    QList<Tenant> parts;
    const QList<::Tenant> all = Tenancy::tenants(library);
    for (const ::Tenant &tenant : all)
        if (all.size() == 1 || !Tenancy::booksOf(library, tenant).isEmpty())
            parts.append(Tenant{.scope = scope, .in = tenant});
    if (parts.isEmpty())
        parts.append(Tenant{.scope = scope, .in = all.value(0, ::Tenant::unfiled())});
    return parts;
}

QJsonObject Chronology::Axis::toJson() const {
    QJsonObject object = order.toJson();
    object.remove("timeline");
    object.insert("lanes", QJsonArray::fromVariantList(lanes | std::ranges::to<QVariantList>()));
    return object;
}

QJsonObject Chronology::Chronicle::toJson() const {
    QJsonArray laneList;
    QJsonArray axisList;
    QJsonArray unplacedList;
    QJsonArray contradictionList;
    for (const Lane &lane : lanes)
        laneList << lane.toJson();
    for (const Axis &axis : axes)
        axisList << axis.toJson();
    for (const Item &item : unplaced)
        unplacedList << item.toJson();
    for (const Contradiction &contradiction : contradictions)
        contradictionList << contradiction.toJson();
    QJsonArray proposalList;
    for (const Proposal &proposal : proposals)
        proposalList << proposal.toJson();
    return {{"lanes", laneList},
            {"axes", axisList},
            {"unplaced", unplacedList},
            {"contradictions", contradictionList},
            {"proposals", proposalList}};
}

Chronology::Chronicle Chronology::chronicle(const Facts &facts) {
    const Orderer orderer(facts);
    Chronicle told;
    const QHash<qint64, Split> splits = splitsOf(facts.edges, thingsOf(facts), facts.standing);
    QHash<qint64, GraphNode> timelines;
    for (const GraphNode &timeline : facts.timelines) {
        // A branch the reader hasn't taken is no timeline yet; one proposed is asked about.
        const auto split = splits.constFind(timeline.id);
        const Standing standing = facts.standing.value(timeline.id, Standing::Accepted);
        if (standing == Standing::Proposed && split != splits.cend())
            told.proposals.append({.timeline = timeline, .split = *split});
        if (standing == Standing::Accepted)
            timelines.insert(timeline.id, timeline);
    }
    QMap<qint64, QList<qint64>> on; // each timeline's events, by timeline
    QSet<qint64> onOne;
    for (const GraphEdge &edge : facts.edges)
        if (edge.kind == onRelation() && timelines.contains(edge.target) && orderer.placed(edge.source)) {
            on[edge.target].append(edge.source);
            onOne.insert(edge.source);
        }
    QMap<qint64, QList<qint64>> own; // the events on no timeline, by book
    for (const GraphNode &event : facts.events)
        if (!orderer.placed(event.id))
            told.unplaced.append(orderer.apart(event.id));
        else if (!onOne.contains(event.id))
            own[orderer.book(event.id)].append(event.id);
    std::ranges::sort(told.unplaced, {}, [&orderer](const Item &item) { return std::pair{orderer.met(item.event.id), item.event.id}; });
    told.lanes = Branches(orderer, timelines, on, splits).lanes();
    for (auto events = own.cbegin(); events != own.cend(); ++events)
        told.lanes.append(orderer.order(ownTimeline(events.key()), *events));
    told.axes = axesOf(orderer, told.lanes);
    told.contradictions = calendarsDisagree(orderer, facts);
    return told;
}

Facts Chronology::read(const Library &library, const GraphKnown &known, const Tenant &tenant) {
    const Graph graph = Graph::forTenant(library, tenant.in);
    // The whole scope at once: its things and the relations among them, read in a few statements.
    const GraphView whole = graph.whole(kept(tenant.scope), everyThing);
    const QList<GraphNode> things =
        whole.nodes | std::views::filter([](const GraphNode &node) { return node.kind == eventKind || node.kind == timelineKind(); })
        | std::ranges::to<QList<GraphNode>>();
    const QHash<qint64, QList<GraphFact>> told =
        graph.factsOf(things | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>());
    Facts facts;
    for (const GraphNode &node : things)
        take(facts, node, told.value(node.id), graph.seen(node.id), known);
    keepEdges(facts, whole.edges, known);
    return facts;
}

Facts Chronology::read(const GraphLayers::Layer &layer) {
    Facts facts;
    for (const GraphNode &node : layer.nodes)
        if (node.kind == eventKind || node.kind == timelineKind()) {
            const QList<GraphPlace> places =
                layer.met.value(node.id) | std::views::transform(&GraphMoment::place) | std::ranges::to<QList<GraphPlace>>();
            take(facts, node, layer.facts.value(node.id), places, everything);
        }
    keepEdges(facts, layer.edges, everything);
    return facts;
}

Facts Chronology::within(Facts facts, const QSet<qint64> &books) {
    facts.events.removeIf([&](const GraphNode &event) {
        const QList<GraphPlace> met = facts.met.value(event.id);
        return !met.isEmpty()
               && std::ranges::none_of(met, [&books](const GraphPlace &at) { return at.bookId == 0 || books.contains(at.bookId); });
    });
    QSet<qint64> kept;
    for (const GraphNode &node : std::as_const(facts.events) + facts.timelines)
        kept.insert(node.id);
    facts.edges.removeIf([&kept](const GraphEdge &edge) { return !kept.contains(edge.source) || !kept.contains(edge.target); });
    return facts;
}
