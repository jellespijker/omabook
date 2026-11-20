#include "eventorder.h"

#include <QMap>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <numeric>
#include <ranges>
#include <set>
#include <span>
#include <tuple>
#include <utility>

using Chronology::Adjacency;
using Chronology::Arrangement;
using Chronology::Revealed;
using Relation = Chronology::Orderer::Relation;

namespace {

template <typename Arcs> [[nodiscard]] Adjacency adjacency(int nodes, const Arcs &arcs, bool reversed) {
    Adjacency packed{.start = QList<int>(nodes + 1, 0), .to = {}};
    for (const auto &[from, to] : arcs)
        ++packed.start[(reversed ? to : from) + 1];
    std::partial_sum(packed.start.cbegin(), packed.start.cend(), packed.start.begin());
    packed.to.resize(packed.start.last());
    QList<int> next = packed.start;
    for (const auto &[from, to] : arcs)
        packed.to[next[reversed ? to : from]++] = reversed ? from : to;
    return packed;
}

// The nodes in the order a depth-first walk is done with them.
[[nodiscard]] QList<int> finished(const Adjacency &graph) {
    const auto nodes = static_cast<int>(graph.start.size() - 1);
    QList<int> order;
    order.reserve(nodes);
    QList<bool> seen(nodes, false);
    QList<std::pair<int, int>> stack;
    for (const int root : std::views::iota(0, nodes)) {
        if (seen[root])
            continue;
        seen[root] = true;
        stack.append({root, graph.start[root]});
        while (!stack.isEmpty()) {
            auto &[node, next] = stack.last();
            if (next == graph.start[node + 1]) {
                order.append(node);
                stack.removeLast();
                continue;
            }
            const int other = graph.to[next++];
            if (!seen[other]) {
                seen[other] = true;
                stack.append({other, graph.start[other]});
            }
        }
    }
    return order;
}

// The strongly connected components (Kosaraju): each node's, numbered from 0.
[[nodiscard]] QList<int> stronglyConnected(int nodes, const QList<std::pair<int, int>> &arcs) {
    const QList<int> order = finished(adjacency(nodes, arcs, false));
    const Adjacency back = adjacency(nodes, arcs, true);
    QList<int> component(nodes, -1);
    int count = 0;
    QList<int> stack;
    for (const int root : order | std::views::reverse) {
        if (component[root] >= 0)
            continue;
        component[root] = count;
        stack.append(root);
        while (!stack.isEmpty()) {
            const int node = stack.takeLast();
            for (const int other : std::span(back.to).subspan(back.start[node], back.start[node + 1] - back.start[node]))
                if (component[other] < 0) {
                    component[other] = count;
                    stack.append(other);
                }
        }
        ++count;
    }
    return component;
}

} // namespace

Arrangement::Arrangement(const Orderer &orderer, QList<qint64> events) : m_orderer(&orderer), m_events(std::move(events)) {
    std::ranges::sort(m_events);
    m_events.erase(std::ranges::unique(m_events).begin(), m_events.end());
    m_events.removeIf([&orderer](qint64 event) { return !orderer.m_events.contains(event); });
    for (const auto [at, event] : std::views::enumerate(m_events))
        m_index.insert(event, static_cast<int>(at));
    QSet<int> ours;
    for (const qint64 event : std::as_const(m_events))
        for (const int at : orderer.m_relationsOf.value(event))
            if (const Relation &relation = orderer.m_relations.at(at); node(relation.first) >= 0 && node(relation.second) >= 0)
                ours.insert(at);
    m_relations = ours.values();
    std::ranges::sort(m_relations);
}

Chronology::Lane Arrangement::run(const GraphNode &timeline) {
    m_lane.timeline = timeline;
    chooseScale();
    checkTimes();
    checkDates();
    build();
    resolve();
    place();
    return std::move(m_lane);
}

