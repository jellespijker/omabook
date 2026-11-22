#include "chronologyorder.h"

#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <ranges>
#include <utility>

#include "eventorder.h"

using namespace Qt::StringLiterals;

namespace {

using Chronology::Dated;
using Chronology::Revealed;
using Relation = Chronology::Orderer::Relation;

[[nodiscard]] Revealed ofTime(const Dated &dated) { return Chronology::revealed(dated.revealed.place); }

} // namespace

QList<Dated> Chronology::latestOfEachBook(const QList<Dated> &times) {
    QMap<qint64, Dated> latest;
    for (const Dated &dated : times) {
        if (!dated.time.on.any())
            continue;
        const auto kept = latest.constFind(dated.revealed.place.bookId);
        if (kept == latest.cend() || std::pair{ofTime(*kept), kept->fact} <= std::pair{ofTime(dated), dated.fact})
            latest.insert(dated.revealed.place.bookId, dated);
    }
    return latest.values();
}

namespace {

[[nodiscard]] QJsonObject momentJson(const GraphMoment &moment) {
    QJsonObject object{{"book", moment.place.bookId}, {"section", moment.place.section}, {"fraction", moment.place.fraction}};
    if (!moment.anchor.isEmpty())
        object.insert("anchor", moment.anchor);
    return object;
}

[[nodiscard]] std::optional<Relation> relationOf(const GraphEdge &edge) {
    Relation relation{.edge = edge.id,
                      .first = edge.source,
                      .second = edge.target,
                      .kind = Relation::Kind::Before,
                      .revealed = {edge.learned, edge.anchor}};
    if (edge.kind == "after"_L1)
        std::swap(relation.first, relation.second);
    else if (edge.kind == "during"_L1)
        relation.kind = Relation::Kind::During;
    else if (edge.kind == "same time as"_L1)
        relation.kind = Relation::Kind::Same;
    else if (edge.kind != "before"_L1)
        return std::nullopt;
    return relation;
}

} // namespace

QString Chronology::Orderer::Relation::name(Kind kind) {
    switch (kind) {
    case Kind::Before:
        return u"before"_s;
    case Kind::Same:
        return u"same time as"_s;
    case Kind::During:
        return u"during"_s;
    }
    return {};
}

Chronology::Lane Chronology::Orderer::order(const GraphNode &timeline, QList<qint64> events) const {
    return Arrangement(*this, std::move(events)).run(timeline);
}

std::optional<Dated> Chronology::chosen(const QList<Dated> &times) {
    const QList<Dated> latest = latestOfEachBook(times);
    if (latest.isEmpty())
        return std::nullopt;
    return *std::ranges::min_element(latest, {}, [](const Dated &dated) {
        return std::tuple{dated.revealed.place.bookId != 0, -static_cast<int>(dated.time.precision()), dated.time.approximate,
                          dated.revealed.place.bookId, dated.fact};
    });
}

QJsonObject Chronology::Source::toJson() const {
    QJsonObject object{{"event", event}, {"said", said}, {"revealed", momentJson(revealed)}};
    if (edge != 0)
        object.insert("edge", edge);
    if (fact != 0)
        object.insert("fact", fact);
    return object;
}

QString Chronology::Contradiction::name(Kind kind) {
    switch (kind) {
    case Kind::Cycle:
        return u"cycle"_s;
    case Kind::Date:
        return u"date"_s;
    case Kind::Times:
        return u"times"_s;
    case Kind::Calendars:
        return u"calendars"_s;
    }
    return {};
}

QString Chronology::Contradiction::rule() const {
    switch (kind) {
    case Kind::Cycle:
        return u"The relations go round in a circle: the later revealed wins, and the one revealed first is set aside."_s;
    case Kind::Date:
        return u"The times say otherwise: dates win over relations, and the relation is set aside."_s;
    case Kind::Times:
        return u"The books give the event times that can't both be: the time put in by hand wins, else the most precise, "
               "else the first book's."_s;
    case Kind::Calendars:
        return u"Two times tie the same calendars apart by different years: the first revealed is kept."_s;
    }
    return {};
}

