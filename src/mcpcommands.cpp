#include "mcp.h"

// Tools that are one of Omabook's commands and nothing more: the command
// already does the work and prints JSON, so the tool only names it. A feature
// that has a command gets its tool here in a few lines.

namespace {

using namespace Mcp;

const QStringList sites{"goodreads", "openlibrary"};
const QStringList allSites{"goodreads", "openlibrary", "hardcover"};

const bool registered = [] {
    registerTool({"calibre_libraries", "Calibre's libraries",
                  "The Calibre libraries found on this computer, each with how many books it holds and whether it was "
                  "imported, set aside or is new. Fails when the Calibre plugin is switched off.",
                  schema(), ReadOnly, [](const QJsonObject &) { return command({"calibre", "libraries"}); }});

    registerTool({"calibre_plugins", "Calibre's plugins",
                  "Calibre's own plugins on this computer, of every kind, saying which Omabook can use (metadata "
                  "sources and conversion) and why not the others.",
                  schema({{"refresh", boolean("Ask Calibre again instead of using what was found last time.")}}), ReadOnly,
                  [](const QJsonObject &arguments) {
                      return command(arguments.value("refresh").toBool() ? QStringList{"calibre", "plugins", "--refresh"}
                                                                         : QStringList{"calibre", "plugins"});
                  }});

    registerTool({"calibre_import", "Import from Calibre",
                  "Brings a Calibre library into the library in use, with Calibre's details, covers, every format, "
                  "ratings and what was read. Importing one again brings its reading up to date without adding books "
                  "twice. The Calibre library itself is only read.",
                  schema({{"folder", string("The Calibre library's folder; every one not imported yet when not given.")},
                          {"all", boolean("Every Calibre library found, also those imported or set aside before.")}}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"calibre", "import"};
                      if (arguments.contains("folder"))
                          asked << arguments.value("folder").toString();
                      else if (arguments.value("all").toBool())
                          asked << "--all";
                      return command(asked);
                  }});

    registerTool({"reading_site_shelves", "A reading site's shelves",
                  "What a reading site says was read, is being read and is wanted, without changing the library. "
                  "Needs the user's name on that site in the settings; Goodreads only answers for a public profile.",
                  schema({{"site", choice("The site.", sites)}}, {"site"}), ReadOnly,
                  [](const QJsonObject &arguments) { return command({arguments.value("site").toString(), "shelves"}); }, true});

    registerTool({"reading_site_sync", "Sync with a reading site",
                  "Takes what a reading site says was read, is being read and is wanted into the library: books here "
                  "get their status, stars and dates, and the others are added as books without a file. Nothing is "
                  "deleted, and what was changed in Omabook since the last sync is kept. Nothing is written to the site.",
                  schema({{"site", choice("The site.", sites)},
                          {"add_new", boolean("Add the site's books the library doesn't have. True when not given.")}},
                         {"site"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{arguments.value("site").toString(), "sync"};
                      if (!arguments.value("add_new").toBool(true))
                          asked << "--no-new";
                      return command(asked);
                  }, true});

    registerTool({"reading_sites_list", "The reading sites",
                  "Goodreads, Open Library and Hardcover, each with how the user is connected to it: not connected, "
                  "connected (and as whom, with how many books), or needing attention and why; when it was last "
                  "synced; whether changes made in Omabook are also written to it. Tokens are never shown.",
                  schema(), ReadOnly, [](const QJsonObject &) { return command({"sites"}); }});

    registerTool({"reading_sites_sync", "Sync with the connected reading sites",
                  "Brings the shelves of a connected reading site (or of every connected one) into the library and, "
                  "where the user turned writing back on, sends what changed in Omabook to the site. Nothing is ever "
                  "deleted on either side; what changed on both sides stays as it is in Omabook and is reported. With "
                  "dry_run nothing changes anywhere and the result says what would. The first write to a site is held "
                  "back until send is true, so the user can look it over. Connecting a site is the user's to do, in "
                  "the window or with `omabook sites connect`: no tool takes a token.",
                  schema({{"site", choice("The site; every connected site when not given.", allSites)},
                          {"dry_run", boolean("Change nothing; say what a sync would do here and on the site.")},
                          {"send", boolean("Agree to the first write to the site, after a dry run showed it.")},
                          {"add_new", boolean("Add the site's books the library doesn't have. True when not given.")}}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"sites", "sync"};
                      if (arguments.contains("site"))
                          asked << arguments.value("site").toString();
                      if (arguments.value("dry_run").toBool())
                          asked << "--dry-run";
                      if (arguments.value("send").toBool())
                          asked << "--send";
                      if (!arguments.value("add_new").toBool(true))
                          asked << "--no-new";
                      return command(asked);
                  }, true});

