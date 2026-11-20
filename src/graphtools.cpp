#include "graphtools.h"

#include "graphbatch.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <cstdio>
#include <expected>
#include <functional>

#include "bookops.h"
#include "graphchoice.h"
#include "graphcommands.h"
#include "graphcontext.h"
#include "graphcut.h"
#include "graphlinks.h"
#include "graphprint.h"
#include "graphwalk.h"
#include "mcp.h"
#include "settings.h"
#include "tenancy.h"

namespace {

QJsonObject problem(const QString &why) { return {{"error", why}}; }

QJsonObject placeJson(const GraphPlace &place) {
    return {{"book", place.bookId}, {"section", place.section}, {"fraction", place.fraction}};
}

} // namespace

bool GraphOps::failed(const QJsonObject &answer) { return answer.contains("error"); }

std::optional<GraphPlace> GraphOps::place(qint64 bookId, const QString &section, const QString &fraction) {
    bool number = false;
    const int at = section.toInt(&number);
    if (!number || at < 0)
        return std::nullopt;
    return GraphPlace{bookId, at, fraction.isEmpty() ? 1.0 : qBound(0.0, fraction.toDouble(), 1.0)};
}

QJsonObject GraphOps::add(Graph graph, const QJsonObject &thing) {
    const Library &library = graph.library();
    const QString kind = thing.value("kind").toString().simplified(), name = thing.value("name").toString().simplified();
    if (kind.isEmpty() || name.isEmpty())
        return problem("A node needs a kind and a name.");
    const qint64 bookId = thing.value("book").toInteger();
    if (bookId > 0 && !library.book(bookId))
        return problem(QString("There is no book %1 in the library.").arg(bookId));
    QJsonObject data = thing.value("data").toObject();
    if (thing.value("aliases").isArray())
        data.insert("aliases", thing.value("aliases"));
    std::optional<GraphPlace> seen;
    if (bookId > 0) {
        // Met where it is said to be, or where the book was read to.
        const Progress read = library.progress(bookId);
        seen = thing.contains("section") ? GraphPlace{bookId, thing.value("section").toInt(), thing.value("fraction").toDouble()}
                                         : GraphPlace{bookId, read.place};
    }
    if (bookId > 0 && !graph.sees(bookId))
        return problem(QString("Book %1 is not in %2.").arg(bookId).arg(Tenancy::nameOf(library, graph.tenant().value_or(Tenant::unfiled()))));
    const qint64 id = graph.upsert(thing.value("scope").toString(), kind, name, thing.value("summary").toString(), data, seen);
    // One known by that name in another tenant is brought up to date there, and not shown here.
    const auto added = id == 0 ? std::nullopt : graph.node(id);
    if (!added)
        return problem("The node could not be added.");
    return {{"node", added->toJson()}};
}

QJsonObject GraphOps::link(Graph graph, qint64 source, qint64 target, const QString &relation, const QString &note,
                           const std::optional<GraphPlace> &learned) {
    if (!graph.node(source) || !graph.node(target))
        return problem(QString("There is no node %1 in the graph.").arg(graph.node(source) ? target : source));
    const qint64 id = graph.link(source, target, relation, note, learned);
    if (id == 0)
        return problem("Two different nodes and a relation between them are needed.");
    return {{"edge", id}, {"source", source}, {"target", target}, {"kind", relation.simplified().toLower()}};
}

QJsonObject GraphOps::unlink(Graph graph, qint64 edge) {
    return graph.unlink(edge) ? QJsonObject{{"removed_edge", edge}} : problem("That relation could not be removed.");
}

QJsonObject GraphOps::merge(Graph graph, qint64 keep, qint64 other) {
    const auto kept = graph.merge(keep, other) ? graph.node(keep) : std::nullopt;
    if (!kept)
        return problem("There are no two such nodes to merge.");
    return {{"node", kept->toJson()}, {"folded_in", other}};
}

QJsonObject GraphOps::remove(Graph graph, qint64 id) {
    const auto node = graph.node(id);
    if (!node || !graph.remove(id))
        return problem(QString("There is no node %1 in the graph.").arg(id));
    return {{"removed", node->toJson()}};
}

