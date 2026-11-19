#include "mcp.h"

// The tools of the `spaces` command (#38): each names the command, which does
// the work and prints JSON.

namespace {

using namespace Mcp;
using namespace Qt::StringLiterals;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the tool registry is filled at start-up (mcp.h), as mcpcommands.cpp fills it.
const bool registered = [] {
    registerTool({"spaces_list", "The spaces you are in",
                  "Every space the library is in: the personal one (the user's own computers), collections published "
                  "or subscribed to, and shared spaces several people write (a household library, a book club), each "
                  "with its kind, the user's role in it (writer or reader) and its members by person id.",
                  schema(), ReadOnly, [](const QJsonObject &) { return command({"spaces", "list"}); }});

    registerTool({"space_create", "Found a shared space",
                  "Founds a shared space of a collection, with the user its first writer: the collection's tree and its "
                  "books' details (never their files) are written into it, and its key is made and kept for the user "
                  "alone. Members are then invited with space_invite and it syncs through a shared folder with space_sync.",
                  schema({{"name", string("The space's name.")},
                          {"collection", string("The collection to share; the space's name when not given (made when there is none).")},
                          {"parts", list("More than the collection.", choice("A part.", {"graph", "questions", "ratings", "reading"}))}},
                         {"name"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"spaces", "create", arguments.value("name").toString()};
                      if (const QString collection = arguments.value("collection").toString().trimmed(); !collection.isEmpty())
                          asked << "--collection" << collection;
                      QStringList parts;
                      for (const auto &part : arguments.value("parts").toArray())
                          parts << part.toString();
                      if (!parts.isEmpty())
                          asked << "--parts" << parts.join(u',');
                      return command(asked);
                  }});

    registerTool({"space_invite", "Invite someone to a shared space",
                  "Makes a person a member of a shared space, as a reader or a writer, and writes their invite: a file "
                  "only they can open, which hands them the space's key. Only a writer invites. Sync the space after, so "
                  "that they are a member when they join.",
                  schema({{"space", string("The shared space's name or id.")},
                          {"person", string("The person's id, as omabook identity --export prints theirs.")},
                          {"role", choice("Their role; writer when not given.", {"reader", "writer"})},
                          {"out", string("Where to write the invite: a file, or a folder for it.")}},
                         {"space", "person"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"spaces", "invite", arguments.value("space").toString(), arguments.value("person").toString()};
                      if (arguments.contains("role"))
                          asked << "--role" << arguments.value("role").toString();
                      if (arguments.contains("out"))
                          asked << "--out" << arguments.value("out").toString();
                      return command(asked);
                  }});

    registerTool(
        {"space_remove_member", "Remove someone from a shared space",
         "Removes a member from a shared space: whatever they write after this is refused by every member, whatever its date, "
         "and the space's key changes, so they can't open what is written after. What they already had of it stays theirs.",
         schema({{"space", string("The shared space's name or id.")}, {"person", string("The member's person id.")}}, {"space", "person"}),
         Destructive, [](const QJsonObject &arguments) {
             return command({"spaces", "remove", arguments.value("space").toString(), arguments.value("person").toString()});
         }});

    registerTool({"space_join", "Join a shared space",
                  "Joins the shared space an invite file is for: opens it with the user's key, keeps the space's key, and "
                  "links a collection of the user's to the space for its books (matched to theirs, or wishlist entries).",
                  schema({{"invite", string("The .omabook-invite file.")},
                          {"as", string("The collection here for its books; the space's name when not given.")}},
                         {"invite"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"spaces", "join", arguments.value("invite").toString()};
                      if (const QString as = arguments.value("as").toString().trimmed(); !as.isEmpty())
                          asked << "--as" << as;
                      return command(asked);
                  }});

    registerTool({"space_sync", "Sync a shared space through a folder",
                  "Takes what the members wrote into a folder something else syncs (Syncthing, a mounted share): every "
                  "batch is checked, and what isn't signed by a writer of the space at its time is refused and said. Its "
                  "books go in the user's collection of it; a writer's own edits of that collection go into the space, "
                  "and what is new here into the folder, sealed with the space's key.",
                  schema({{"space", string("The shared space's name or id.")}, {"folder", string("The folder the members share.")}},
                         {"space", "folder"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(
                          {"spaces", "sync", arguments.value("space").toString(), "--folder", arguments.value("folder").toString()});
                  }});
    return true;
}();

} // namespace