    registerTool({"reading_site_disconnect", "Disconnect a reading site",
                  "Forgets who the user is on a reading site, and its token. The books and everything known of them "
                  "stay as they are.",
                  schema({{"site", choice("The site.", allSites)}}, {"site"}), Changes,
                  [](const QJsonObject &arguments) { return command({"sites", "disconnect", arguments.value("site").toString()}); }});

    registerTool({"goodreads_import", "Import a Goodreads export",
                  "Takes the file Goodreads gives under My Books > Import and export (goodreads_library_export.csv) "
                  "into the library, the same way reading_site_sync does. This is the dependable way in from Goodreads.",
                  schema({{"file", string("The export file, as an absolute path.")},
                          {"add_new", boolean("Add the books the library doesn't have. True when not given.")}},
                         {"file"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"goodreads", "import", arguments.value("file").toString()};
                      if (!arguments.value("add_new").toBool(true))
                          asked << "--no-new";
                      return command(asked);
                  }});

    registerTool({"goodreads_export", "Write a file for Goodreads",
                  "Writes the library's reading (status, stars, dates, ISBNs) as a file Goodreads' own import page "
                  "takes, which is the only way to write to Goodreads; the user uploads it there.",
                  schema({{"file", string("Where to write it, as an absolute path.")}}, {"file"}), Changes,
                  [](const QJsonObject &arguments) { return command({"goodreads", "export", arguments.value("file").toString()}); }});

    registerTool({"logs_read", "Read Omabook's log",
                  "What Omabook did and what went wrong, newest last: AI requests (provider, model, tier, size, time, "
                  "how they ended), plugin errors, library upgrades, syncs, and what the window told the person. It "
                  "holds no book text, questions or answers. Use it to find out why something failed.",
                  schema({{"lines", integer("How many of the newest entries; 100 when not given.")},
                          {"level", choice("Only this level and above.", {"debug", "info", "warning", "critical"})},
                          {"category", choice("Only one part of Omabook.", {"ai", "library", "graph", "plugins", "sites", "sync", "app", "qml"})},
                          {"since", string("Only since then: 30m, 2h, 1d, or an ISO date and time.")},
                          {"grep", string("Only entries that hold these words.")}}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      QStringList asked{"logs", "-n", QString::number(arguments.value("lines").toInt(100))};
                      for (const char *option : {"level", "category", "since", "grep"})
                          if (arguments.contains(option))
                              asked << QString("--") + option << arguments.value(option).toString();
                      return command(asked);
                  }});

    registerTool({"library_forget", "Forget a library",
                  "Takes a library off the list of the ones Omabook knows. Its folder stays, unless trash is true: "
                  "then the folder, books and all, goes to the trash, which is refused for a folder that is also a "
                  "Calibre library. The only library can't be forgotten.",
                  schema({{"folder", string("The library's folder, as library_list gives it.")},
                          {"trash", boolean("Also move the folder to the trash. False when not given.")}},
                         {"folder"}),
                  Destructive, [](const QJsonObject &arguments) {
                      return command({"libraries", arguments.value("trash").toBool() ? "--trash" : "--forget",
                                      arguments.value("folder").toString()});
                  }});

    registerTool({"themes_list", "The themes",
                  "The themes Omabook's colors can come from, the one in use marked: automatic (Omarchy's current "
                  "theme where Omarchy is, else Aether's, else Tokyo Night), Omarchy's and Aether's where they are, "
                  "Omarchy's own themes built into Omabook, and the user's own. Also the Aether blueprints that could "
                  "be imported.",
                  schema(), ReadOnly, [](const QJsonObject &) { return command({"theme", "list"}); }});

    registerTool({"theme_use", "Use a theme",
                  "Makes Omabook take its colors from a theme, at once and from then on: \"auto\", \"omarchy\", "
                  "\"aether\", a name themes_list gives (\"nord\", \"catppuccin-latte\"), or the path of a colors.toml. "
                  "Colors the user set in style.json still win. It doesn't change the desktop's theme.",
                  schema({{"theme", string("The theme, as themes_list names it.")}}, {"theme"}), Changes,
                  [](const QJsonObject &arguments) { return command({"theme", "use", arguments.value("theme").toString()}); }});
    return true;
}();

} // namespace
