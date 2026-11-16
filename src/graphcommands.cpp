#include "graphcommands.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include "graphprint.h"
#include "graphsync.h"
#include "graphtools.h"

namespace {

using GraphPrint::print;

GraphCommands::Kept scopes(const Library &library) {
    return {
        GraphOps::scopes(library), [](const QJsonObject &answer) {
            for (const auto &value : answer.value("scopes").toArray()) {
                const QString scope = value.toObject().value("scope").toString();
                print(stdout,
                      QString("%1\t%2").arg(value.toObject().value("nodes").toInt()).arg(scope.isEmpty() ? "(the general graph)" : scope));
            }
        }};
}

GraphCommands::Kept log(const Library &library, qint64 bookId) {
    return {GraphOps::log(library, bookId), [](const QJsonObject &answer) {
                for (const auto &value : answer.value("days").toArray()) {
                    const QJsonObject day = value.toObject();
                    print(stdout, QString("%1\t%2% → %3%\t%4")
                                      .arg(day.value("day").toString())
                                      .arg(qRound(day.value("from").toDouble()))
                                      .arg(qRound(day.value("to").toDouble()))
                                      .arg(day.value("title").toString()));
                }
            }};
}

// Another computer's graph, as `omabook sync` hands it over (graphsync.h).
GraphCommands::Kept take(Library &library, const QString &path) {
    QFile file(path);
    const QJsonDocument bundle = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
    const QJsonObject answer = bundle.isObject() ? QJsonObject{{"changed", GraphSync::take(library, bundle.object())}}
                                                 : QJsonObject{{"error", path + " is not a graph sent by another computer's Omabook."}};
    return {answer, [](const QJsonObject &taken) { print(stdout, QString("%1 new in the graph.").arg(taken.value("changed").toInt())); }};
}

} // namespace

std::optional<GraphCommands::Kept> GraphCommands::housekeeping(Library &library, const QString &sub, const QStringList &words) {
    if (sub == "scopes")
        return scopes(library);
    if (sub == "copy-scope" && words.size() == 3)
        return Kept{GraphOps::copyScope(library, words.at(1), words.at(2)),
                    [to = words.at(2)](const QJsonObject &) { print(stdout, "Copied to " + to + "."); }};
    if (sub == "remove-scope" && words.size() == 2)
        return Kept{GraphOps::removeScope(library, words.at(1)),
                    [scope = words.at(1)](const QJsonObject &) { print(stdout, "Removed " + scope + "."); }};
    if (sub == "log")
        return log(library, words.value(1).toLongLong());
    if (sub == "take" && words.size() == 2)
        return take(library, words.at(1));
    if (sub == "sync")
        return Kept{GraphOps::sync(library), [](const QJsonObject &answer) {
                        print(stdout, QString("%1 books stand in the graph.").arg(answer.value("books").toInt()));
                    }};
    return std::nullopt;
}
