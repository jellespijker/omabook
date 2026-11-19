#include "graphmatch.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <limits>
#include <optional>
#include <tuple>

#include "graph.h"

namespace {

struct NodeNames {
    qint64 id = 0;
    QString kind;
    QString own;
    QSet<QString> aliases;
    QSet<QString> all;
};

struct NodeMatch {
    int remote = 0;
    qint64 local = 0;
    int tier = 0;
    int common = 0;
};

NodeNames namesOf(qint64 id, const QString &kind, const QString &name, const QStringList &aliases) {
    NodeNames out{id, kind, name.simplified().toLower(), {}, {}};
    if (!out.own.isEmpty())
        out.all.insert(out.own);
    for (const QString &alias : aliases) {
        const QString clean = alias.simplified().toLower();
        if (clean.isEmpty() || clean == out.own)
            continue;
        out.aliases.insert(clean);
        out.all.insert(clean);
    }
    return out;
}

NodeNames remoteNames(const QJsonObject &node) {
    QStringList aliases;
    for (const auto &alias : node.value("aliases").toArray())
        aliases << alias.toString();
    return namesOf(node.value("id").toInteger(), node.value("kind").toString(), node.value("name").toString(), aliases);
}

int matchTier(const NodeNames &mine, const NodeNames &theirs) {
    if (mine.kind != theirs.kind)
        return 0;
    if (!mine.own.isEmpty() && mine.own == theirs.own)
        return 1;
    if (!mine.own.isEmpty() && theirs.aliases.contains(mine.own))
        return 2;
    if (!theirs.own.isEmpty() && mine.aliases.contains(theirs.own))
        return 3;
    if (std::ranges::any_of(mine.aliases, [&](const QString &alias) { return theirs.aliases.contains(alias); }))
        return 4;
    return 0;
}

int commonNames(const NodeNames &mine, const NodeNames &theirs) {
    return static_cast<int>(std::ranges::count_if(mine.all, [&](const QString &name) { return theirs.all.contains(name); }));
}

} // namespace

// Match each incoming thing to at most one local thing, preferring an own-name
// match over an ambiguous other-name match, and never merging two local things.
QList<qint64> GraphMatch::matchNodes(const Graph &graph, const QJsonArray &nodes) {
    QHash<QString, QList<NodeNames>> byKind;
    for (const GraphNode &local : graph.whole({}, std::numeric_limits<int>::max()).nodes)
        byKind[local.kind] << namesOf(local.id, local.kind, local.name, local.aliases());
    QList<NodeMatch> candidates;
    for (int i = 0; i < nodes.size(); ++i) {
        const NodeNames theirs = remoteNames(nodes.at(i).toObject());
        const auto localIt = byKind.constFind(theirs.kind);
        if (localIt == byKind.cend())
            continue;
        for (const NodeNames &mine : *localIt)
            if (const int tier = matchTier(mine, theirs); tier > 0)
                candidates << NodeMatch{i, mine.id, tier, commonNames(mine, theirs)};
    }
    std::ranges::sort(candidates, [](const NodeMatch &a, const NodeMatch &b) {
        return std::tuple{a.tier, -a.common, a.local, a.remote} < std::tuple{b.tier, -b.common, b.local, b.remote};
    });
    QList<qint64> matched(nodes.size(), 0);
    QSet<qint64> taken;
    for (const NodeMatch &candidate : candidates) {
        if (matched.at(candidate.remote) != 0 || taken.contains(candidate.local))
            continue;
        matched[candidate.remote] = candidate.local;
        taken.insert(candidate.local);
    }
    return matched;
}