void Arrangement::chooseScale() {
    QSet<QString> calendars;
    for (const qint64 event : std::as_const(m_events))
        if (const auto time = m_orderer->m_chosen.constFind(event); time != m_orderer->m_chosen.cend()) {
            calendars.insert(time->time.on.calendar);
            for (const EventTime::Reckoning &other : time->time.also)
                calendars.insert(other.calendar);
        }
    QStringList names = calendars.values();
    std::ranges::sort(names, {}, [](const QString &name) { return std::pair{name != EventTime::gregorianName(), name}; });
    qsizetype most = 0;
    for (const QString &name : std::as_const(names)) {
        const Calendars::Scale scale = m_orderer->m_calendars.scale(name);
        const auto count = std::ranges::count_if(m_events, [this, &scale](qint64 event) {
            const auto time = m_orderer->m_chosen.constFind(event);
            return time != m_orderer->m_chosen.cend() && scale.extent(time->time);
        });
        if (count > most) {
            most = count;
            m_scale = scale;
        }
    }
    m_lane.calendar = most > 0 ? m_scale.calendar() : QString();
    m_extents = m_events | std::views::transform([this, most](qint64 event) -> std::optional<Extent> {
                    const auto time = m_orderer->m_chosen.constFind(event);
                    return most > 0 && time != m_orderer->m_chosen.cend() ? m_scale.extent(time->time) : std::nullopt;
                })
                | std::ranges::to<QList<std::optional<Extent>>>();
}

void Arrangement::checkTimes() {
    for (const auto [at, event] : std::views::enumerate(m_events)) {
        const auto chosen = m_orderer->m_chosen.constFind(event);
        const std::optional<Extent> mine = m_extents.at(at);
        if (chosen == m_orderer->m_chosen.cend() || !mine)
            continue;
        for (const Dated &other : latestOfEachBook(m_orderer->m_times.value(event))) {
            const auto there = other.fact == chosen->fact ? std::nullopt : m_scale.extent(other.time);
            if (there && !there->meets(*mine))
                report(Contradiction::Kind::Times, {event}, {timeSource(event, *chosen), timeSource(event, other)},
                       timeSource(event, other));
        }
    }
}

void Arrangement::checkDates() {
    for (const int at : std::as_const(m_relations)) {
        const Relation &relation = m_orderer->m_relations.at(at);
        const auto &first = m_extents.at(node(relation.first));
        const auto &second = m_extents.at(node(relation.second));
        if (!first || !second)
            continue;
        // One thing before another can't be after it; at the same time, or during it, they meet.
        const auto firstTime = source(relation.first);
        const auto secondTime = source(relation.second);
        if (!firstTime || !secondTime || (relation.kind == Relation::Kind::Before ? !second->before(*first) : first->meets(*second)))
            continue;
        m_setAside.insert(relation.edge);
        report(Contradiction::Kind::Date, {relation.first, relation.second}, {source(relation), *firstTime, *secondTime}, source(relation));
    }
}

void Arrangement::build() {
    for (const auto &extent : std::as_const(m_extents))
        if (extent && extent->from)
            m_points.append(*extent->from);
    std::ranges::sort(m_points);
    m_points.erase(std::ranges::unique(m_points).begin(), m_points.end());
    const auto base = static_cast<int>(m_events.size());
    for (const int at : std::views::iota(0, std::max(0, static_cast<int>(m_points.size()) - 1)))
        m_arcs.append({.from = base + at, .to = base + at + 1, .strict = false, .origin = 0});
    for (const auto [at, extent] : std::views::enumerate(m_extents)) {
        const int event = static_cast<int>(at);
        if (extent && extent->from)
            m_arcs.append({.from = base + pointAt(*extent->from), .to = event, .strict = false, .origin = 0});
        if (const int after = extent && extent->to ? pointAt(*extent->to) : -1; after >= 0)
            m_arcs.append({.from = event, .to = base + after, .strict = true, .origin = 0});
    }
    for (const int at : std::as_const(m_relations)) {
        const Relation &relation = m_orderer->m_relations.at(at);
        if (!kept(relation))
            continue;
        const int first = node(relation.first);
        const int second = node(relation.second);
        if (relation.kind == Relation::Kind::Before) {
            m_arcs.append({.from = first, .to = second, .strict = true, .origin = relation.edge});
        } else if (relation.kind == Relation::Kind::Same) {
            m_arcs.append({.from = first, .to = second, .strict = false, .origin = relation.edge});
            m_arcs.append({.from = second, .to = first, .strict = false, .origin = relation.edge});
        } else {
            during(relation);
        }
    }
    m_active = QList<bool>(m_arcs.size(), true);
}

