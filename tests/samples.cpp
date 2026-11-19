#include "samples.h"

#include <QFile>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QtTest>
#include <algorithm>

#include "graph.h"
#include "library.h"
#include "spaces.h"

namespace Samples {

void loadSqlDump(const QString &dumpPath, const QString &dbPath, int &formatOut) {
    QFile file(dumpPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QString connection = "dump-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(dbPath);
        QVERIFY(database.open());
        for (const QString &statement : QString::fromUtf8(file.readAll()).split(";\n", Qt::SkipEmptyParts)) {
            const QString sql = statement.trimmed();
            if (sql.isEmpty() || sql == "COMMIT" || sql.startsWith("BEGIN"))
                continue;
            QSqlQuery query(database);
            QVERIFY2(query.exec(sql), qPrintable(dumpPath + ": " + query.lastError().text() + "\n" + sql.left(120)));
        }
        QSqlQuery version(database);
        QVERIFY(version.exec("PRAGMA user_version") && version.next());
        formatOut = version.value(0).toInt();
        version.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}

QJsonObject librarySnapshot(const Library &library) {
    QJsonArray books;
    for (const Book &book : library.books()) {
        const Progress prog = library.progress(book.id);
        books << QJsonObject{{"gid", Spaces::bookId(library, book.id)},
                             {"title", book.meta.title},
                             {"authors", book.meta.authorText()},
                             {"series", book.meta.series},
                             {"series_index", book.meta.seriesIndex},
                             {"publisher", book.meta.publisher},
                             {"isbn", book.meta.isbn},
                             {"tags", QJsonArray::fromStringList(book.meta.tags)},
                             {"shelf", book.shelf},
                             {"rating", book.rating},
                             {"finished", book.finished},
                             {"paper", book.paper},
                             {"details_at", book.detailsAt},
                             {"reading_at", book.readingAt},
                             {"section", prog.section},
                             {"fraction", prog.fraction},
                             {"percent", prog.percent},
                             {"progress_updated", prog.updated}};
    }
    QList<Collection> sortedCols = library.collections(true);
    std::ranges::sort(sortedCols, {}, &Collection::path);
    QJsonArray collections;
    for (const Collection &col : sortedCols) {
        QStringList included;
        for (const qint64 bid : col.included)
            included << Spaces::bookId(library, bid);
        included.sort();
        collections << QJsonObject{{"gid", Spaces::collectionId(library, col.id)},
                                   {"path", col.path},
                                   {"rule", col.rule},
                                   {"removed", col.removed},
                                   {"updated", col.updated},
                                   {"included", QJsonArray::fromStringList(included)}};
    }
    QJsonArray removals;
    for (const Library::Removal &rem : library.removals())
        removals << QJsonObject{{"key", rem.key}, {"at", rem.at}};

    const Graph graph(library);
    QList<QJsonObject> nodeList;
    for (const QString &scope : graph.scopes()) {
        const GraphView view = graph.whole(scope);
        for (const GraphNode &node : view.nodes) {
            QStringList aliases = node.aliases();
            aliases.sort();
            nodeList << QJsonObject{{"gid", Spaces::nodeId(library, node.id)},
                                    {"scope", node.scope},
                                    {"kind", node.kind},
                                    {"name", node.name},
                                    {"summary", node.summary},
                                    {"aliases", QJsonArray::fromStringList(aliases)},
                                    {"facts", int(graph.facts(node.id).size())},
                                    {"seen", int(graph.seen(node.id).size())}};
        }
    }
    std::ranges::sort(nodeList, {}, [](const QJsonObject &obj) { return obj.value("gid").toString(); });
    QJsonArray nodes;
    for (const QJsonObject &obj : nodeList)
        nodes << obj;
    QJsonArray days;
    for (const ReadingDay &day : graph.readingLog())
        days << QJsonObject{{"book", Spaces::bookId(library, day.bookId)}, {"day", day.day}, {"from", day.from}, {"to", day.to}};

    return {{"books", books}, {"collections", collections}, {"removals", removals}, {"nodes", nodes}, {"days", days}};
}

} // namespace Samples
