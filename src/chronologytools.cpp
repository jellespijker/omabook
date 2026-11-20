#include "mcp.h"

// The tools of the `chronology` command (#45): each names the command, which
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
    const QJsonObject scope = string("A graph of its own, such as world:42; the general graph when not given.");
    const QJsonObject event = string("The event: its number in the graph, or its name.");
    registerTool(
        {"chronology", "The chronology of the books read",
         "The events of the books read in the order they happen in their world, not the order they are read in, never "
         "past where the reader is in each book: of every timeline (a book's own, a series', a world's, real history) its "
         "events in order (\"lanes\", each item with its \"rank\", events at the same time sharing one, its time as the "
         "book says it and where that was revealed), which timelines line up on one axis (\"axes\"), the events of which "
         "neither a time nor an order is known (\"unplaced\"), and every contradiction with all its statements and where "
         "each was revealed, and the one set aside by the rule. A part for each side of a library kept apart by its top "
         "collections. \"events\" says how many are shown of how many known; \"by_hand\" marks what the user put in.",
         schema({{"book", integer("Only this book's events, by its number.", 1)},
                 {"series", string("Only the events of this series' books.")},
                 {"collection", string("Only the events of this collection's books.")},
                 {"scope", scope},
                 {"timeline", string("Only this timeline, by its number or name.")},
                 {"from", string("Only what happens from this time on: Y, Y-MM, Y-MM-DD, Y-MM-DDTHH or Y-MM-DDTHH:MM.")},
                 {"to", string("Only what happens up to this time.")},
                 {"calendar", string("The calendar of from and to; each timeline's own when not given.")},
                 {"up_to_section", integer("With book: as known up to and including this section of it, counted from 0.", 0)},
                 {"up_to_fraction", Mcp::number("How far into that section, from 0 to 1; all of it when not given.", 0, 1)},
                 {"whole", boolean("Everything that is known, read or not. It gives away what happens: only when the user asks.")}}),
         ReadOnly, [](const QJsonObject &arguments) {
             QStringList asked{"chronology"};
             if (arguments.contains("book"))
                 asked << QString::number(arguments.value("book").toInteger());
             asked << options(arguments, {{"series", "--series"},
                                          {"collection", "--collection"},
                                          {"scope", "--scope"},
                                          {"timeline", "--timeline"},
                                          {"from", "--from"},
                                          {"to", "--to"},
                                          {"calendar", "--calendar"},
                                          {"up_to_section", "--upto"},
                                          {"up_to_fraction", "--fraction"}});
             if (arguments.value("whole").toBool())
                 asked << "--whole";
             return command(asked);
         }});

    registerTool({"chronology_set_when", "Say when an event happens",
                  "Puts in by hand when an event of the graph happens in its story's world: a moment (1944-06-06), a span "
                  "(1939-09-01..1945-09-02, either end may be left out), or only the book's words for it (said). What is put in by "
                  "hand wins over the times the books give. A time that isn't one is refused, with why.",
                  schema({{"event", event},
                          {"time", string("Y, Y-MM, Y-MM-DD, Y-MM-DDTHH or Y-MM-DDTHH:MM, or a span from..to; or a whole time as JSON.")},
                          {"calendar", string("The story's own calendar, such as Third Age; the Gregorian when not given.")},
                          {"approximate", boolean("The book says around, or some time in.")},
                          {"said", string("The book's own few words for the time.")},
                          {"scope", scope}},
                         {"event"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"chronology", "set-when", arguments.value("event").toString()};
                      if (const QString time = arguments.value("time").toString(); !time.isEmpty())
                          asked << time;
                      asked << options(arguments, {{"calendar", "--calendar"}, {"said", "--said"}, {"scope", "--scope"}});
                      if (arguments.value("approximate").toBool())
                          asked << "--approximate";
                      return command(asked);
                  }});

    registerTool({"chronology_order", "Put two events in order",
                  "Puts in by hand that one event of the graph happens before, after, during or at the same time as "
                  "another, in their story's world. An event is in no order with itself.",
                  schema({{"event", event},
                          {"relation", choice("How the event is to the other.", {"before", "after", "during", "same time as"})},
                          {"other", string("The other event: its number in the graph, or its name.")},
                          {"scope", scope}},
                         {"event", "relation", "other"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(QStringList{"chronology", "order", arguments.value("event").toString(),
                                                 arguments.value("relation").toString(), arguments.value("other").toString()}
                                     + options(arguments, {{"scope", "--scope"}}));
                  }});

    registerTool({"chronology_place", "Put an event on a timeline",
                  "Puts in by hand that an event of the graph is on a timeline: a book's, a series', a world's, real history, "
                  "or one the user named. Two books share a timeline when their events are on it.",
                  schema({{"event", event}, {"timeline", string("The timeline: its number in the graph, or its name.")}, {"scope", scope}},
                         {"event", "timeline"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(QStringList{"chronology", "place", arguments.value("event").toString(), "on",
                                                 arguments.value("timeline").toString()}
                                     + options(arguments, {{"scope", "--scope"}}));
                  }});

    registerTool({"chronology_timeline_add", "Make a timeline",
                  "Makes a timeline to put events on, or finds the one of that name: a book's, a series', a world's, the "
                  "reader's own, or real history, which is one for the whole library whatever it is called.",
                  schema({{"name", string("What it is called.")},
                          {"kind", choice("Whose it is; the reader's when not given.", {"book", "series", "world", "history", "reader"})}},
                         {"name"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(QStringList{"chronology", "timeline", "add", arguments.value("name").toString()}
                                     + options(arguments, {{"kind", "--kind"}}));
                  }});
    return true;
}();

} // namespace
