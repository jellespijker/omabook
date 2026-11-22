#include "graphtimes.h"

#include <QHash>
#include <QJsonArray>
#include <expected>
#include <optional>
#include <utility>

#include "chronology.h"
#include "chronologybranch.h"
#include "eventtime.h"

using namespace Qt::StringLiterals;

namespace {

using Chronology::Whose;
using GraphBuild::Timed;

// A part of a time left as the answer's template has it: not given.
[[nodiscard]] bool blank(const QJsonValue &part) {
    return part.isNull() || (part.isString() && part.toString().trimmed().isEmpty()) || (part.isArray() && part.toArray().isEmpty())
           || (part.isBool() && !part.toBool());
}

// The time a model gave an event, read as the chronology reads one: none
// when it gave nothing, and what is wrong with it when it is no time.
[[nodiscard]] std::expected<std::optional<EventTime>, QString> timeIn(const QJsonValue &value) {
    if (value.isUndefined() || value.isNull())
        return std::nullopt;
    if (!value.isObject())
        return std::unexpected(u"a time is a JSON object"_s);
    QJsonObject given = value.toObject();
    for (const QString &key : given.keys())
        if (blank(given.value(key)))
            given.remove(key);
    if (given.isEmpty())
        return std::nullopt;
    auto time = EventTime::parse(given);
    if (!time)
        return std::unexpected(time.error());
    return std::optional(std::move(*time));
}

// The timeline a model named: the book's own when it named none, its
// series', real history, or a world by its name.
[[nodiscard]] std::expected<std::pair<Whose, QString>, QString> timelineIn(const QJsonValue &value) {
    if (value.isUndefined() || value.isNull())
        return std::pair{Whose::Book, QString()};
    if (!value.isString())
        return std::unexpected(u"a timeline is named in words"_s);
    const QString said = value.toString().simplified();
    const QString word = said.toLower();
    if (word.isEmpty() || word == "reader"_L1)
        return std::pair{Whose::Book, QString()};
    if (word == "real history"_L1)
        return std::pair{Whose::History, QString()};
    if (const auto whose = Chronology::whose(word))
        return std::pair{*whose, QString()};
    if (said.size() > 80)
        return std::unexpected(u"a timeline's name is a few words"_s);
    return std::pair{Whose::World, said};
}

// One answer's times, laid into the graph of one book.
class Laying {
public:
    Laying(Graph &graph, const Library &library, qint64 bookId, const QList<GraphBuild::Passage> &passages)
        : m_graph(&graph), m_library(&library), m_bookId(bookId), m_passages(&passages) {}

    // An event's time and timeline, revealed where the text says them.
    void event(const QJsonObject &node, qint64 id) {
        const int section = GraphBuild::sectionOf(node.value("section"_L1));
        const GraphMoment met = GraphBuild::placeOf(m_bookId, *m_passages, section, {node.value("name"_L1).toString()});
        const GraphMoment revealed = GraphBuild::revealedAt(m_bookId, *m_passages, section, node.value("quote"_L1), met);
        const auto time = timeIn(node.value("when"_L1));
        if (time && *time) {
            const auto at = GraphBuild::momentOf(m_bookId, *m_passages, section, {(*time)->said});
            count(Chronology::setWhen(*m_graph, id, **time, at.value_or(revealed)), timed.times);
        } else if (!time) {
            ++timed.dropped;
        }
        const auto line = timelineIn(node.value("timeline"_L1));
        if (!line)
            ++timed.dropped;
        const auto [whose, called] = line.value_or(std::pair{Whose::Book, QString()});
        // On a branch, and so never on what it branches off.
        if (const qint64 branch = branchNamed(whose, called); branch > 0) {
            count(Chronology::placeOn(*m_graph, id, branch, met), timed.placed);
            return;
        }
        place(id, whose, called, met);
        // A date on the real calendar is a date of real history, where books
        // that have nothing else in common meet; but not in a book that
        // departs from it, where only what the model puts there by name is.
        if (time && *time && (*time)->on.gregorian() && whose != Whose::History && !offHistory())
            place(id, Whose::History, {}, met);
    }

    // A timeline the model says departs from another at an event (an
    // alternate history, an alternative universe), proposed for the reader
    // to take or not, and revealed where the text says it.
    void branch(const QJsonObject &node, const GraphBuild::NodeOf &nodeOf) {
        const QString name = node.value("name"_L1).toString().simplified();
        const qint64 at = nodeOf(node.value("at"_L1).toString().simplified());
        const auto parent = timelineIn(node.value("branches_from"_L1));
        if (name.isEmpty() || name.size() > 120 || at == 0 || !parent || node.value("branches_from"_L1).toString().trimmed().isEmpty()) {
            ++timed.dropped;
            return;
        }
        const int section = GraphBuild::sectionOf(node.value("section"_L1));
        const GraphMoment met = GraphBuild::placeOf(m_bookId, *m_passages, section, {name});
        const GraphMoment revealed = GraphBuild::revealedAt(m_bookId, *m_passages, section, node.value("quote"_L1), met);
        const qint64 known = branchNamed(parent->first, parent->second);
        const qint64 from = known > 0 ? known : Chronology::timelineFor(*m_graph, *m_library, m_bookId, parent->first, parent->second, met);
        const auto made =
            Chronology::branch(*m_graph, {.timeline = 0, .name = name, .from = from, .at = at, .revealed = revealed, .proposed = true});
        if (!made) {
            ++timed.dropped;
            return;
        }
        m_branches.insert(name.toLower(), *made);
        if (parent->first == Whose::History)
            m_offHistory = true;
        ++timed.proposed;
    }