void Arrangement::during(const Relation &relation) {
    const int inner = node(relation.first);
    const int outer = node(relation.second);
    const auto base = static_cast<int>(m_events.size());
    // It begins no sooner than what it is during, and ends before what follows that.
    m_arcs.append({.from = outer, .to = inner, .strict = false, .origin = relation.edge});
    for (const int at : m_orderer->m_relationsOf.value(relation.second)) {
        const Relation &next = m_orderer->m_relations.at(at);
        if (next.kind == Relation::Kind::Before && next.first == relation.second && kept(next) && node(next.second) >= 0)
            m_arcs.append({.from = inner, .to = node(next.second), .strict = true, .origin = relation.edge});
    }
    // Of no time of its own, it is within the other's.
    const auto &outerTime = m_extents.at(outer);
    if (!outerTime || m_extents.at(inner))
        return;
    if (outerTime->from)
        m_arcs.append({.from = base + pointAt(*outerTime->from), .to = inner, .strict = false, .origin = relation.edge});
    if (const int after = outerTime->to ? pointAt(*outerTime->to) : -1; after >= 0)
        m_arcs.append({.from = inner, .to = base + after, .strict = true, .origin = relation.edge});
}

int Arrangement::pointAt(qint64 minute) const {
    const auto found = std::ranges::lower_bound(m_points, minute);
    return found == m_points.cend() ? -1 : static_cast<int>(found - m_points.cbegin());
}

QList<int> Arrangement::components() const {
    QList<std::pair<int, int>> arcs;
    for (const auto [at, arc] : std::views::enumerate(m_arcs))
        if (m_active.at(at))
            arcs.append({arc.from, arc.to});
    return stronglyConnected(nodes(), arcs);
}

void Arrangement::resolve() {
    for (bool broke = true; broke;) {
        broke = false;
        const QList<int> component = components();
        QList<std::pair<int, int>> leaving; // each node's active arcs, by number
        for (const auto [at, arc] : std::views::enumerate(m_arcs))
            if (m_active.at(at))
                leaving.append({arc.from, static_cast<int>(at)});
        const Adjacency arcs = adjacency(nodes(), leaving, false);
        // Every circle has an arc a walk through the graph takes back to
        // where it still is: those close the circles, as a rule.
        const Walk walk = walked(arcs);
        for (const int at : walk.back)
            if (m_active.at(at) && intact(at, walk) && breakCircle(circleThrough(at, arcs, component)))
                broke = true;
        broke = broke || breakAny(arcs, component);
    }
}

bool Arrangement::breakAny(const Adjacency &arcs, const QList<int> &component) {
    // The first strict arc within each component closes a circle.
    QMap<int, int> closing;
    for (const auto [at, arc] : std::views::enumerate(m_arcs))
        if (m_active.at(at) && arc.strict && component.at(arc.from) == component.at(arc.to) && !closing.contains(component.at(arc.from)))
            closing.insert(component.at(arc.from), static_cast<int>(at));
    bool broke = false;
    for (const int at : std::as_const(closing))
        if (breakCircle(circleThrough(at, arcs, component)))
            broke = true;
    return broke;
}

Arrangement::Walk Arrangement::walked(const Adjacency &arcs) const {
    enum class Seen : std::uint8_t { Not, OnTheWay, Done };
    QList<Seen> seen(nodes(), Seen::Not);
    Walk walk{.back = {}, .cameBy = QList<int>(nodes(), -1)};
    QList<std::pair<int, int>> stack; // a node, and the next of its arcs to follow
    for (const int root : std::views::iota(0, nodes())) {
        if (seen[root] != Seen::Not)
            continue;
        seen[root] = Seen::OnTheWay;
        stack.append({root, arcs.start[root]});
        while (!stack.isEmpty()) {
            auto &[node, next] = stack.last();
            if (next == arcs.start[node + 1]) {
                seen[node] = Seen::Done;
                stack.removeLast();
                continue;
            }
            const int at = arcs.to[next++];
            const int other = m_arcs.at(at).to;
            if (seen[other] == Seen::OnTheWay) {
                walk.back.append(at);
            } else if (seen[other] == Seen::Not) {
                seen[other] = Seen::OnTheWay;
                walk.cameBy[other] = at;
                stack.append({other, arcs.start[other]});
            }
        }
    }
    return walk;
}

bool Arrangement::intact(int back, const Walk &walk) const {
    // The walk's way from where the arc goes back to, down to the arc.
    const Arc &arc = m_arcs.at(back);
    for (int node = arc.from; node != arc.to; node = m_arcs.at(walk.cameBy.at(node)).from)
        if (walk.cameBy.at(node) < 0 || !m_active.at(walk.cameBy.at(node)))
            return false;
    return true;
}

