#include "mcp.h"

// Tools that are one of Omabook's commands and nothing more: the command
// already does the work and prints JSON, so the tool only names it. A feature
// that has a command gets its tool here in a few lines.

namespace {

using namespace Mcp;
using namespace Qt::StringLiterals;

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

    // How books are set on the page: `omabook format`.
    registerTool({"reading_format_get", "How books are set on the page",
                  "The reading format: font, type size, longest line (the measure, in characters), line spacing "
                  "(percent), space between paragraphs, first-line indent, margins (pixels), alignment, the layout "
                  "(pages, scroll or auto) and the first page of a two-up PDF (alone, paired or auto), and how many "
                  "pages stand side by side (pages: auto, 1, 2 or 3, for every book). Without a book, the defaults every book follows; "
                  "with one, what is in force for it, the defaults, and the options it has of its own (\"own\"). Also "
                  "the bounds of each. Pages that are pictures (PDF, comics) are not set, but for the layout and the first page.",
                  schema({{"book", integer("A book's id, for its own; the defaults when not given.")}}), ReadOnly,
                  [](const QJsonObject &arguments) {
                      QStringList asked{"format"};
                      if (arguments.contains("book"))
                          asked << QString::number(arguments.value("book").toInteger());
                      return command(asked);
                  }});

