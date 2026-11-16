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
}