QList<int> Arrangement::circleThrough(int closing, const Adjacency &arcs, const QList<int> &component) const {
    // The shortest way back from the arc's end to its start, within its
    // component, along the arcs not set aside; none when there is no way back.
    const Arc &last = m_arcs.at(closing);
    constexpr int unseen = -2;
    constexpr int start = -1;
    QList<int> cameBy(nodes(), unseen); // the arc each node was come to by
    cameBy[last.to] = start;
    QList<int> frontier{last.to};
    for (qsizetype done = 0; done < frontier.size() && cameBy.at(last.from) == unseen; ++done) {
        const int here = frontier.at(done);
        for (const int at : std::span(arcs.to).subspan(arcs.start[here], arcs.start[here + 1] - arcs.start[here])) {
            const int there = m_arcs.at(at).to;
            if (m_active.at(at) && component.at(there) == component.at(here) && cameBy.at(there) == unseen) {
                cameBy[there] = at;
                frontier.append(there);
            }
        }
    }
    if (cameBy.at(last.from) == unseen)
        return {};
    QList<int> circle{closing};
    for (int at = cameBy.at(last.from); at >= 0; at = cameBy.at(m_arcs.at(at).from))
        circle.append(at);
    return circle;
}

bool Arrangement::breakCircle(const QList<int> &arcs) {
    if (std::ranges::none_of(arcs, [this](int at) { return m_arcs.at(at).strict; }))
        return false; // at the same time, which is no contradiction
    bool dated = false;
    QSet<qint64> origins;
    QSet<qint64> events;
    for (const int at : arcs) {
        const Arc &arc = m_arcs.at(at);
        dated = dated || arc.origin == 0;
        if (arc.origin != 0)
            origins.insert(arc.origin);
        for (const int end : {arc.from, arc.to})
            if (end < m_events.size())
                events.insert(m_events.at(end));
    }
    QList<Relation> stated;
    for (const qint64 origin : std::as_const(origins))
        stated.append(m_orderer->m_relations.at(m_orderer->m_byEdge.value(origin)));
    if (stated.isEmpty())
        return false;
    std::ranges::stable_sort(stated, {},
                             [](const Relation &relation) { return std::pair{revealed(relation.revealed.place), relation.edge}; });
    QList<Source> sources =
        stated | std::views::transform([this](const Relation &relation) { return source(relation); }) | std::ranges::to<QList<Source>>();
    if (dated)
        for (const qint64 event : std::as_const(events))
            if (const auto time = m_extents.at(node(event)) ? source(event) : std::nullopt)
                sources.append(*time);
    report(dated ? Contradiction::Kind::Date : Contradiction::Kind::Cycle, events.values(), sources, source(stated.first()));
    setAside(stated.first().edge);
    return true;
}

void Arrangement::setAside(qint64 edge) {
    m_setAside.insert(edge);
    for (const auto [at, arc] : std::views::enumerate(m_arcs))
        if (arc.origin == edge)
            m_active[at] = false;
}

void Arrangement::place() {
    const QList<int> component = components();
    QList<Class> classes(component.isEmpty() ? 0 : std::ranges::max(component) + 1);
    const auto base = static_cast<int>(m_events.size());
    for (const auto [at, of] : std::views::enumerate(component)) {
        Class &one = classes[of];
        if (at >= base) {
            one.own = m_points.at(at - base);
            one.kind = 0;
            continue;
        }
        const qint64 event = m_events.at(at);
        if (const auto &extent = m_extents.at(at); extent && extent->from) {
            one.own = std::min(one.own.value_or(*extent->from), *extent->from);
            one.kind = 1;
        }
        one.met = std::min(one.met, m_orderer->m_firstMet.value(event));
        one.first = std::min(one.first, event);
        one.events.append(event);
    }
    for (const auto [at, arc] : std::views::enumerate(m_arcs))
        if (m_active.at(at) && component.at(arc.from) != component.at(arc.to)) {
            classes[component.at(arc.from)].next.append(component.at(arc.to));
            ++classes[component.at(arc.to)].before;
        }
    layOut(classes, hints(classes));
}