    registerTool({"reading_format_set", "Change how books are set on the page",
                  "Changes the reading format, for one book alone or as the defaults every book without its own "
                  "follows; the window follows at once. Only the options given change. A value out of bounds is "
                  "refused with the allowed range, and then nothing changes. reset puts the book back to the defaults "
                  "(or, without a book, the defaults back to Omabook's own) and goes alone.",
                  schema({{"book", integer("A book's id, to change its own; the defaults when not given.")},
                          {"font_size", integer("The type size in pixels, 10 to 48.")},
                          {"font_family", string("A font's name; \"\" for the desktop's.")},
                          {"measure", integer("The longest line, in characters of the font, 40 to 90; 66 reads best.")},
                          {"line_height", integer("Line spacing in percent, 100 to 300.")},
                          {"paragraph_spacing", integer("The space after a paragraph in pixels, 0 to 80.")},
                          {"indent", integer("The indent of a paragraph's first line in pixels, 0 to 64.")},
                          {"margins", integer("The room around the text in pixels, 0 to 160.")},
                          {"align", choice("Where a paragraph's lines go: start, justify (filled out to both sides), "
                                           "center or end. Start and end turn round in a right-to-left language.",
                                           {"start", "justify", "center", "end"})},
                          {"layout", choice("How the book is read through: pages (cut into pages that turn, each chapter "
                                            "starting on a page of its own; a PDF's page shown whole), scroll (one column "
                                            "that runs on into the next chapter) or auto (pages where two or more fit side "
                                            "by side, else a column that scrolls).",
                                            {"auto", "pages", "scroll"})},
                          {"first_page", choice("Pages that are pictures, two side by side: alone (the first on its own, "
                                                "then 2|3, as a printed book opens), paired (1|2, 3|4) or auto (alone when "
                                                "the first page is taller than wide, paired for slides).",
                                                {"auto", "alone", "paired"})},
                          {"pages", choice("How many pages stand side by side, for every book (not with book): auto (as "
                                           "many as the window has room for, each with lines of at least 45 characters, "
                                           "up to three), or 1, 2 or 3, shown where each page keeps 45 characters.",
                                           {"auto", "1", "2", "3"})},
                          {"reset", boolean("Back to the defaults (for a book), or to Omabook's own (without one).")}}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"format"};
                      if (arguments.contains("book"))
                          asked << QString::number(arguments.value("book").toInteger());
                      static const QList<std::pair<const char *, const char *>> options{
                          {"font_size", "--font-size"}, {"font_family", "--font"}, {"measure", "--measure"},
                          {"line_height", "--line-height"}, {"paragraph_spacing", "--paragraph-spacing"},
                          {"indent", "--indent"}, {"margins", "--margins"}, {"align", "--align"}, {"layout", "--layout"},
                          {"first_page", "--first-page"}, {"pages", "--pages"}};
                      for (const auto &[key, flag] : options)
                          if (arguments.contains(key)) {
                              const QJsonValue value = arguments.value(key);
                              asked << flag << (value.isDouble() ? QString::number(value.toDouble()) : value.toString());
                          }
                      if (arguments.value("reset").toBool())
                          asked << "--reset";
                      if (asked.size() == (arguments.contains("book") ? 2 : 1))
                          return Result::fail("Give at least one option to change, or reset.");
                      return command(asked);
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
    // --- Collections -----------------------------------------------------------
    registerTool({"collections_list", "The collections",
                  "The library's collections as a tree: named parts of the library, named as the user likes, each filled "
                  "by its rule (a search, as books_search takes it) and by books put in or taken out by hand, with how "
                  "many books each holds now. A book can be in several; nothing is copied or moved.",
                  schema(), ReadOnly, [](const QJsonObject &) { return command({"collections", "list"}); }});

    registerTool({"collection_get", "A collection and its books",
                  "One collection: its rule, the books put in and taken out by hand, its own view and reading format, "
                  "and every book in it.",
                  schema({{"name", string("The collection's name, or its number.")}}, {"name"}), ReadOnly,
                  [](const QJsonObject &arguments) { return command({"collections", "show", arguments.value("name").toString()}); }});

    registerTool({"collection_add", "Make a collection",
                  "Makes a collection, named as the user likes. A name with \"/\" is a path: Studie/Python is Python inside "
                  "Studie, and the collections a path goes through are made too; a collection holds what those inside it hold. Its rule is a search: bare words, author:, title:, series:, tag:, genre: "
                  "(fiction, nonfiction, or a tag), format:, lang:, publisher:, about: (the description), status:, is: "
                  "and rating:; \"quotes\" keep words together, -word leaves out, OR joins alternatives. A new book "
                  "lands in every collection whose rule finds it. Without a rule it holds only books put in by hand.",
                  schema({{"name", string("What it is called, or its path inside others.")},
                          {"rule", string("The search that fills it, such as tag:history OR series:Dune.")},
                          {"parent", string("The collection to make it inside, by its path.")}},
                         {"name"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"collections", "add", arguments.value("name").toString()};
                      if (!arguments.value("parent").toString().trimmed().isEmpty())
                          asked << "--parent" << arguments.value("parent").toString();
                      if (!arguments.value("rule").toString().trimmed().isEmpty())
                          asked << "--rule" << arguments.value("rule").toString();
                      return command(asked);
                  }});

    registerTool({"collection_set_rule", "Change a collection's rule",
                  "Changes the search that fills a collection; an empty rule leaves only the books put in by hand. "
                  "Books put in or taken out by hand stay so.",
                  schema({{"name", string("The collection's name, or its number.")}, {"rule", string("The new search.")}},
                         {"name", "rule"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command({"collections", "rule", arguments.value("name").toString(), arguments.value("rule").toString()});
                  }});

    registerTool({"collection_rename", "Rename or move a collection",
                  "Gives a collection another name, or with a path (\"Werk/Hydraulica\") moves it there too; the "
                  "collections inside it go along.",
                  schema({{"name", string("The collection's path, name, or number.")}, {"new_name", string("Its new name or path.")}},
                         {"name", "new_name"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command({"collections", "rename", arguments.value("name").toString(), arguments.value("new_name").toString()});
                  }});

    registerTool({"collection_put", "Put books in a collection or take them out",
                  "Puts books in a collection by hand (\"include\"), takes them out (\"exclude\"), whatever its "
                  "rule says, or lets the rule decide for them again (\"rule\"). The books stay in the library either way.",
                  schema({{"name", string("The collection's name, or its number.")},
                          {"books", list("The books, by their ids from books_search.", integer("A book's id.", 1))},
                          {"how", choice("Put in, take out, or follow the rule again.", {"include", "exclude", "rule"})}},
                         {"name", "books", "how"}),
                  Changes, [](const QJsonObject &arguments) {
                      const QString how = arguments.value("how").toString();
                      QStringList asked{"collections", how == "rule" ? u"follow"_s : how, arguments.value("name").toString()};
                      for (const QJsonValue &book : arguments.value("books").toArray())
                          asked << QString::number(book.toInteger());
                      return command(asked);
                  }});

    registerTool({"collection_remove", "Remove a collection",
                  "Takes a collection away. Its books stay in the library and in any other collection; only the "
                  "collection, its rule and its choices made by hand go.",
                  schema({{"name", string("The collection's name, or its number.")}}, {"name"}), Destructive,
                  [](const QJsonObject &arguments) { return command({"collections", "remove", arguments.value("name").toString()}); }});

    registerTool({"collection_format", "A collection's reading format",
                  "Shows or sets how a collection's books are set on the page, such as justified text for novels or "
                  "longer lines for technical books. It lies over the library's defaults, and a book's own options "
                  "lie over it. Options are those of reading_format_get: font_size, font_family, measure, "
                  "line_height, paragraph_spacing, indent, margins and align.",
                  schema({{"name", string("The collection's name, or its number.")},
                          {"options", object("Options to set, such as {\"align\": \"justify\", \"measure\": 72}.")},
                          {"reset", boolean("Let the collection follow the defaults again.")}},
                         {"name"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"collections", "format", arguments.value("name").toString()};
                      if (arguments.value("reset").toBool())
                          asked << "--reset";
                      const QJsonObject options = arguments.value("options").toObject();
                      for (auto it = options.begin(); it != options.end(); ++it)
                          asked << "--set" << it.key() + "=" + (it->isDouble() ? QString::number(it->toInteger()) : it->toString());
                      return command(asked);
                  }});

    registerTool({"collection_move", "Put a collection inside another",
                  "Puts a collection inside another one, or at the top with an empty parent. What is inside it goes along.",
                  schema({{"name", string("The collection's path, name, or number.")},
                          {"parent", string("The collection to put it in, by its path; empty for the top.")}},
                         {"name", "parent"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command({"collections", "move", arguments.value("name").toString(), arguments.value("parent").toString()});
                  }});

    registerTool({"collection_share", "Share a collection as a space",
                  "Publishes a collection as a space others can follow (it follows the collection as it changes) and, "
                  "with output, writes its snapshot, an .omabook-space file to give them. Answers first what goes in, "
                  "counted by kind (collections, books, things, facts and relations of the graph, questions, ratings, "
                  "reading places and days), and what is left out: the books' files always, and each of the user's own "
                  "parts that isn't switched on. Of the graph only what the user has read goes, nothing past their "
                  "place in a book. Show that to the user with dry_run before sharing, and ask them which parts to add "
                  "and the name they go by in it.",
                  schema({{"name", string("The collection's path, name, or number.")},
                          {"graph", boolean("With what the knowledge graph knows of its books.")},
                          {"with", list("The user's own parts to add.", choice("A part.", {"questions", "ratings", "progress"}))},
                          {"as", string("The name the user goes by in the space; needed the first time.")},
                          {"output", string("The file to write the snapshot to, ending in .omabook-space.")},
                          {"dry_run", boolean("Only say what would go in and what would stay out; publish and write nothing.")}},
                         {"name"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"collections", "share", arguments.value("name").toString()};
                      if (arguments.value("graph").toBool())
                          asked << "--with-graph";
                      QStringList parts;
                      for (const QJsonValue part : arguments.value("with").toArray())
                          parts << part.toString();
                      if (!parts.isEmpty())
                          asked << "--with" << parts.join(',');
                      for (const char *option : {"as", "output"})
                          if (const QString value = arguments.value(QLatin1StringView(option)).toString(); !value.isEmpty())
                              asked << "--" + QString::fromLatin1(option) << value;
                      if (arguments.value("dry_run").toBool())
                          asked << "--dry-run";
                      return command(asked);
                  }});

    registerTool({"books_find", "Find books by what is known of them, or by a question",
                  "Searches as the window's search box does: the words of books_search, where a bare word also finds a "
                  "book by the characters, places, ideas, events, plot threads, themes, topics and questions the "
                  "knowledge graph met in it, and knows:<word> looks only there. Only what the user has read counts: "
                  "nothing from past their furthest page, and nothing of a book they haven't begun. A book found that "
                  "way says what found it under \"known\" (the node, its kind, and where it was met). With ai, the "
                  "words are a question in the user's own words (\"the books where a mentor dies\", \"something "
                  "about control theory I haven't finished\"): Omabook's quick model reads the library a batch at a "
                  "time, from the books' details and what is known of them, never their text, and answers with the "
                  "books that fit, the best first, a reason each. An answer is kept, so the same question of an "
                  "unchanged library isn't asked again.",
                  schema({{"words", string("The search, or with ai the question.")},
                          {"ai", boolean("Ask the AI rather than search.")},
                          {"collection", string("Only the books of this collection, by its path.")}},
                         {"words"}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      // A leading space keeps "-word" from reading as an option.
                      QStringList asked{"search", " " + arguments.value("words").toString()};
                      if (arguments.value("ai").toBool())
                          asked << "--ai";
                      if (!arguments.value("collection").toString().trimmed().isEmpty())
                          asked << "--collection" << arguments.value("collection").toString();
                      return command(asked);
                  }, true});

    registerTool({"collections_sort", "Propose where books belong",
                  "Has Omabook's quick model propose, for each book not yet in a collection with none inside it, "
                  "a collection there is or a new one anywhere in the tree (named in the style and language of the "
                  "user's own), from the books' titles, authors, tags and the start of their descriptions, a batch "
                  "at a time. Books it can't place with confidence come back in \"to_decide\", for the user. Nothing "
                  "changes: show the numbered proposals to the user, then apply the ones they pick with "
                  "collections_sort_apply, which takes them from this list without asking the model again. With "
                  "\"within\", it sorts the books of that collection that are in none inside it yet into collections "
                  "inside it, there or new; they stay in it too.",
                  schema({{"within", string("A collection's path, to propose collections inside it for its books.")}}), ReadOnly,
                  [](const QJsonObject &arguments) {
                      QStringList asked{"collections", "sort"};
                      if (const QString within = arguments.value("within").toString().trimmed(); !within.isEmpty())
                          asked << within;
                      return command(asked);
                  }, true});

    registerTool({"collections_sort_apply", "Apply picked collection proposals",
                  "Applies proposals from the list collections_sort last gave, by their numbers; a proposal can be "
                  "given another path (a new collection is made for it). The model is not asked again.",
                  schema({{"pick", list("The proposals, as their numbers, or \"3=Some/Path\" for another path.", string("A number."))},
                          {"all", boolean("Apply every proposal on the list.")}}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList picked;
                      for (const QJsonValue &value : arguments.value("pick").toArray())
                          picked << (value.isDouble() ? QString::number(value.toInteger()) : value.toString());
                      if (picked.isEmpty() && !arguments.value("all").toBool())
                          return Result::fail("Pick proposals by their numbers, or give all.");
                      return command(picked.isEmpty() ? QStringList{"collections", "sort", "--apply"}
                                                      : QStringList{"collections", "sort", "--pick", picked.join(',')});
                  }});

    registerTool({"collection_import", "Subscribe to a shared collection",
                  "Imports a collection someone shared, from its .omabook-space file, by subscribing to its space: the "
                  "space is kept read-only beside the library, never merged into it. Its books the user has are matched "
                  "(the same file, an identifier or ISBN, or title and authors) and the others become wishlist entries "
                  "(never downloads), in a collection of the user's own: the space's name, or `as`; one of that name is "
                  "merged into, not overwritten. Its knowledge graph stays in the space, shown beside the user's as far as "
                  "they have read each book. A newer "
                  "file of the same space brings only what is new. Try dry_run first to show the user what it would do.",
                  schema({{"file", string("The .omabook-space file.")},
                          {"as", string("The collection's name here, instead of the shared one's.")},
                          {"dry_run", boolean("Change nothing; say what importing would do.")}},
                         {"file"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"collections", "import", arguments.value("file").toString()};
                      if (const QString as = arguments.value("as").toString().trimmed(); !as.isEmpty())
                          asked << "--as" << as;
                      if (arguments.value("dry_run").toBool())
                          asked << "--dry-run";
                      return command(asked);
                  }});

    registerTool({"collection_unsubscribe", "Unsubscribe from a shared collection",
                  "Drops a shared collection's space from the library. The user's own collection of it stays, with its "
                  "books and wishlist entries.",
                  schema({{"name", string("The space's name or id, or the collection it was imported into.")}}, {"name"}), Destructive,
                  [](const QJsonObject &arguments) {
                      return command({"collections", "unsubscribe", arguments.value("name").toString()});
                  }});
    return true;
}();

} // namespace