    // An edge of order between two events, which must both be events.
    void order(const QJsonObject &edge, const GraphBuild::NodeOf &nodeOf) {
        const QString from = edge.value("from"_L1).toString().simplified();
        const QString to = edge.value("to"_L1).toString().simplified();
        const qint64 source = nodeOf(from);
        const qint64 target = nodeOf(to);
        if (source == 0 || target == 0) {
            ++timed.dropped;
            return;
        }
        const int section = GraphBuild::sectionOf(edge.value("section"_L1));
        const GraphMoment first = GraphBuild::placeOf(m_bookId, *m_passages, section, {from});
        const GraphMoment second = GraphBuild::placeOf(m_bookId, *m_passages, section, {to});
        const GraphMoment learned = GraphBuild::revealedAt(m_bookId, *m_passages, section, edge.value("quote"_L1),
                                                           first.place.notAfter(second.place) ? second : first);
        const QString note = edge.value("inferred"_L1).toBool() ? Chronology::inferred() : QString();
        count(Chronology::order(*m_graph, source, edge.value("relation"_L1).toString(), target, learned, note), timed.orders);
    }

    Timed timed;

private:
    // A branch by the name the model gave it: one of this answer's, or one there is already.
    [[nodiscard]] qint64 branchNamed(Whose whose, const QString &called) const {
        if (whose != Whose::World || called.isEmpty())
            return 0;
        if (const qint64 made = m_branches.value(called.toLower()); made > 0)
            return made;
        const qint64 known = m_graph->named({}, Chronology::timelineKind(), called);
        const auto node = known > 0 ? m_graph->node(known) : std::nullopt;
        return node && node->data.value("timeline"_L1).toString() == Chronology::name(Whose::Branch) ? known : 0;
    }

    // Whether the book departs from real history: a branch of it was proposed in it.
    [[nodiscard]] bool offHistory() {
        if (!m_offHistory.has_value()) {
            m_offHistory = false;
            for (const auto &[node, at] : m_graph->metIn(m_bookId))
                if (node.kind == Chronology::timelineKind() && node.data.value("timeline"_L1).toString() == Chronology::name(Whose::Branch))
                    if (const auto parent = m_graph->node(Chronology::parentOf(*m_graph, node.id)))
                        m_offHistory = *m_offHistory || parent->data.value("timeline"_L1).toString() == Chronology::name(Whose::History);
        }
        return *m_offHistory;
    }

    void place(qint64 id, Whose whose, const QString &called, const GraphMoment &met) {
        const qint64 line = Chronology::timelineFor(*m_graph, *m_library, m_bookId, whose, called, met);
        if (line > 0)
            count(Chronology::placeOn(*m_graph, id, line, met), timed.placed);
    }

    // What the chronology kept is counted, what it refused is dropped.
    void count(const std::expected<qint64, QString> &kept, int &into) {
        if (kept)
            ++into;
        else
            ++timed.dropped;
    }

    Graph *m_graph;
    const Library *m_library;
    qint64 m_bookId;
    const QList<GraphBuild::Passage> *m_passages;
    QHash<QString, qint64> m_branches; // this answer's, by name
    std::optional<bool> m_offHistory;
};

} // namespace

Timed &Timed::operator+=(const Timed &other) {
    proposed += other.proposed;
    times += other.times;
    orders += other.orders;
    placed += other.placed;
    dropped += other.dropped;
    return *this;
}

QJsonObject Timed::toJson() const {
    return {{"times", times}, {"orders", orders}, {"placed", placed}, {"proposed", proposed}, {"dropped", dropped}};
}

bool GraphBuild::ofOrder(const QString &relation) { return Chronology::orderRelations().contains(relation.simplified().toLower()); }

Timed GraphBuild::applyTimes(Graph &graph, const Library &library, qint64 bookId, const QList<Passage> &passages, const QJsonObject &answer,
                             const NodeOf &nodeOf) {
    Laying laying(graph, library, bookId, passages);
    const QJsonArray nodes = answer.value("nodes"_L1).toArray();
    // The branches first, for the events the answer puts on them.
    for (qsizetype index = 0; index < nodes.size() && index < mostNodes; ++index)
        if (const QJsonObject node = nodes.at(index).toObject(); node.value("kind"_L1).toString().simplified().toLower() == "timeline"_L1)
            laying.branch(node, nodeOf);
    for (qsizetype index = 0; index < nodes.size() && index < mostNodes; ++index) {
        const QJsonObject node = nodes.at(index).toObject();
        const qint64 id = nodeOf(node.value("name"_L1).toString().simplified());
        const auto known = id > 0 ? graph.node(id) : std::nullopt;
        if (known && known->kind == "event"_L1)
            laying.event(node, id);
        else if (const auto time = timeIn(node.value("when"_L1)); !time || *time)
            ++laying.timed.dropped; // a time of what is no event
    }
    const QJsonArray edges = answer.value("edges"_L1).toArray();
    for (qsizetype index = 0; index < edges.size() && index < mostEdges; ++index)
        if (const QJsonObject edge = edges.at(index).toObject(); ofOrder(edge.value("relation"_L1).toString()))
            laying.order(edge, nodeOf);
    return laying.timed;
}