QJsonObject GraphOps::build(Library &library, qint64 bookId, const std::optional<GraphPlace> &upTo, bool whole, bool again,
                            const GraphBuild::Asker &ask, const std::function<void(const GraphBuild::Built &)> &said) {
    if (!library.book(bookId))
        return problem(QString("There is no book %1 in the library.").arg(bookId));
    QString error;
    const auto document = BookOps::open(&library, QString::number(bookId), &error);
    if (!document)
        return problem(error);
    // As far as it was read, unless a place is named: the graph of a book
    // is not to run ahead of its reader by itself.
    const Progress read = library.progress(bookId);
    const GraphPlace goal = whole ? GraphPlace{bookId, document->count() - 1, 1.0} : upTo.value_or(GraphPlace{bookId, read.place});
    if (!whole && !upTo && read.percent <= 0)
        return problem("This book has not been read yet, and its graph goes no further than its reader. Name a section "
                       "with --upto, or take the whole book with --all.");
    if (again)
        GraphBuild::forgetBuilt(library, bookId);
    const GraphBuild::Built built = GraphBuild::build(library, *document, bookId, goal, ask, said);
    // Nothing at all could be asked: that is a failure, not a thin graph.
    if (!built.complete && built.requests == 0)
        return problem(built.problems.value(0, "The graph could not be built."));
    return {{"book", bookId}, {"requests", built.requests}, {"nodes", built.nodes}, {"edges", built.edges},
            {"sent", built.sent}, {"complete", built.complete}, {"up_to", placeJson(built.upTo)},
            {"chronology", built.times.toJson()}, {"problems", QJsonArray::fromStringList(built.problems)}};
}

QJsonObject GraphOps::connect(Library &library, qint64 bookId, const GraphBuild::Asker &ask, bool again, bool estimate) {
    if (!library.book(bookId))
        return problem(QString("There is no book %1 in the library.").arg(bookId));
    if (estimate) {
        const auto at = GraphBuild::builtTo(library, bookId);
        const GraphLinks::Offer offer = at ? GraphLinks::offer(library, bookId, *at) : GraphLinks::Offer();
        return {{"book", bookId}, {"requests", again ? (offer.empty() ? 0 : 1) : (GraphLinks::due(library, bookId) ? 1 : 0)},
                {"own", offer.own.size()}, {"others", offer.others.size()}};
    }
    const GraphLinks::Connected connected = GraphLinks::connect(library, bookId, ask, again);
    if (!connected.asked && !connected.problem.isEmpty())
        return problem(connected.problem);
    QJsonObject answer{{"book", bookId}, {"asked", connected.asked}, {"links", connected.links}, {"topics", connected.topics}};
    if (!connected.skipped.isEmpty())
        answer.insert("skipped", connected.skipped);
    if (!connected.problem.isEmpty())
        answer.insert("problems", QJsonArray{connected.problem});
    // The connections the book now has, as they read in its tenant: both ends by name.
    const Graph graph = Graph::ofReader(library, bookId);
    const GraphView beyond = graph.beyond(bookId);
    QHash<qint64, GraphNode> ends;
    for (const GraphNode &node : graph.ofBook(bookId).nodes + beyond.nodes)
        ends.insert(node.id, node);
    QJsonArray connections;
    for (const GraphEdge &edge : beyond.edges + graph.ofBook(bookId).edges) {
        if (!edge.across())
            continue;
        QJsonObject object = edge.toJson();
        object.insert("relation", edge.relation());
        object.insert("from", ends.value(edge.source).name);
        object.insert("to", ends.value(edge.target).name);
        for (const qint64 end : {edge.source, edge.target})
            for (const GraphPlace &place : graph.seen(end))
                if (place.bookId != bookId && library.book(place.bookId)
                    && !object.contains("other_book"))
                    object.insert("other_book", library.book(place.bookId)->meta.title);
        connections << object;
    }
    answer.insert("connections", connections);
    return answer;
}

QJsonObject GraphOps::copyScope(Graph graph, const QString &from, const QString &to) {
    if (!graph.copyScope(from, to))
        return problem("A scope is copied to a new name: \"" + to + "\" is taken, empty, or there is nothing in \"" + from + "\".");
    return {{"scope", to}, {"copied_from", from}};
}

QJsonObject GraphOps::removeScope(Library &library, const QString &scope) {
    Graph graph(library);
    if (scope.isEmpty() || !graph.scopes().contains(scope) || !graph.removeScope(scope))
        return problem(scope.isEmpty() ? QString("The general graph is not a scope to remove.")
                                       : "There is no scope \"" + scope + "\".");
    return {{"removed_scope", scope}};
}

QJsonObject GraphOps::sync(Library &library) {
    Graph graph(library);
    graph.syncLibrary();
    return {{"books", graph.find({}, "book", QString(), 1000000).size()}};
}

// ---------------------------------------------------------------------------
// The command line.