QJsonObject Chronology::Contradiction::toJson() const {
    QJsonArray sourceList;
    for (const Source &source : sources)
        sourceList << source.toJson();
    return {{"kind", name(kind)},
            {"events", QJsonArray::fromVariantList(events | std::ranges::to<QVariantList>())},
            {"sources", sourceList},
            {"set_aside", setAside.toJson()},
            {"rule", rule()}};
}

QJsonObject Chronology::Item::toJson() const {
    QJsonObject object{{"event", event.id}, {"name", event.name}, {"rank", rank}};
    if (time) {
        QJsonObject when = time->time.toJson();
        when.insert("precision", EventTime::name(time->time.precision()));
        object.insert("time", when);
        object.insert("revealed", momentJson(time->revealed));
    }
    if (extent && extent->from)
        object.insert("from", *extent->from);
    if (extent && extent->to)
        object.insert("to", *extent->to);
    if (within != 0)
        object.insert("within", within);
    return object;
}

QJsonObject Chronology::Lane::toJson() const {
    QJsonArray itemList;
    for (const Item &item : items) {
        QJsonObject object = item.toJson();
        if (shared.contains(item.event.id))
            object.insert("shared", true);
        itemList << object;
    }
    QJsonArray contradictionList;
    for (const Contradiction &contradiction : contradictions)
        contradictionList << contradiction.toJson();
    const QJsonObject line{
        {"id", timeline.id}, {"name", timeline.name}, {"whose", timeline.data.value("timeline").toString()}, {"book", timeline.bookId}};
    QJsonObject lane{{"timeline", line}, {"calendar", calendar}, {"items", itemList}, {"contradictions", contradictionList}};
    if (split)
        lane.insert("branch", split->toJson());
    return lane;
}

Chronology::Orderer::Orderer(const Facts &facts) {
    QList<Dated> all;
    for (const GraphNode &event : facts.events) {
        m_events.insert(event.id, event);
        const QList<GraphPlace> places = facts.met.value(event.id);
        m_firstMet.insert(event.id, places.isEmpty() ? Revealed{true, 0, 0, 0}
                                                     : std::ranges::min(places | std::views::transform(&Chronology::revealed)));
        QList<Dated> times = facts.times.value(event.id);
        std::ranges::stable_sort(times, {}, [](const Dated &dated) { return std::pair{ofTime(dated), dated.fact}; });
        if (const auto time = chosen(times))
            m_chosen.insert(event.id, *time);
        all += times;
        m_times.insert(event.id, std::move(times));
    }
    // The ties between calendars are read in the order revealed, which says
    // which of two that disagree is kept.
    std::ranges::stable_sort(all, {}, [](const Dated &dated) { return std::pair{ofTime(dated), dated.fact}; });
    m_calendars = Calendars(all);
    QList<GraphEdge> edges = facts.edges;
    std::ranges::sort(edges, {}, &GraphEdge::id);
    for (const GraphEdge &edge : std::as_const(edges)) {
        const auto relation = relationOf(edge);
        if (!relation || relation->first == relation->second || !m_events.contains(relation->first) || !m_events.contains(relation->second))
            continue;
        m_relationsOf[relation->first].append(static_cast<int>(m_relations.size()));
        m_relationsOf[relation->second].append(static_cast<int>(m_relations.size()));
        m_byEdge.insert(relation->edge, static_cast<int>(m_relations.size()));
        m_relations.append(*relation);
    }
}

qint64 Chronology::Orderer::book(qint64 event) const { return std::get<1>(m_firstMet.value(event)); }

Chronology::Item Chronology::Orderer::apart(qint64 event) const {
    Item item{.event = m_events.value(event), .rank = -1, .time = {}, .extent = {}, .within = 0};
    const QList<Dated> times = m_times.value(event);
    if (const auto time = m_chosen.constFind(event); time != m_chosen.cend())
        item.time = *time;
    else if (!times.isEmpty())
        item.time = times.last();
    return item;
}

QList<std::pair<qint64, qint64>> Chronology::Orderer::ties() const {
    return m_relations | std::views::transform([](const Relation &relation) { return std::pair{relation.first, relation.second}; })
           | std::ranges::to<QList<std::pair<qint64, qint64>>>();
}
