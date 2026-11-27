#include "graphprint.h"

#include <QHash>
#include <QJsonArray>
#include <print>

void GraphPrint::print(FILE *stream, const QString &text) {
    std::println(stream, "{}", text.toStdString());
    std::fflush(stream);
}

QString GraphPrint::nodeLine(const QJsonObject &node) {
    return QString("%1\t%2\t%3%4")
        .arg(node.value("id").toInteger())
        .arg(node.value("kind").toString(), node.value("name").toString(),
             node.value("summary").toString().isEmpty() ? QString() : "\t" + node.value("summary").toString());
}

// What a book connects to elsewhere, a line a tie: both ends by name, and
// the book the far end is in.
void GraphPrint::printBeyond(const QJsonObject &answer) {
    QHash<qint64, QString> names;
    QHash<qint64, QString> books;
    for (const auto &value : answer.value("nodes").toArray())
        names.insert(value.toObject().value("id").toInteger(), value.toObject().value("name").toString());
    for (const auto &value : answer.value("beyond").toObject().value("nodes").toArray()) {
        const QJsonObject node = value.toObject();
        names.insert(node.value("id").toInteger(), node.value("name").toString());
        books.insert(node.value("id").toInteger(), node.value("in").toArray().first().toObject().value("title").toString());
    }
    for (const auto &value : answer.value("beyond").toObject().value("edges").toArray()) {
        const QJsonObject edge = value.toObject();
        const qint64 source = edge.value("source").toInteger();
        const qint64 target = edge.value("target").toInteger();
        const QString in = books.contains(target) ? books.value(target) : books.value(source);
        print(stdout,
              QString("%1\t%2 — %3 → %4%5%6")
                  .arg(edge.value("id").toInteger())
                  .arg(names.value(source), edge.value("kind").toString(), names.value(target), in.isEmpty() ? QString() : " [" + in + "]",
                       edge.value("note").toString().isEmpty() ? QString() : ": " + edge.value("note").toString()));
    }
}

// A view as lines: the things, then how they relate, by name.
void GraphPrint::printView(const QJsonObject &view) {
    QHash<qint64, QString> names;
    for (const auto &value : view.value("nodes").toArray()) {
        names.insert(value.toObject().value("id").toInteger(), value.toObject().value("name").toString());
        print(stdout, nodeLine(value.toObject()));
    }
    for (const auto &value : view.value("edges").toArray()) {
        const QJsonObject edge = value.toObject();
        print(stdout, QString("%1\t%2 — %3 → %4")
                          .arg(edge.value("id").toInteger())
                          .arg(names.value(edge.value("source").toInteger()), edge.value("kind").toString(),
                               names.value(edge.value("target").toInteger())));
    }
    // Cut at a limit, it says so: how many there are in all, and how to reach the rest.
    if (const QString cut = view.value("cut").toString(); !cut.isEmpty()) {
        const QJsonObject total = view.value("total").toObject();
        print(stdout, QString("\nShown: the %1 %2 of %3 things, and %4 of %5 relations. %6")
                          .arg(view.value("nodes").toArray().size())
                          .arg(cut)
                          .arg(total.value("nodes").toInteger())
                          .arg(view.value("edges").toArray().size())
                          .arg(total.value("edges").toInteger())
                          .arg(cut == "nearest" ? "Look around one of them with: omabook graph node <id>"
                                                : "Ask for more with --limit, or look around one thing with: omabook graph node <id>"));
    }
    // A graph someone shared, after one's own and apart from it.
    for (const auto &value : view.value("layers").toArray()) {
        const QJsonObject layer = value.toObject();
        const QString owner = layer.value("owner").toString();
        print(stdout, QString("\nFrom %1 (%2), as far as you have read:")
                          .arg(owner.isEmpty() ? QString("a shared graph") : owner + "'s shared graph", layer.value("name").toString()));
        printView(layer);
    }
}

const char *const GraphPrint::usage =
    "Usage: omabook graph <command> [options]\n\n"
    "The knowledge graph: the people, places, ideas, plot threads and themes of the books being read, the real-world\n"
    "topics they touch, the questions asked, and what was read when. It is never ahead of a book's reader.\n\n"
    "  show [book] [--upto section] [--scope s]   A book's graph (up to a place), or a whole scope\n"
    "  find <words> [--kind k] [--scope s|--everywhere]  Search names, other names and summaries\n"
    "  node <id> [--depth n] [--timeline] [--book b [--upto section]]\n"
    "                                             One thing, where it was met, and what is around it; with\n"
    "                                             --timeline, when each thing known of it was revealed\n"
    "  add --kind k --name n [--summary s] [--alias a] [--book id [--section n]] [--scope s] [--data json]\n"
    "  link <a> <b> <relation> [--note text]      Relate two nodes: a \"mentor of\" b\n"
    "  unlink <edge>                              Remove a relation\n"
    "  merge <keep> <other>                       Fold one node into another\n"
    "  remove <id>                                Remove a node and its relations\n"
    "  build <book> [--upto section|--all] [--again]  Grow a book's graph with the AI, as far as it was read\n"
    "  connect <book>|--read [--estimate] [--again]  Have the AI link a book's themes and ideas to the other books read\n"
    "  export [book] --format json|graphml|dot|cypher [--output file] [--scope s] [--upto section]\n"
    "  scopes                                     The graphs there are: the general one (\"\") and made-up worlds\n"
    "  copy-scope <from> <to>, remove-scope <s>   Fork a graph under a new name; throw one away\n"
    "  log [book]                                 The reading log: what was read on which day\n"
    "  sync                                       Bring the books' own nodes in step with the library\n"
    "  take <file>                                Merge in another computer's graph (what omabook sync sends)\n\n"
    "Kinds: character, place, group, concept, item, event, thread, theme, topic, question, book, author, series, tag.\n"
    "chronology [book]: the events in the order they happen (omabook chronology). Every command takes --json and --library <folder>.\n\n"
    "The graph of a library kept apart by its top-level collections is read, and added to by hand, in one of them at a time:\n"
    "--collection names it (or \"unfiled\"). Without it, that of the book or the thing named, or the library's only one;\n"
    "a scope of its own (--scope) is read whole, and `scopes` lists those of every one.";
