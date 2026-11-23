#pragma once

#include <QJsonObject>
#include <QString>
#include <QtTypes>
#include <functional>
#include <limits>

#include "mcp.h"

// An agent's change to many books at once over MCP (#208): a tool that can
// make one has its handler wrapped, so that a call changing more than
// MomentSnapshot::bulkBooks books takes a moment snapshot of the library
// first, and its answer says that snapshot as the undo ("undo": {"snapshot",
// "how"}). One that can't be taken is said in the answer's "notes", and the
// change goes on, as on any computer; a hub refuses it (MomentSnapshot::beforeBulk).
namespace McpBulk {

// How many books a call changes, from its arguments.
using Count = std::function<qsizetype(const QJsonObject &arguments)>;
// What a call changes whose books aren't known before it runs (every
// proposal on a list): taken as many.
inline constexpr qsizetype many = std::numeric_limits<qsizetype>::max();

// `handler`, after the moment snapshot when `count` says the call is a bulk change.
[[nodiscard]] Mcp::Handler snapshotFirst(Count count, Mcp::Handler handler);
// The books a list argument names, one each.
[[nodiscard]] Count listed(const QString &key);

} // namespace McpBulk
