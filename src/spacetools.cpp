#include "mcp.h"
#include "spaceparts.h"

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
                  "books' details (their files only once space_files lets them travel) are written into it, and its key is made and kept "
                  "for the user "
                  "alone. Members are then invited with space_invite and it syncs through a shared folder with space_sync.",
                  schema({{"name", string("The space's name.")},
                          {"collection", string("The collection to share; the space's name when not given (made when there is none).")},
                          {"parts", list("More than the collection.", choice("A part.", SpaceParts::names()))}},
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

    registerTool({"space_name", "Choose the name to go by in a shared space",
                  "Sets the name the user goes by in a shared space they write, which every member sees beside their "
                  "annotations once they sync: one line of at most 64 characters. Ask the user which name they want.",
                  schema({{"space", string("The shared space's name or id.")}, {"name", string("The name to go by.")}}, {"space", "name"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command({"spaces", "name", arguments.value("space").toString(), arguments.value("name").toString()});
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
    registerTool({"space_files", "Let a space's book files travel, or stop them",
                  "Switches whether the books' files of a space travel to everyone it reaches: a shared space, as one of its "
                  "writers (every member gets them, and whoever is made one later), or a space the user published (whoever has "
                  "its file or link, and whoever they pass it to). Off unless switched on. Before turning it on, tell the user "
                  "plainly who will get the files and ask. Each member fetches a file only when opening the book (space_fetch_file).",
                  schema({{"space", string("The space's name or id.")}, {"on", boolean("Whether the files travel.")}}, {"space", "on"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(
                          {"spaces", "files", arguments.value("space").toString(), arguments.value("on").toBool() ? "on" : "off"});
                  }});

    registerTool({"space_annotations", "Carry annotations into a space, or stop them",
                  "Switches whether the highlights, margin notes and book ink in a space's books go into it: a shared space, as one "
                  "of its writers (every member's then go, each member seeing the others' as far as they have read), or a space "
                  "the user published (theirs, to whoever has it). Off unless switched on. Before turning it on, tell the user "
                  "plainly who will see their annotations and ask.",
                  schema({{"space", string("The space's name or id.")}, {"on", boolean("Whether the annotations go.")}}, {"space", "on"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(
                          {"spaces", "annotations", arguments.value("space").toString(), arguments.value("on").toBool() ? "on" : "off"});
                  }});

    registerTool({"space_fetch_file", "Fetch a book's file from a space",
                  "Fetches the file of a book that has none here from a space that lets its files travel (its shared folder, "
                  "or beside the space's file it was subscribed from), checks it against the hash the space names, and gives "
                  "it to the book. A book with a file of its own keeps it.",
                  schema({{"book", integer("The book's number.")}}, {"book"}), Changes, [](const QJsonObject &arguments) {
                      return command({"spaces", "fetch", QString::number(arguments.value("book").toInteger())});
                  }});
    return true;
}();

} // namespace
