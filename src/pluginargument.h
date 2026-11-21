#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <cstdio>
#include <expected>

// The JSON `omabook plugin run <name> <what> [json]` hands the plugin (#131):
// the words after <what>, joined, or with `-` alone what standard input
// holds, which is as long as it needs to be where one argument of a command
// line holds at most 128 KiB. A bare value (a string, a number) is JSON too.
namespace PluginArgument {

// Undefined when there are no words; why not, as the parser says it, when it isn't JSON.
[[nodiscard]] std::expected<QJsonValue, QString> parse(const QStringList &words, FILE *input = stdin);

} // namespace PluginArgument
