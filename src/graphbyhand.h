#pragma once

#include <QCommandLineParser>
#include <QJsonObject>
#include <QStringList>

#include "graph.h"

// What `omabook graph add` and `link` make of their options: a node or a
// relation said by hand. The answer is as GraphOps', with "error" when the
// options can't be one.
namespace GraphByHand {

[[nodiscard]] QJsonObject add(const Graph &graph, const QCommandLineParser &parser);
// Learned in a book: at the section named, or where it was read to.
[[nodiscard]] QJsonObject link(const Graph &graph, const QCommandLineParser &parser, const QStringList &words);

} // namespace GraphByHand
