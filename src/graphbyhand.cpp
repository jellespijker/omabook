#include "graphbyhand.h"

#include <QJsonArray>
#include <QJsonDocument>

#include "bookops.h"
#include "graphtools.h"
#include "library.h"

QJsonObject GraphByHand::add(const Graph &graph, const QCommandLineParser &parser) {
    QJsonObject thing{{"kind", parser.value("kind")},
                      {"name", parser.value("name")},
                      {"summary", parser.value("summary")},
                      {"scope", parser.value("scope")}};
    if (parser.isSet("alias"))
        thing.insert("aliases", QJsonArray::fromStringList(parser.values("alias")));
    if (parser.isSet("data")) {
        const QJsonDocument data = QJsonDocument::fromJson(parser.value("data").toUtf8());
        if (!data.isObject())
            return {{"error", R"(--data takes a JSON object, such as '{"url": "..."}'.)"}};
        thing.insert("data", data.object());
    }
    if (parser.isSet("book"))
        thing.insert("book", parser.value("book").toLongLong());
    if (parser.isSet("section")) {
        const auto fits = BookOps::checkSection(graph.library(), parser.value("book").toLongLong(), parser.value("section").toInt());
        if (!fits)
            return {{"error", fits.error().text()}};
        thing.insert("section", parser.value("section").toInt());
        thing.insert("fraction", parser.value("fraction").toDouble());
    }
    return GraphOps::add(graph, thing);
}

QJsonObject GraphByHand::link(const Graph &graph, const QCommandLineParser &parser, const QStringList &words) {
    const Library &library = graph.library();
    const qint64 bookId = parser.value("book").toLongLong();
    std::optional<GraphPlace> learned;
    if (bookId > 0 && library.book(bookId)) {
        if (const auto past = BookOps::checkSection(library, bookId, parser.value("section").toInt()); parser.isSet("section") && !past)
            return {{"error", past.error().text()}};
        learned = parser.isSet("section") ? GraphOps::place(bookId, parser.value("section"), parser.value("fraction"))
                                          : std::optional(GraphPlace{bookId, library.progress(bookId).place});
    }
    return GraphOps::link(graph, words.value(1).toLongLong(), words.value(2).toLongLong(), words.mid(3).join(' '), parser.value("note"),
                          learned);
}
