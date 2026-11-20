// A book's view in a large library (#99): a book's graph, its spoiler cut
// and the graph palette's list of books read that book's sightings by the
// index on graph_seen by book, not the whole graph, which the palette's list
// did once a book.

#include <QElapsedTimer>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <utility>

#include "bookmeta.h"
#include "changelog.h"
#include "graph.h"
#include "graphmodel.h"
#include "library.h"

namespace {

constexpr int books = 3000;

// The plan SQLite makes of `sql`, its steps in order.
QString plan(const Library &library, const QString &sql, const QVariantList &values) {
    QStringList steps;
    for (QSqlQuery query = ChangeLog::query(library, "EXPLAIN QUERY PLAN " + sql, values); query.next();)
        steps << query.value(3).toString();
    return steps.join(" | ");
}

} // namespace

class BookViewTests : public QObject {
    Q_OBJECT

private slots:
    void aBooksViewReadsOnlyThatBooksSightings() {
        QTemporaryDir dir;
        Library library(dir.filePath("library"));
        QVERIFY(library.open());
        Graph graph(library);
        QList<qint64> ids;
        {
            const Library::Batch batch(library);
            for (int i = 0; i < books; ++i) {
                BookMeta meta;
                meta.title = QString("Book %1").arg(i);
                const Library::Added added = library.addEntry(meta, true);
                QVERIFY2(added.error.isEmpty(), qPrintable(added.error));
                ids << added.id;
                // Two things of its own, early and late, and one it shares
                // with the hundred books around it.
                graph.upsert(QString(), "character", QString("Early %1").arg(i), {}, {}, GraphPlace{added.id, 0, 0.5});
                graph.upsert(QString(), "character", QString("Late %1").arg(i), {}, {}, GraphPlace{added.id, 2, 0.5});
                graph.upsert(QString(), "place", QString("Shared %1").arg(i / 100), {}, {}, GraphPlace{added.id, 1, 0.5});
            }
        }
        // A book's sightings as Graph::metIn asks for them, under every
        // book's view, and the things a book's tenant is given (tenancy):
        // a range of the index by book, not a read of the whole graph.
        const QList<std::pair<QString, QVariantList>> asked{
            {QString("SELECT graph_nodes.id, scope, kind, name, summary, data, COALESCE(graph_nodes.book_id, 0), "
                     "updated, graph_seen.book_id, graph_seen.section, graph_seen.fraction FROM graph_nodes "
                     "JOIN graph_seen ON graph_seen.node_id = graph_nodes.id WHERE graph_seen.book_id = ? AND "
                     "scope = ? ORDER BY graph_seen.section, graph_seen.fraction, graph_nodes.id"),
             {ids.first(), QString("")}},
            {QString("SELECT node_id FROM graph_seen WHERE book_id = ?"), {ids.first()}}};
        for (const auto &[sql, values] : asked) {
            const QString steps = plan(library, sql, values);
            QVERIFY2(steps.startsWith("SEARCH graph_seen USING INDEX graph_seen_book (book_id=?)")
                         || steps.startsWith("SEARCH graph_seen USING COVERING INDEX graph_seen_book (book_id=?)"),
                     qPrintable(steps));
        }
        // One book's view, and its spoiler cut, in a few milliseconds.
        const qint64 book = ids.at(books / 2);
        QElapsedTimer clock;
        clock.start();
        const GraphView whole = graph.ofBook(book);
        const GraphView cut = graph.ofBook(book, GraphPlace{book, 1, 0.9});
        const qint64 took = clock.elapsed();
        QCOMPARE(whole.nodes.size(), 3);
        QCOMPARE(cut.nodes.size(), 2);
        QVERIFY2(took < 50, qPrintable(QString("a book's view and its cut took %1 ms").arg(took)));
        // The palette's list of books: every book's view in turn, which took
        // four seconds here without the index.
        GraphBrowser browser;
        browser.setLibrary(&library);
        clock.restart();
        const QVariantList listed = browser.books();
        const qint64 listing = clock.elapsed();
        QCOMPARE(listed.size(), books);
        QVERIFY2(listing < 1500, qPrintable(QString("the palette listed %1 books in %2 ms").arg(listed.size()).arg(listing)));
    }
};

QTEST_MAIN(BookViewTests)
#include "bookview_tests.moc"
