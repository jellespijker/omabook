#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "graph.h"
#include "library.h"

// The `omabook graph` commands that look after the graph rather than read or
// grow it: its scopes, the reading log, another computer's graph taken in,
// and the books' own nodes. The others are in graphtools.cpp.
namespace GraphCommands {

struct Kept {
    QJsonObject answer;
    std::function<void(const QJsonObject &)> show; // how the answer reads without --json
};

// Runs `omabook graph <words>` when its first word is one of these; nothing
// when it isn't. The scopes, a copy of one and the reading log are of the
// graph's tenant.
// `scopes` of `everyTenant` lists those of every tenant (GraphOps::everyScope).
std::optional<Kept> housekeeping(Library &library, const Graph &graph, const QString &sub, const QStringList &words,
                                 bool everyTenant = false);

} // namespace GraphCommands