namespace {

constexpr auto usage =
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

// `connect`: one book, or every book read, each compared with the others once.
GraphCommands::Kept connectBooks(Library &library, const QCommandLineParser &parser, const QStringList &words) {
    using GraphPrint::print;
    const auto ask = [](const AiRequest &request, QString *reply, QString *why) { return BookOps::ask(request, reply, why); };
    const bool single = words.size() == 2;
    const QList<qint64> books = single ? QList<qint64>{words.value(1).toLongLong()} : GraphBatch::readBooks(library);
    QJsonArray each;
    int requests = 0;
    int links = 0;
    int topics = 0;
    for (const qint64 id : books) {
        const QJsonObject one = GraphOps::connect(library, id, ask, parser.isSet("again"), parser.isSet("estimate"));
        if (GraphOps::failed(one) && single)
            return {one, {}};
        requests += one.value("requests").toInt() + (one.value("asked").toBool() ? 1 : 0);
        links += one.value("links").toInt();
        topics += one.value("topics").toInt();
        each << one;
    }
    const QJsonObject answer =
        single ? each.first().toObject() : QJsonObject{{"books", each}, {"requests", requests}, {"links", links}, {"topics", topics}};
    return {answer, [each, single, requests, links, topics, estimate = parser.isSet("estimate")](const QJsonObject &) {
                if (estimate) {
                    print(stdout, QString("%1 requests, about %2 tokens.").arg(requests).arg(requests * 4000));
                    return;
                }
                for (const auto &one : each) {
                    if (!one.toObject().value("skipped").toString().isEmpty() && single)
                        print(stdout, one.toObject().value("skipped").toString());
                    for (const auto &said : one.toObject().value("problems").toArray())
                        print(stderr, said.toString());
                    if (single)
                        for (const auto &value : one.toObject().value("connections").toArray()) {
                            const QJsonObject edge = value.toObject();
                            print(stdout, QString("%1 — %2 → %3 [%4]%5")
                                              .arg(edge.value("from").toString(), edge.value("relation").toString(),
                                                   edge.value("to").toString(), edge.value("other_book").toString(),
                                                   edge.value("note").toString().isEmpty() ? QString() : ": " + edge.value("note").toString()));
                        }
                }
                print(stdout, QString("%1 new connections and %2 shared topics, in %3 requests.").arg(links).arg(topics).arg(requests));
            }};
}

} // namespace

