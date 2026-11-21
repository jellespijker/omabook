#include "mcp.h"

// The tools of the `meaning` command (#72): each names the command, which
// does the work and prints JSON.

namespace {

using namespace Mcp;
using namespace Qt::StringLiterals;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the tool registry is filled at start-up (mcp.h), as mcpcommands.cpp fills it.
const bool registered = [] {
    registerTool({"meaning_search", "Search a book by meaning",
                  "Finds the stretches of a book's text that best answer a question by what it means, not only its words (\"where do "
                  "they cross the river?\"), together with the words themselves; or of every indexed book of a top-level collection "
                  "with book \"all\". Each comes with its book, its place (section, fraction, and \"to\" where it ends), its text and "
                  "\"by\": meaning, words or both. Nothing past where the user has read to in a book is given unless \"whole\" is true: "
                  "don't ask for it without the user's say. A book is searched only once it is indexed (meaning_index).",
                  schema({{"book", string("The book's number, or \"all\" for the books of a collection.")},
                          {"question", string("What to find, in plain words.")},
                          {"collection", string("With book \"all\": the top-level collection to look in, by its name.")},
                          {"whole", boolean("Past the user's place too; only when they asked for it.")},
                          {"limit", integer("How many at most; 10 when not given.", 1)}},
                         {"book", "question"}),
                  ReadOnly,
                  [](const QJsonObject &arguments) {
                      QStringList asked{u"meaning"_s, u"search"_s, arguments.value("book").toVariant().toString()};
                      asked << arguments.value("question").toString().split(u' ', Qt::SkipEmptyParts);
                      if (const QString collection = arguments.value("collection").toString(); !collection.isEmpty())
                          asked << u"--collection"_s << collection;
                      if (arguments.value("whole").toBool())
                          asked << u"--whole"_s;
                      if (arguments.contains("limit"))
                          asked << u"--limit"_s << QString::number(arguments.value("limit").toInt());
                      return command(asked);
                  },
                  true});
    registerTool({"meaning_index", "Index a book by meaning",
                  "Indexes a book's text by what it means, so that meaning_search finds in it: its text is cut in stretches and each "
                  "is embedded by the user's own embedding server (never a hosted one unless the user allowed it in the settings), "
                  "then kept in the library, and made again when the book's file changes. It can take a minute for a long book.",
                  schema({{"book", integer("The book's number.", 1)}}, {"book"}), Changes,
                  [](const QJsonObject &arguments) {
                      return command({u"meaning"_s, u"index"_s, QString::number(arguments.value("book").toInteger())});
                  },
                  true});
    registerTool({"meaning_status", "The meaning index",
                  "Which server and model make the embeddings (or why none may), whether every book is indexed or only those asked "
                  "for, and each indexed book: indexed, out of date (its file or the model changed) or not yet, with its stretches.",
                  schema(), ReadOnly, [](const QJsonObject &) { return command({u"meaning"_s}); }});
    return true;
}();

} // namespace
