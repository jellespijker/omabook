#include "mcp.h"

// The tools of a book's file: each names the command (bookfilecommands),
// which does the work and prints JSON.

namespace {

using namespace Mcp;
using namespace Qt::StringLiterals;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the tool registry is filled at start-up (mcp.h), as mcpcommands.cpp fills it.
const bool registered = [] {
    registerTool({"book_export", "Copy a book out",
                  "Copies one of a book's files out of the library into a folder. It never overwrites a file there.",
                  schema({{"id", integer("The book's number.", 1)},
                          {"folder", string("Where to put the copy, as an absolute path.")},
                          {"format", string("Which of the book's formats; the one the reader prefers when not given.")}},
                         {"id", "folder"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{u"export"_s, QString::number(arguments.value("id").toInteger()),
                                        arguments.value("folder").toString()};
                      if (arguments.contains("format"))
                          asked << u"--format"_s << arguments.value("format").toString();
                      return command(asked);
                  }});
    registerTool({"book_embed_details", "Write a book's details into its file",
                  "Writes the book's details as the library has them (title, authors, series, identifiers, tags, description, "
                  "cover...) into its EPUB, the library's own copy, so other apps and e-readers show them. The file the user "
                  "added is never touched, and the library's copy as it came in is kept: \"restore\" puts it back. Only for a "
                  "book with an EPUB; the user can have it done at every edit in the settings.",
                  schema({{"id", integer("The book's number.", 1)},
                          {"restore", boolean("Put the EPUB back as it came in, before details were written into it.")}},
                         {"id"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{u"embed"_s, QString::number(arguments.value("id").toInteger())};
                      if (arguments.value("restore").toBool())
                          asked << u"--restore"_s;
                      return command(asked);
                  }});
    return true;
}();

} // namespace
