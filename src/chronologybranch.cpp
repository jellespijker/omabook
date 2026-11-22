#include "chronologybranch.h"

#include <QDateTime>
#include <QJsonDocument>
#include <algorithm>
#include <tuple>

#include "chronology.h"

using namespace Qt::StringLiterals;

namespace {

using Chronology::Standing;

constexpr auto eventKind = "event"_L1;
// How far up a timeline's parents are followed, so that a circle still ends.
constexpr int deepest = 64;

[[nodiscard]] QJsonObject momentJson(const GraphMoment &moment) {
    QJsonObject object{{"book", moment.place.bookId}, {"section", moment.place.section}, {"fraction", moment.place.fraction}};
    if (!moment.anchor.isEmpty())
        object.insert("anchor", moment.anchor);
    return object;
}

// A word on a branch as it is kept: {"standing": "accepted", "said": ms}.
// When it was said tells two words revealed at the same place apart.
[[nodiscard]] QJsonObject wordOf(const GraphFact &fact) { return QJsonDocument::fromJson(fact.value.toUtf8()).object(); }

[[nodiscard]] std::optional<Standing> standingNamed(const QString &name) {
    for (const Standing standing : {Standing::Proposed, Standing::Accepted, Standing::Refused})
        if (Chronology::name(standing) == name)
            return standing;
    return std::nullopt;
}

// The order words come in: by where they were revealed (what no book says
// last), then by when they were said.
[[nodiscard]] auto ordering(const GraphFact &fact) {
    const GraphPlace &at = fact.revealed.place;
    return std::tuple{at.bookId == 0, at.bookId, at.section, at.fraction, wordOf(fact).value("said").toInteger(), fact.id};
}

[[nodiscard]] std::tuple<bool, qint64, int, double> revealedOrder(const GraphEdge &edge) {
    return {edge.learned.bookId == 0, edge.learned.bookId, edge.learned.section, edge.learned.fraction};
}

// A timeline's own relations of a branch.
[[nodiscard]] QList<GraphEdge> splitEdges(const Graph &graph, qint64 timeline) {
    QList<GraphEdge> edges = graph.around(timeline, 1, 1000).edges;
    edges.removeIf([timeline](const GraphEdge &edge) { return edge.source != timeline || !Chronology::ofBranch(edge.kind); });
    return edges;
}

// Whether `timeline` is `ancestor` or branches off it, however far up.
[[nodiscard]] bool descends(const Graph &graph, qint64 timeline, qint64 ancestor) {
    qint64 at = timeline;
    for (int depth = 0; at != 0 && depth < deepest; ++depth) {
        if (at == ancestor)
            return true;
        at = Chronology::parentOf(graph, at);
    }
    return false;
}

[[nodiscard]] std::expected<GraphNode, Failure> ofKind(const Graph &graph, qint64 id, const QString &kind) {
    const auto node = graph.node(id);
    if (!node)
        return std::unexpected(Failure(Failure::Code::NoThingNumbered, {QString::number(id)}));
    if (node->kind != kind)
        return std::unexpected(Failure(Failure::Code::NotOfKind, {node->name, node->kind, kind}));
    return *node;
}

// The branch a Branching names: its number, the timeline of its name, or
// none yet (0). Never a timeline of that name that is no branch, nor one
// that `parent` is or branches off.
[[nodiscard]] std::expected<qint64, Failure> branchOf(const Graph &graph, const Chronology::Branching &branching, const QString &name,
                                                      const GraphNode &parent) {
    qint64 id = 0;
    if (branching.timeline != 0) {
        const auto timeline = ofKind(graph, branching.timeline, Chronology::timelineKind());
        if (!timeline)
            return std::unexpected(timeline.error());
        id = timeline->id;
    } else if (const auto node = graph.node(graph.named({}, Chronology::timelineKind(), name))) {
        if (node->data.value("timeline"_L1).toString() != Chronology::name(Chronology::Whose::Branch))
            return std::unexpected(Failure(Failure::Code::TimelineNoBranch, {name}));
        id = node->id;
    }
    if (id != 0 && descends(graph, parent.id, id))
        return std::unexpected(Failure(Failure::Code::BranchOffItself, {graph.node(id).value_or(GraphNode{}).name, parent.name}));
    return id;
}

// A timeline's split, said anew: the relations it had to another parent or event go.
[[nodiscard]] bool split(Graph &graph, qint64 branch, const GraphNode &parent, const GraphNode &at,
                         const std::optional<GraphMoment> &revealed) {
    const std::optional<GraphPlace> place = revealed ? std::optional(revealed->place) : std::nullopt;
    const QString anchor = revealed ? revealed->anchor : QString();
    for (const GraphEdge &edge : splitEdges(graph, branch))
        if (edge.target != (edge.kind == Chronology::branchesFrom() ? parent.id : at.id))
            graph.unlink(edge.id);
    return graph.link(branch, parent.id, Chronology::branchesFrom(), {}, place, anchor) != 0
           && graph.link(branch, at.id, Chronology::splitsAt(), {}, place, anchor) != 0;
}

} // namespace

bool Chronology::ofBranch(const QString &relation) { return relation == branchesFrom() || relation == splitsAt(); }

QString Chronology::name(Standing standing) {
    switch (standing) {
    case Standing::Proposed:
        return u"proposed"_s;
    case Standing::Accepted:
        return u"accepted"_s;
    case Standing::Refused:
        return u"refused"_s;
    }
    return {};
}

bool Chronology::hasStanding(const QList<GraphFact> &facts) {
    return std::ranges::any_of(facts, [](const GraphFact &fact) { return fact.kind == standingFact(); });
}