QList<int> Arrangement::topological(const QList<Class> &classes) {
    QList<int> order;
    QList<int> before = classes | std::views::transform(&Class::before) | std::ranges::to<QList<int>>();
    for (const auto [at, one] : std::views::enumerate(classes))
        if (one.before == 0)
            order.append(static_cast<int>(at));
    for (qsizetype done = 0; done < order.size(); ++done)
        for (const int next : classes[order[done]].next)
            if (--before[next] == 0)
                order.append(next);
    return order;
}

QList<qint64> Arrangement::hints(const QList<Class> &classes) {
    const QList<int> topological = Arrangement::topological(classes);
    // Its own time; else that of the latest that comes before it; else that
    // of the first that comes after it.
    QList<std::optional<qint64>> hint = classes | std::views::transform(&Class::own) | std::ranges::to<QList<std::optional<qint64>>>();
    for (const int at : std::as_const(topological))
        if (const std::optional<qint64> mine = hint[at])
            for (const int next : classes[at].next)
                if (!classes[next].own)
                    hint[next] = std::max(hint[next].value_or(*mine), *mine);
    const QList<std::optional<qint64>> forward = hint;
    for (const int at : topological | std::views::reverse)
        for (const int next : classes[at].next)
            if (const std::optional<qint64> theirs = hint[next]; theirs && !forward[at])
                hint[at] = std::min(hint[at].value_or(*theirs), *theirs);
    return hint | std::views::transform([](const std::optional<qint64> &one) { return one.value_or(std::numeric_limits<qint64>::min()); })
           | std::ranges::to<QList<qint64>>();
}

void Arrangement::layOut(QList<Class> &classes, const QList<qint64> &hint) {
    QHash<qint64, qint64> within;
    for (const int at : std::as_const(m_relations))
        if (const Relation &relation = m_orderer->m_relations.at(at); relation.kind == Relation::Kind::During && kept(relation))
            within.insert(relation.first, within.value(relation.first, relation.second));
    using Key = std::tuple<qint64, int, Revealed, qint64, int>;
    const auto key = [&classes, &hint](int at) { return Key{hint[at], classes[at].kind, classes[at].met, classes[at].first, at}; };
    std::set<Key> ready;
    for (const auto [at, one] : std::views::enumerate(classes))
        if (one.before == 0)
            ready.insert(key(static_cast<int>(at)));
    int rank = 0;
    while (!ready.empty()) {
        const int at = std::get<4>(*ready.begin());
        ready.erase(ready.begin());
        Class &one = classes[at];
        std::ranges::sort(one.events, {}, [this](qint64 event) { return std::pair{m_orderer->m_firstMet.value(event), event}; });
        for (const qint64 event : std::as_const(one.events))
            m_lane.items.append(
                {.event = m_orderer->event(event),
                 .rank = rank,
                 .time = m_orderer->m_chosen.contains(event) ? std::optional(m_orderer->m_chosen.value(event)) : std::nullopt,
                 .extent = m_extents.at(node(event)),
                 .within = within.value(event)});
        rank += one.events.isEmpty() ? 0 : 1;
        for (const int next : std::as_const(one.next))
            if (--classes[next].before == 0)
                ready.insert(key(next));
    }
}

Chronology::Source Arrangement::source(const Relation &relation) const {
    return {.edge = relation.edge,
            .fact = 0,
            .event = relation.first,
            .said = m_orderer->event(relation.first).name + u' ' + Relation::name(relation.kind) + u' '
                    + m_orderer->event(relation.second).name,
            .revealed = relation.revealed};
}

std::optional<Chronology::Source> Arrangement::source(qint64 event) const {
    const auto time = m_orderer->m_chosen.constFind(event);
    return time == m_orderer->m_chosen.cend() ? std::nullopt : std::optional(timeSource(event, *time));
}

Chronology::Source Arrangement::timeSource(qint64 event, const Dated &dated) {
    return {.edge = 0, .fact = dated.fact, .event = event, .said = dated.time.written(), .revealed = dated.revealed};
}

void Arrangement::report(Contradiction::Kind kind, QList<qint64> events, QList<Source> sources, const Source &setAside) {
    std::ranges::sort(events);
    std::ranges::stable_sort(sources, {}, [](const Source &one) { return std::tuple{revealed(one.revealed.place), one.edge, one.fact}; });
    m_lane.contradictions.append({.kind = kind, .events = std::move(events), .sources = std::move(sources), .setAside = setAside});
}