int runGraphCli(const QStringList &arguments) {
    using namespace GraphPrint;
    QCommandLineParser parser;
    parser.setApplicationDescription("The knowledge graph of what is read.");
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library", "Use the library in this folder instead of the current one.", "folder"});
    parser.addOption({"scope", "A graph of its own, such as world:42; the general graph when not given.", "scope"});
    parser.addOption({"collection", "The top-level collection (or \"unfiled\") whose graph to read or add to.", "name"});
    parser.addOption({"everywhere", "find: search every scope."});
    parser.addOption({"upto", "Up to and including this section, counted from 0.", "section"});
    parser.addOption({"fraction", "With --upto or --section: how far into that section, from 0 to 1.", "number"});
    parser.addOption({"kind", "The kind of node.", "kind"});
    parser.addOption({"name", "add: the node's name.", "name"});
    parser.addOption({"summary", "add: what is known of it.", "text"});
    parser.addOption({"alias", "add: another name it goes by; may be given more than once.", "name"});
    parser.addOption({"data", "add: anything else to keep with it, as a JSON object.", "json"});
    parser.addOption({"book", "add, link: the book it was met or learned in, by its number.", "id"});
    parser.addOption({"section", "add, link: the section of that book.", "section"});
    parser.addOption({"note", "link: a remark on the relation.", "text"});
    parser.addOption({"depth", "node: how many relations away to go; 1 when not given.", "number"});
    parser.addOption({"timeline", "node: also when each thing known of it was revealed, and where."});
    parser.addOption({"limit", "show, find: the most nodes to answer.", "number"});
    parser.addOption({"format", "export: json, graphml, dot or cypher.", "format"});
    parser.addOption({"output", "export: write to this file instead of printing.", "file"});
    parser.addOption({"beyond", "show: with a book, also what it connects to in other books."});
    parser.addOption({"no-connect", "build: don't then look for connections to the other books read."});
    parser.addOption({"all", "build: take the whole book, read or not."});
    parser.addOption({"again", "build: start over from the first page."});
    parser.addOption({"read", "build: every book that was read or is being read, instead of naming them."});
    parser.addOption({"query", "build: the books a search finds, as omabook list takes it.", "search"});
    parser.addOption({"estimate", "build: only say what it would send, without sending anything."});
    parser.addOption({{"h", "help"}, "Show this help."});
    QStringList rest = arguments;
    rest.removeAt(1);
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    const QString sub = words.value(0);
    const bool json = parser.isSet("json");
    if (sub.isEmpty() || parser.isSet("help")) {
        print(stdout, usage);
        return 0;
    }

    Library library(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    QString error;
    if (!library.open(&error)) {
        print(stderr, error);
        return 1;
    }
    const QString scope = parser.value("scope");
    const qint64 first = words.value(1).toLongLong(), second = words.value(2).toLongLong();
    // The graph, as each command names a tenant, a book, a thing or a world.
    const std::expected<GraphChoice::Chosen, QString> chosen = GraphChoice::of(library, parser, sub, first);
    if (!chosen) {
        print(stderr, chosen.error());
        return 1;
    }
    const Graph &graph = chosen->graph;
    auto upTo = [&](qint64 bookId) { return GraphOps::place(bookId, parser.value("upto"), parser.value("fraction")); };
    auto wrong = [&] {
        print(stderr, usage);
        return 1;
    };

    QJsonObject answer;
    std::function<void()> plain; // how the answer reads without --json
    if (sub == "show") {
        answer = GraphOps::show(graph, first, scope, upTo(first), parser.isSet("limit") ? parser.value("limit").toInt() : 500,
                                parser.isSet("beyond"));
        plain = [&] {
            printView(answer);
            if (answer.contains("beyond")) {
                print(stdout, "\nBeyond this book:");
                printBeyond(answer);
            }
            if (answer.value("nodes").toArray().isEmpty())
                print(stdout, first > 0
                                  ? "Nothing is in the graph for this book yet. Grow it with: omabook graph build " + QString::number(first)
                                  : QString("This graph is empty."));
        };
    } else if (sub == "find") {
        answer = GraphOps::find(graph, words.mid(1).join(' '), parser.value("kind"), scope, parser.isSet("everywhere"),
                                parser.isSet("limit") ? parser.value("limit").toInt() : 50);
        plain = [&] { printView(answer); };
    } else if (sub == "node" && words.size() == 2) {
        const qint64 inBook = parser.value("book").toLongLong();
        answer = GraphOps::node(graph, first, parser.isSet("depth") ? parser.value("depth").toInt() : 1,
                                parser.isSet("timeline"), inBook, upTo(inBook));
        plain = [&] {
            for (const QJsonValue &value : answer.value("seen").toArray())
                print(stdout, QString("met in %1, section %2")
                                  .arg(value.toObject().value("title").toString())
                                  .arg(value.toObject().value("section").toInt()));
            printView(answer);
            if (answer.contains("timeline"))
                print(stdout, "\nWhen it was revealed:");
            // "section 3\tsummary\tShe is the Regent's daughter.\t“the Regent's daughter”"
            for (const QJsonValue &value : answer.value("timeline").toArray()) {
                const QJsonObject step = value.toObject(), at = step.value("revealed").toObject();
                const QString what = step.value("what").toString();
                const QString text = what == "relation" ? (step.value("outgoing").toBool()
                                                               ? step.value("text").toString() + " " + step.value("other_name").toString()
                                                               : step.value("other_name").toString() + " " + step.value("text").toString())
                                                        : step.value("text").toString();
                print(stdout,
                      QString("%1\t%2\t%3%4%5")
                          .arg(at.isEmpty() ? QString("by hand") : QString("section %1").arg(at.value("section").toInt()), what, text,
                               at.value("anchor").toString().isEmpty() ? QString() : "\t“" + at.value("anchor").toString() + "”",
                               step.value("ahead").toBool() ? QString("\t(ahead of the reader)") : QString()));
            }
        };
    } else if (sub == "add") {
        QJsonObject thing{
            {"kind", parser.value("kind")}, {"name", parser.value("name")}, {"summary", parser.value("summary")}, {"scope", scope}};
        if (parser.isSet("alias"))
            thing.insert("aliases", QJsonArray::fromStringList(parser.values("alias")));
        if (parser.isSet("data")) {
            const QJsonDocument data = QJsonDocument::fromJson(parser.value("data").toUtf8());
            if (!data.isObject()) {
                print(stderr, "--data takes a JSON object, such as '{\"url\": \"...\"}'.");
                return 1;
            }
            thing.insert("data", data.object());
        }
        if (parser.isSet("book"))
            thing.insert("book", parser.value("book").toLongLong());
        if (parser.isSet("section")) {
            thing.insert("section", parser.value("section").toInt());
            thing.insert("fraction", parser.value("fraction").toDouble());
        }
        answer = GraphOps::add(graph, thing);
        plain = [&] { print(stdout, nodeLine(answer.value("node").toObject())); };
    } else if (sub == "link" && words.size() >= 4) {
        // Learned in a book: at the section named, or where it was read to.
        const qint64 bookId = parser.value("book").toLongLong();
        const Progress read = library.progress(bookId);
        std::optional<GraphPlace> learned;
        if (bookId > 0 && library.book(bookId))
            learned = parser.isSet("section") ? GraphOps::place(bookId, parser.value("section"), parser.value("fraction"))
                                              : std::optional(GraphPlace{bookId, read.place});
        answer = GraphOps::link(graph, first, second, words.mid(3).join(' '), parser.value("note"), learned);
        plain = [&] { print(stdout, QString("Relation %1 is in the graph.").arg(answer.value("edge").toInteger())); };
    } else if (sub == "unlink" && words.size() == 2) {
        answer = GraphOps::unlink(graph, first);
        plain = [&] { print(stdout, "Removed the relation."); };
    } else if (sub == "merge" && words.size() == 3) {
        answer = GraphOps::merge(graph, first, second);
        plain = [&] { print(stdout, nodeLine(answer.value("node").toObject())); };
    } else if (sub == "remove" && words.size() == 2) {
        answer = GraphOps::remove(graph, first);
        plain = [&] { print(stdout, "Removed " + answer.value("removed").toObject().value("name").toString() + "."); };
    } else if (sub == "build" && (words.size() > 2 || parser.isSet("read") || parser.isSet("query") || parser.isSet("estimate"))) {
        // Several books at once, after the fact: named, found, or all that were read.
        GraphBatch::Choice choice;
        for (const QString &word : words.mid(1))
            choice.books << word.toLongLong();
        if (parser.isSet("read"))
            choice.books += GraphBatch::readBooks(library);
        if (parser.isSet("query"))
            for (const Book &book : library.books(parser.value("query")))
                choice.books << book.id;
        choice.whole = parser.isSet("all");
        choice.again = parser.isSet("again");
        if (parser.isSet("estimate")) {
            const GraphBatch::Plan planned = GraphBatch::plan(library, choice);
            answer = planned.toJson();
            plain = [&, planned] {
                for (const GraphBatch::Plan::Item &item : planned.items)
                    print(stdout, QString("%1\t%2\t%3")
                                      .arg(item.id)
                                      .arg(item.title, item.skipped.isEmpty() ? QString("%1 requests").arg(item.requests) : item.skipped));
                print(stdout,
                      QString("%1 books, %2 requests, about %3 tokens.").arg(planned.books).arg(planned.requests).arg(planned.tokens()));
            };
        } else {
            const auto ask = [](const AiRequest &request, QString *reply, QString *why) { return BookOps::ask(request, reply, why); };
            const GraphBatch::Done done =
                GraphBatch::run(library, choice, ask, {}, [&](int index, int total, const QString &title, const GraphBatch::Done &soFar) {
                    if (!json)
                        print(stderr, QString("%1 of %2, %3: %4 things and %5 relations so far")
                                          .arg(index + 1)
                                          .arg(total)
                                          .arg(title)
                                          .arg(soFar.nodes)
                                          .arg(soFar.edges));
                });
            answer = done.toJson();
            plain = [&, done] {
                for (const QString &said : done.problems)
                    print(stderr, said);
                print(stdout, QString("%1 books: %2 things and %3 relations, in %4 requests.")
                                  .arg(done.books)
                                  .arg(done.nodes)
                                  .arg(done.edges)
                                  .arg(done.requests));
            };
        }
    } else if (sub == "build" && words.size() == 2) {
        const auto ask = [](const AiRequest &request, QString *reply, QString *why) { return BookOps::ask(request, reply, why); };
        answer = GraphOps::build(
            library, first, upTo(first), parser.isSet("all"), parser.isSet("again"), ask, [&](const GraphBuild::Built &built) {
                if (!json)
                    print(
                        stderr,
                        QString("section %1: %2 things and %3 relations so far").arg(built.upTo.section).arg(built.nodes).arg(built.edges));
            });
        // Then, as in the window, what the book connects to in the others
        // read: once per build, and only where growing by itself is wanted.
        if (!GraphOps::failed(answer) && answer.value("complete").toBool() && !parser.isSet("no-connect") && Settings().graph() == "auto") {
            const QJsonObject connected = GraphOps::connect(library, first, ask);
            if (!GraphOps::failed(connected)) {
                answer.insert("requests", answer.value("requests").toInt() + (connected.value("asked").toBool() ? 1 : 0));
                answer.insert("connections", connected.value("links").toInt() + connected.value("topics").toInt());
            }
        }
        plain = [&] {
            for (const QJsonValue &said : answer.value("problems").toArray())
                print(stderr, said.toString());
            if (answer.value("connections").toInt() > 0)
                print(stdout, QString("%1 connections to other books; see: omabook graph show %2 --beyond")
                                  .arg(answer.value("connections").toInt())
                                  .arg(first));
            print(stdout, QString("%1 things and %2 relations from %3 characters of text, in %4 requests.%5")
                              .arg(answer.value("nodes").toInt())
                              .arg(answer.value("edges").toInt())
                              .arg(answer.value("sent").toInteger())
                              .arg(answer.value("requests").toInt())
                              .arg(answer.value("complete").toBool() ? "" : " It stopped early; run it again to go on."));
        };
    } else if (sub == "connect" && (words.size() == 2 || parser.isSet("read"))) {
        const GraphCommands::Kept connected = connectBooks(library, parser, words);
        answer = connected.answer;
        plain = [&answer, show = connected.show] { show(answer); };
    } else if (sub == "export") {
        answer = GraphOps::exported(graph, first, scope, upTo(first), parser.isSet("format") ? parser.value("format") : "json",
                                    parser.value("output"));
        plain = [&] {
            if (answer.contains("file"))
                print(stdout, "Wrote " + answer.value("file").toString());
            else
                fputs(answer.value("content").toString().toUtf8().constData(), stdout);
        };
    } else if (auto kept = GraphCommands::housekeeping(library, graph, sub, words, chosen->everyTenant)) {
        answer = kept->answer;
        plain = [&answer, show = kept->show] { show(answer); };
    } else {
        return wrong();
    }

    if (GraphOps::failed(answer)) {
        print(stderr, answer.value("error").toString());
        return 1;
    }
    if (json)
        print(stdout, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Indented)).trimmed());
    else
        plain();
    return 0;
}

