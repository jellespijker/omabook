#include "graphbatch.h"

#include <QJsonArray>
#include <ranges>

#include "bookops.h"
#include "formats.h"
#include "graphlinks.h"
#include "graphtools.h"

namespace {

// Whether a book is taken to its end: asked for, or the book was read.
// A book marked as read elsewhere has no place of its own to stop at.
bool toTheEnd(const Book &book, const GraphBatch::Choice &choice) { return choice.whole || book.status() == "read"; }

} // namespace

QJsonObject GraphBatch::Plan::toJson() const {
    QJsonArray array;
    for (const Item &item : items) {
        QJsonObject object{{"id", item.id}, {"title", item.title}, {"characters", item.characters}, {"requests", item.requests}};
        if (!item.skipped.isEmpty())
            object.insert("skipped", item.skipped);
        if (item.connects)
            object.insert("connects", true);
        array << object;
    }
    return {{"books", books}, {"requests", requests}, {"characters", characters}, {"tokens", tokens()}, {"items", array}};
}

QJsonObject GraphBatch::Done::toJson() const {
    return {{"books", books}, {"requests", requests}, {"nodes", nodes}, {"edges", edges}, {"connections", connections}, {"stopped", stopped},
            {"problems", QJsonArray::fromStringList(problems)}};
}

QList<qint64> GraphBatch::readBooks(const Library &library) {
    QList<qint64> ids;
    for (const Book &book : library.books())
        if (!book.formats.isEmpty() && (book.status() == "read" || book.status() == "reading"))
            ids << book.id;
    return ids;
}

GraphBatch::Plan GraphBatch::plan(Library &library, const Choice &choice) {
    Plan plan;
    for (const qint64 id : choice.books) {
        Plan::Item item;
        item.id = id;
        const auto book = library.book(id);
        if (!book) {
            item.skipped = "There is no such book.";
            plan.items << item;
            continue;
        }
        item.title = book->meta.title;
        QString error;
        const QString file = BookOps::readableFile(library, id, &error);
        const auto document = file.isEmpty() ? nullptr : Formats::open(file, &error);
        const Progress read = library.progress(id);
        if (!document) {
            item.skipped = book->formats.isEmpty() ? QString("It has no file to read from.") : error;
        } else if (!toTheEnd(*book, choice) && read.percent <= 0) {
            item.skipped = "It hasn't been read yet.";
        } else {
            const GraphPlace upTo = toTheEnd(*book, choice) ? GraphPlace{id, document->count() - 1, 1.0}
                                                             : GraphPlace{id, read.section, read.fraction};
            const auto passages = GraphBuild::passages(*document, choice.again ? std::nullopt : GraphBuild::builtTo(library, id), upTo);
            item.characters = GraphBuild::size(passages);
            item.requests = int(GraphBuild::chunks(passages).size());
            if (item.characters < GraphBuild::worthAsking) {
                item.skipped = "Its graph is already up to date.";
                item.characters = 0;
                item.requests = 0;
            }
        }
        if (item.skipped.isEmpty()) {
            // One more request compares the book with the others read, when
            // there are any: the ones before it in this run, or already built.
            if (plan.books > 0 || GraphLinks::company(library, id)) {
                ++item.requests;
                item.connects = true;
            }
            ++plan.books;
            plan.requests += item.requests;
            plan.characters += item.characters;
        }
        plan.items << item;
    }
    return plan;
}

GraphBatch::Done GraphBatch::run(Library &library, const Choice &choice, const GraphBuild::Asker &ask, const std::function<bool()> &stop,
                                 const std::function<void(int, int, const QString &, const Done &)> &said) {
    Done done;
    const Plan planned = plan(library, choice);
    const auto work = planned.items | std::views::filter([](const Plan::Item &item) { return item.skipped.isEmpty(); })
                      | std::ranges::to<QList<Plan::Item>>();
    for (const auto &[index, item] : std::views::enumerate(work)) {
        if (stop && stop()) {
            done.stopped = true;
            break;
        }
        const auto book = library.book(item.id);
        if (!book)
            continue;
        const Done before = done;
        // Stopping is saying no to the next request: the build ends there,
        // with what was answered so far in the graph and marked as built.
        const GraphBuild::Asker asker = [&](const AiRequest &request, QString *answer, QString *error) {
            if (stop && stop()) {
                done.stopped = true;
                *error = "Stopped.";
                return false;
            }
            return ask(request, answer, error);
        };
        const QJsonObject built = GraphOps::build(library, item.id, std::nullopt, toTheEnd(*book, choice), choice.again, asker,
                                                  [&](const GraphBuild::Built &now) {
                                                      done.requests = before.requests + now.requests;
                                                      done.nodes = before.nodes + now.nodes;
                                                      done.edges = before.edges + now.edges;
                                                      if (said)
                                                          said(int(index), int(work.size()), item.title, done);
                                                  });
        done.requests = before.requests + built.value("requests").toInt();
        done.nodes = before.nodes + built.value("nodes").toInt();
        done.edges = before.edges + built.value("edges").toInt();
        // Then, once for the book, what it connects to in the others read.
        if (!done.stopped && !built.contains("error")) {
            const GraphLinks::Connected connected = GraphLinks::connect(library, item.id, asker, choice.again);
            done.requests += connected.asked ? 1 : 0;
            done.nodes += connected.topics;
            done.edges += connected.links + 2 * connected.topics;
            done.connections += connected.links + connected.topics;
            if (!connected.problem.isEmpty() && !done.stopped)
                done.problems << item.title + ": " + connected.problem;
        }
        ++done.books;
        for (const QJsonValue &problem : built.value("problems").toArray())
            if (!done.stopped || problem.toString() != "Stopped.")
                done.problems << item.title + ": " + problem.toString();
        if (built.contains("error"))
            done.problems << item.title + ": " + built.value("error").toString();
        if (said)
            said(int(index), int(work.size()), item.title, done);
        if (done.stopped)
            break;
    }
    return done;
}