Standing Chronology::standingOf(const QList<GraphFact> &facts, const GraphKnown &known) {
    const GraphFact *latest = nullptr;
    for (const GraphFact &fact : facts)
        if (fact.kind == standingFact() && known(fact.revealed.place) && standingNamed(wordOf(fact).value("standing").toString())
            && (!latest || ordering(*latest) < ordering(fact)))
            latest = &fact;
    return latest ? standingNamed(wordOf(*latest).value("standing").toString()).value_or(Standing::Accepted) : Standing::Accepted;
}

QJsonObject Chronology::Split::toJson() const {
    QJsonObject object{{"from", parent.id}, {"from_name", parent.name}, {"at", at.id}, {"at_name", at.name}, {"standing", name(standing)}};
    if (revealed.place.bookId > 0)
        object.insert("revealed", momentJson(revealed));
    return object;
}

QJsonObject Chronology::Proposal::toJson() const {
    QJsonObject object = split.toJson();
    object.insert("timeline", timeline.id);
    object.insert("name", timeline.name);
    return object;
}

QHash<qint64, Chronology::Split> Chronology::splitsOf(const QList<GraphEdge> &edges, const QHash<qint64, GraphNode> &things,
                                                      const QHash<qint64, Standing> &standing) {
    // Of each timeline, its latest relation of either kind.
    QHash<qint64, const GraphEdge *> from;
    QHash<qint64, const GraphEdge *> at;
    for (const GraphEdge &edge : edges) {
        const auto target = things.constFind(edge.target);
        if (!things.contains(edge.source) || target == things.cend())
            continue;
        QHash<qint64, const GraphEdge *> *into = nullptr;
        if (edge.kind == branchesFrom() && target->kind == timelineKind() && edge.target != edge.source)
            into = &from;
        else if (edge.kind == splitsAt() && target->kind == eventKind)
            into = &at;
        if (into && (!into->contains(edge.source) || revealedOrder(*into->value(edge.source)) <= revealedOrder(edge)))
            into->insert(edge.source, &edge);
    }
    QHash<qint64, Split> splits;
    for (auto parent = from.cbegin(); parent != from.cend(); ++parent) {
        const GraphEdge *split = at.value(parent.key());
        if (!split)
            continue;
        const GraphEdge *later = revealedOrder(**parent) < revealedOrder(*split) ? split : *parent;
        splits.insert(parent.key(), {.parent = things.value((*parent)->target),
                                     .at = things.value(split->target),
                                     .revealed = {later->learned, later->anchor},
                                     .standing = standing.value(parent.key(), Standing::Accepted)});
    }
    return splits;
}

qint64 Chronology::parentOf(const Graph &graph, qint64 timeline) {
    qint64 parent = 0;
    for (const GraphEdge &edge : splitEdges(graph, timeline))
        if (edge.kind == branchesFrom())
            parent = edge.target;
    return parent;
}

QString Chronology::branchName(const GraphNode &parent, const GraphNode &at) {
    if (parent.data.value("timeline"_L1).toString() == name(Whose::History))
        return u"Alternate history from %1"_s.arg(at.name);
    return u"%1 from %2"_s.arg(parent.name, at.name);
}

std::expected<qint64, Failure> Chronology::branch(Graph &graph, const Branching &branching) {
    const auto parent = ofKind(graph, branching.from, timelineKind());
    if (!parent)
        return std::unexpected(parent.error());
    const auto at = ofKind(graph, branching.at, eventKind);
    if (!at)
        return std::unexpected(at.error());
    const QString name = branching.name.simplified().isEmpty() ? branchName(*parent, *at) : branching.name.simplified();
    auto id = branchOf(graph, branching, name, *parent);
    if (!id)
        return id;
    // A branch there is with no word on it was made by hand: it is taken already.
    const bool propose = branching.proposed && (*id == 0 || hasStanding(graph.facts(*id)));
    if (*id == 0)
        id = graph.upsert({}, timelineKind(), name, {}, {{"timeline", Chronology::name(Whose::Branch)}},
                          branching.revealed.transform([](const GraphMoment &moment) { return moment.place; }),
                          branching.revealed.value_or(GraphMoment{}).anchor, branching.revealed);
    if (*id == 0 || !split(graph, *id, *parent, *at, branching.revealed))
        return std::unexpected(Failure(Failure::Code::BranchNotKept));
    if (propose)
        graph.learn(*id, standingFact(), u"{\"standing\":\"proposed\"}"_s, branching.revealed);
    return *id;
}

std::expected<qint64, Failure> Chronology::decide(Graph &graph, qint64 branch, bool accept) {
    const auto timeline = ofKind(graph, branch, timelineKind());
    if (!timeline)
        return std::unexpected(timeline.error());
    const QList<GraphEdge> edges = splitEdges(graph, branch);
    if (edges.isEmpty())
        return std::unexpected(Failure(Failure::Code::BranchesOffNothing, {timeline->name}));
    const GraphEdge latest = *std::ranges::max_element(edges, {}, revealedOrder);
    const QJsonObject word{{"standing", name(accept ? Standing::Accepted : Standing::Refused)},
                           {"said", QDateTime::currentMSecsSinceEpoch()}};
    const std::optional<GraphMoment> where =
        latest.learned.bookId > 0 ? std::optional(GraphMoment{latest.learned, latest.anchor}) : std::nullopt;
    const qint64 fact = graph.learn(branch, standingFact(), QString::fromUtf8(QJsonDocument(word).toJson(QJsonDocument::Compact)), where);
    if (fact == 0)
        return std::unexpected(Failure(Failure::Code::BranchWordNotKept, {timeline->name}));
    return fact;
}