// ---------------------------------------------------------------------------
// The MCP tools: each is one of the commands above.

namespace {

using namespace Mcp;

QString numberOf(const QJsonObject &arguments, const QString &key) { return QString::number(arguments.value(key).toInteger()); }

// "--upto <section>" (and how far into it) when the arguments name a place.
QStringList upToArguments(const QJsonObject &arguments) {
    QStringList asked;
    if (arguments.contains("up_to_section"))
        asked << "--upto" << numberOf(arguments, "up_to_section");
    if (arguments.contains("up_to_fraction"))
        asked << "--fraction" << QString::number(arguments.value("up_to_fraction").toDouble());
    return asked;
}

// "--collection <name>" when the arguments name the tenant to read or write in.
QStringList inCollection(const QJsonObject &arguments) {
    return arguments.contains("collection") ? QStringList{"--collection", arguments.value("collection").toString()} : QStringList();
}
QJsonObject collection() {
    return string("The top-level collection (or \"unfiled\") whose graph to read or add to: a library kept apart "
                                      "by its top-level collections has a graph for each. Not needed for a book or a thing in one "
                  "alone, nor in a library with only one; the error says which there are.");
}

const QJsonObject bookNumber = integer("The book's number in the library.", 1);
const QJsonObject nodeNumber = integer("The node's number, as graph_find and graph_book give it.", 1);
constexpr auto kinds = "character, place, group, concept, item, event, thread, theme, topic, question, book, author, series or tag";

const bool registered = [] {
    registerTool({"graph_find", "Search the knowledge graph",
                  "Finds things in the knowledge graph Omabook keeps of what is read: the people, places, groups, "
                  "ideas (a magic system, a technology), items, events, plot threads and themes of books, the "
                  "real-world topics they touch, the questions that were asked, and the books, authors, series and "
                  "tags themselves. Every word must be in a thing's name, other names or summary.",
                  schema({{"collection", collection()}, {"words", string("What to look for; nothing for every node of a kind.")},
                          {"kind", string(QString("Only this kind: ") + kinds + ".")},
                          {"scope", string("A graph of its own, such as world:42; the general graph when not given.")},
                          {"everywhere", boolean("Search every scope.")},
                          {"limit", integer("The most nodes to answer; 50 when not given.", 1)}}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "find", arguments.value("words").toString()};
                      if (arguments.contains("kind")) asked << "--kind" << arguments.value("kind").toString();
                      if (arguments.contains("scope")) asked << "--scope" << arguments.value("scope").toString();
                      if (arguments.value("everywhere").toBool()) asked << "--everywhere";
                      if (arguments.contains("limit")) asked << "--limit" << numberOf(arguments, "limit");
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_node", "A node and what is around it",
                  "One thing of the knowledge graph: what is known of it, the books and places it was met at, and the nearest "
                  "things within a few relations of it with those relations (\"total\" counts them all, \"cut\" says not all "
                  "are answered). With timeline, also when each thing known of it was revealed: every summary it had, every "
                  "other name it was given, where it was met and each relation, each with its book, section and fraction and "
                  "a few words of the text there (\"anchor\") to find the very spot; what lies past the reader's furthest place "
                  "in its book is marked \"ahead\" and gives away what happens. With book and up_to_section, the node as a "
                  "reader at that place knows it, and nothing later.",
                  schema({{"collection", collection()}, {"id", nodeNumber}, {"depth", integer("How many relations away to go; 1 when not given.", 0)},
                          {"timeline", boolean("Also when each thing known of it was revealed, and where.")},
                          {"book", integer("Only what this book revealed, by its number.", 1)},
                          {"up_to_section", integer("With book: as known up to and including this section.", 0)},
                          {"up_to_fraction", Mcp::number("How far into that section, from 0 to 1; all of it when not given.", 0, 1)}},
                         {"id"}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "node", numberOf(arguments, "id")};
                      if (arguments.contains("depth")) asked << "--depth" << numberOf(arguments, "depth");
                      if (arguments.value("timeline").toBool()) asked << "--timeline";
                      if (arguments.contains("book")) asked << "--book" << numberOf(arguments, "book") << upToArguments(arguments);
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_book", "A book's knowledge graph",
                  "What a book's reader has met: the things seen in it and the relations learned in it. The graph is "
                  "grown as the book is read, so it is never ahead of the reader; up_to_section cuts it at an "
                  "earlier place still, for what was known by then.",
                  schema({{"collection", collection()}, {"id", bookNumber}, {"up_to_section", integer("Only what was met up to and including this section.", 0)},
                          {"up_to_fraction", Mcp::number("How far into that section, from 0 to 1; all of it when not given.", 0, 1)},
                          {"beyond", boolean("Also what the book connects to elsewhere, as \"beyond\": things of other books "
                                             "tied to it by a connection across books, and the other books that share a thing with it.")}},
                         {"id"}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      QStringList asked = QStringList{"graph", "show", numberOf(arguments, "id")} + upToArguments(arguments);
                      if (arguments.value("beyond").toBool()) asked << "--beyond";
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_connect", "Connect a book to the others read",
                  "Has the model Omabook is set up with compare a book's most telling things (themes, ideas, plot "
                  "threads, main characters) with those of the other books the reader has read, and add what "
                  "connects them as relations across books: echoes, contrasts with, same idea as, answers, inspired "
                  "by, refers to, each with a sentence saying why. Only what was read of either book is used. It is "
                  "one request, and none when the same comparison was made before; with estimate nothing is sent "
                  "and the answer says whether a request would be.",
                  schema({{"id", bookNumber}, {"estimate", boolean("Send nothing; say what it would take.")},
                          {"again", boolean("Ask even when the same comparison was made before.")}},
                         {"id"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "connect", numberOf(arguments, "id")};
                      if (arguments.value("estimate").toBool()) asked << "--estimate";
                      if (arguments.value("again").toBool()) asked << "--again";
                      return command(asked);
                  }, true});

    registerTool({"graph_scope", "A whole graph",
                  "A whole graph, the most connected things first (\"total\" counts them all, \"cut\" says not all are answered): the "
                  "general graph of everything read, or a scope of its own (a made-up world a plugin keeps). For one book use graph_book.",
                  schema({{"collection", collection()}, {"scope", string("The scope, such as world:42; the general graph when not given.")},
                          {"limit", integer("The most nodes to answer; 500 when not given.", 1)}}),
                  ReadOnly, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "show"};
                      if (arguments.contains("scope")) asked << "--scope" << arguments.value("scope").toString();
                      if (arguments.contains("limit")) asked << "--limit" << numberOf(arguments, "limit");
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_scopes", "The graphs there are",
                  "Lists the graphs: the general one (an empty scope) and every scope of its own, each with how many "
                  "nodes it holds.",
                  schema({{"collection", collection()}}), ReadOnly, [](const QJsonObject &arguments) { return command(QStringList{"graph", "scopes"} + inCollection(arguments)); }});

    registerTool({"graph_add", "Add or update a node",
                  "Adds a thing to the knowledge graph, or brings the one already known by that name (or one of its "
                  "other names) in the same scope and of the same kind up to date: a summary given replaces the old "
                  "one, data is merged, aliases are added to.",
                  schema({{"collection", collection()}, {"kind", string(QString("What kind of thing: ") + kinds + ", or a kind of your own.")},
                          {"name", string("Its name.")},
                          {"summary", string("What is known of it, in a sentence or a few.")},
                          {"aliases", list("Other names it goes by.", string("A name."))},
                          {"data", object("Anything else to keep with it.")},
                          {"scope", string("The graph to add it to; the general graph when not given.")},
                          {"book", integer("The book it was met in, by its number.", 1)},
                          {"section", integer("The section of that book it was met in; where the book was read to when not given.", 0)}},
                         {"kind", "name"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "add", "--kind", arguments.value("kind").toString(), "--name",
                                        arguments.value("name").toString()};
                      if (arguments.contains("summary")) asked << "--summary" << arguments.value("summary").toString();
                      for (const QJsonValue &alias : arguments.value("aliases").toArray()) asked << "--alias" << alias.toString();
                      if (arguments.contains("data"))
                          asked << "--data" << QString::fromUtf8(QJsonDocument(arguments.value("data").toObject()).toJson(QJsonDocument::Compact));
                      if (arguments.contains("scope")) asked << "--scope" << arguments.value("scope").toString();
                      if (arguments.contains("book")) asked << "--book" << numberOf(arguments, "book");
                      if (arguments.contains("section")) asked << "--section" << numberOf(arguments, "section");
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_link", "Relate two nodes",
                  "Says how one thing relates to another, as a short verb phrase read from the first to the second: "
                  "\"mentor of\", \"member of\", \"alludes to\". Saying the same again only updates the note.",
                  schema({{"collection", collection()}, {"source", nodeNumber}, {"target", nodeNumber}, {"relation", string("The relation, read from source to target.")},
                          {"note", string("A remark on it.")},
                          {"book", integer("The book it was learned in, by its number.", 1)},
                          {"section", integer("The section of that book it was learned in.", 0)}},
                         {"source", "target", "relation"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "link", numberOf(arguments, "source"), numberOf(arguments, "target"),
                                        arguments.value("relation").toString()};
                      if (arguments.contains("note")) asked << "--note" << arguments.value("note").toString();
                      if (arguments.contains("book")) asked << "--book" << numberOf(arguments, "book");
                      if (arguments.contains("section")) asked << "--section" << numberOf(arguments, "section");
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_unlink", "Remove a relation",
                  "Removes one relation between two nodes, by the relation's own number as graph_node gives it. The nodes stay.",
                  schema({{"collection", collection()}, {"edge", integer("The relation's number.", 1)}}, {"edge"}), Destructive,
                  [](const QJsonObject &arguments) { return command(QStringList{"graph", "unlink", numberOf(arguments, "edge")} + inCollection(arguments)); }});

    registerTool({"graph_merge", "Fold one node into another",
                  "Folds a node into another when they are the same thing under two names: its relations, the places "
                  "it was met at and its names move over to the one kept, and it is gone.",
                  schema({{"collection", collection()}, {"keep", nodeNumber}, {"other", integer("The node that is folded into it.", 1)}}, {"keep", "other"}),
                  Destructive, [](const QJsonObject &arguments) {
                      return command(QStringList{"graph", "merge", numberOf(arguments, "keep"), numberOf(arguments, "other")} + inCollection(arguments));
                  }});

    registerTool({"graph_remove", "Remove a node",
                  "Removes a node from the knowledge graph, with its relations. This can't be undone.",
                  schema({{"collection", collection()}, {"id", nodeNumber}}, {"id"}), Destructive,
                  [](const QJsonObject &arguments) { return command(QStringList{"graph", "remove", numberOf(arguments, "id")} + inCollection(arguments)); }});

    registerTool({"graph_build", "Grow a book's graph with Omabook's AI",
                  "Has the model Omabook is set up with read a book and add what it finds to the knowledge graph: "
                  "its people, places, groups, ideas, items, events, plot threads, themes and the real-world topics "
                  "it touches, each with where it was learned. Only what was not sent before is sent, in several "
                  "requests for a long book, which costs AI calls. It goes as far as the book was read, unless a "
                  "section is named or whole is true.",
                  schema({{"id", bookNumber}, {"up_to_section", integer("Go up to and including this section.", 0)},
                          {"whole", boolean("Take the book to its end, read or not.")},
                          {"again", boolean("Start over from the first page; what is in the graph is brought up to date.")}},
                         {"id"}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked = QStringList{"graph", "build", numberOf(arguments, "id")} + upToArguments(arguments);
                      if (arguments.value("whole").toBool()) asked << "--all";
                      if (arguments.value("again").toBool()) asked << "--again";
                      return command(asked);
                  }, true});

    registerTool({"graph_export", "The graph as a file other tools read",
                  "A book's graph, or a whole scope, as json, graphml (Gephi, yEd), dot (Graphviz) or cypher "
                  "(statements that build it in Neo4j). With output_path it is written there; without, the text is returned.",
                  schema({{"collection", collection()}, {"format", choice("The format; json when not given.", GraphView::formats())},
                          {"book", integer("The book whose graph to write; a scope when not given.", 1)},
                          {"up_to_section", integer("With a book: only what was met up to and including this section.", 0)},
                          {"scope", string("Without a book: the scope; the general graph when not given.")},
                          {"output_path", string("Where to write it, as an absolute path.")}}),
                  Changes, [](const QJsonObject &arguments) {
                      QStringList asked{"graph", "export"};
                      if (arguments.contains("book")) asked << numberOf(arguments, "book");
                      asked << "--format" << arguments.value("format").toString("json") << upToArguments(arguments);
                      if (arguments.contains("scope")) asked << "--scope" << arguments.value("scope").toString();
                      if (arguments.contains("output_path")) asked << "--output" << arguments.value("output_path").toString();
                      return command(asked + inCollection(arguments));
                  }});

    registerTool({"graph_copy_scope", "Fork a graph",
                  "Copies a whole graph under a new scope name, so a made-up world can start from what the books "
                  "say (copy the general graph, \"\") or be forked. The new name must not be taken.",
                  schema({{"collection", collection()}, {"from", string("The scope to copy; \"\" for the general graph.")}, {"to", string("The new scope's name, such as world:42.")}},
                         {"from", "to"}),
                  Changes, [](const QJsonObject &arguments) {
                      return command(QStringList{"graph", "copy-scope", arguments.value("from").toString(), arguments.value("to").toString()}
                                     + inCollection(arguments));
                  }});

    registerTool({"graph_remove_scope", "Throw a graph away",
                  "Removes a scope with everything in it. The general graph is not a scope and can't be removed this way.",
                  schema({{"scope", string("The scope to remove.")}}, {"scope"}), Destructive,
                  [](const QJsonObject &arguments) { return command({"graph", "remove-scope", arguments.value("scope").toString()}); }});

    registerTool({"graph_reading_log", "What was read when",
                  "The reading log: for each day a book was read here, from what percent of it to what percent. For "
                  "one book, or for every book.",
                  schema({{"collection", collection()}, {"book", integer("Only this book, by its number.", 1)}}), ReadOnly,
                  [](const QJsonObject &arguments) {
                      return command((arguments.contains("book") ? QStringList{"graph", "log", numberOf(arguments, "book")}
                                                                 : QStringList{"graph", "log"})
                                     + inCollection(arguments));
                  }});
    return true;
}();

} // namespace
