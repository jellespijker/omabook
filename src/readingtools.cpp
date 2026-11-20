#include "mcp.h"

// The tools of the `stats` command (#68): each names the command, which
// does the work and prints JSON.

namespace {

using namespace Mcp;
using namespace Qt::StringLiterals;

// "--name value" for each of these arguments that is given.
QStringList options(const QJsonObject &arguments, std::initializer_list<std::pair<const char *, const char *>> named) {
    QStringList asked;
    for (const auto &[key, option] : named)
        if (const QJsonValue value = arguments.value(QLatin1StringView(key)); !value.isUndefined() && !value.isNull())
            asked << QLatin1StringView(option) << (value.isString() ? value.toString() : value.toVariant().toString());
    return asked;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the tool registry is filled at start-up (mcp.h), as mcpcommands.cpp fills it.
const bool registered = [] {
    const QJsonObject year = integer("The year; this one when not given.", 1);
    registerTool({"reading_stats", "Reading statistics",
                  "What the user read, added up, worked out from the library alone: of a year, the time read (\"seconds\", counted while "
                  "a book is open in the reader, the time away left out; nothing for reading before Omabook 0.1.10 counted it), the days "
                  "read on, how much was read in whole books (\"books\": a day from 10% to 40% of one is 0.3) and the books finished, in "
                  "\"total\", per month and per day read on; every year's beside it; the days read in a row (\"streaks\": the current "
                  "run and the longest); the books read or finished in the year, each with when it was begun, last read and finished "
                  "and how many days that took; how many books were read without a known finishing day (\"undated\"); and the year's "
                  "goal with how much of it is done. With \"book\", that one book's reading of all time instead.",
                  schema({{"year", year},
                          {"collection", string("Only the books of this collection, by its name or path.")},
                          {"book", integer("Only this book's reading, by its number.", 1)}}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      return command(QStringList{u"stats"_s}
                                     + options(arguments, {{"year", "--year"}, {"collection", "--collection"}, {"book", "--book"}}));
                  }});
    registerTool({"reading_goal_set", "Set the year's reading goal",
                  "Sets the goal of a year's reading: a number of books finished, or of hours read; a target of 0 takes it away. "
                  "It is the user's own and goes to their other computers at the next sync; it is only shown, never a reminder.",
                  schema({{"target", integer("How many books or hours; 0 for no goal.")},
                          {"kind", choice("What the goal counts.", {u"books"_s, u"hours"_s})},
                          {"year", year}},
                         {"target"}),
                  Changes, [](const QJsonObject &arguments) {
                      const int target = arguments.value("target").toInt();
                      const QStringList goal = target > 0
                                                   ? QStringList{QString::number(target), arguments.value("kind").toString(u"books"_s)}
                                                   : QStringList{u"none"_s};
                      return command(QStringList{u"stats"_s, u"goal"_s} + goal + options(arguments, {{"year", "--year"}}));
                  }});
    return true;
}();

} // namespace
