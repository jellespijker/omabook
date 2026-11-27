#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

// Checking arguments against a tool's schema: the part of JSON Schema the
// schema builders in mcp.h produce. What is wrong is said in words an agent
// can act on, since it goes back to it as the tool's result.
namespace McpArguments {

// What is wrong with `value`, called `name`; empty when nothing is.
[[nodiscard]] QString check(const QJsonObject &schema, const QJsonValue &value, const QString &name);

// A schema of a number, with the most it can be.
[[nodiscard]] QJsonObject atMost(QJsonObject schema, int most);

} // namespace McpArguments
