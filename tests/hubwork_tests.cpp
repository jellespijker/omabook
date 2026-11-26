// The hub's background work (#192): a new book's graph is built on the hub,
// whole, by the quick model of the hub's own settings, a job at a time, beside
// the daemon, which goes on answering meanwhile; how far it got is asked by
// request; a job cancelled leaves nothing of itself; a job done is never done
// again; nothing is sent where the hub's settings don't allow hosted quick
// work; and what the hub built reaches a computer by the normal sync.
//
// Everything is in temporary folders: a hub's keeper and its state, a stand-in
// AI server on this computer, and a computer's library synced through a
// session, all in this process (hubworking.h).

#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>
#include <optional>

#include "graph.h"
#include "graphbuilt.h"
#include "hubworking.h"
#include "library.h"

using namespace HubWorking;
using namespace Qt::StringLiterals;

namespace {

// A computer's library.
struct Computer {
    QTemporaryDir dir;
    Library library{dir.filePath(u"library"_s)};

    Computer() { QVERIFY(library.open()); }

    qint64 add(const QString &title) {
        const QString file = dir.filePath(title + u".epub"_s);
        return longBook(file, title) ? library.add(file).id : 0;
    }
};

} // namespace

class HubWorkTests : public QObject {
    Q_OBJECT

private slots:
    void aNewBookGetsItsGraphOnTheHub() {
        Hub hub;
        const QString started = hub.start(u"The Lighthouse"_s);
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QVERIFY(hub.graph().first.isEmpty());
        QVERIFY(hub.worked());
        const QJsonObject status = hub.ask({{"request", "work"}});
        QVERIFY2(status.value("ok"_L1).toBool(), qPrintable(QString::fromUtf8(compact(status))));
        QCOMPARE(outcome(status), u"done"_s);
        const auto [names, built] = hub.graph();
        QVERIFY2(names.contains(u"Mara Venn"_s), qPrintable(names.join(u", "_s)));
        QVERIFY(built);
        // Who built it, and when, is in the library, to travel with the book (#219).
        const auto mark = hub.mark();
        QVERIFY(mark.has_value());
        QCOMPARE(mark->state, GraphBuilt::State::Built);
        QCOMPARE(mark->by, u"homehub"_s);
        QVERIFY(mark->hub);
        QVERIFY(mark->to.has_value());
        // Laid into the library after a moment snapshot, as a bulk change (#208).
        QVERIFY(hub.moments.contains(HubKeeper::Moment::Bulk));
        // Done once: looking again sends nothing more.
        const int sent = chats(hub.server);
        QVERIFY(sent >= 3);
        QVERIFY(hub.worked());
        QCOMPARE(chats(hub.server), sent);
        QCOMPARE(hub.graph().first, names);
    }

    void howFarItGotIsAskedWhileItWorks() {
        Hub hub;
        const QString started = hub.start(u"The Lighthouse"_s);
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QList<QJsonObject> seen;
        std::optional<GraphBuilt::Mark> meanwhile;
        hub.server.answering = [&hub, &seen, &meanwhile](const FakeServer::Request &request) {
            // The daemon answers while the model is asked: the session's status, and a sync, aren't held up.
            if (request.line.startsWith("POST")) {
                seen << hub.ask({{"request", "work"}}) << hub.ask({{"request", "status"}});
                meanwhile = hub.mark();
            }
            return answered(request);
        };
        QVERIFY(hub.worked());
        QVERIFY(seen.size() >= 6);
        const QJsonObject second = seen.at(2).value("running"_L1).toObject();
        QCOMPARE(second.value("title"_L1).toString(), u"The Lighthouse"_s);
        QCOMPARE(second.value("kind"_L1).toString(), u"graph"_s);
        QCOMPARE(second.value("capability"_L1).toString(), u"quick"_s);
        QCOMPARE(second.value("done"_L1).toInt(), 1);
        QVERIFY(second.value("total"_L1).toInt() >= 3);
        QVERIFY(seen.at(3).value("ok"_L1).toBool());
        QVERIFY(!hub.ask({{"request", "work"}}).contains("running"_L1));
        // While it worked, the library said the hub was building it (#219).
        QVERIFY(meanwhile.has_value());
        QCOMPARE(meanwhile->state, GraphBuilt::State::Building);
    }

    void aCancelledJobLeavesNothingOfItself() {
        Hub hub;
        const QString started = hub.start(u"The Lighthouse"_s);
        QVERIFY2(started.isEmpty(), qPrintable(started));
        int asked = 0;
        bool cancelled = false;
        hub.server.answering = [&](const FakeServer::Request &request) {
            if (request.line.startsWith("POST") && ++asked == 2)
                cancelled = hub.ask({{"request", "work-cancel"}}).value("cancelled"_L1).toBool();
            return answered(request);
        };
        QVERIFY(hub.worked());
        QVERIFY(cancelled);
        QCOMPARE(asked, 2);
        QCOMPARE(outcome(hub.ask({{"request", "work"}})), u"cancelled"_s);
        // Two passages were answered, and neither is in the graph, nor marked as built.
        const auto [names, built] = hub.graph();
        QVERIFY2(names.isEmpty(), qPrintable(names.join(u", "_s)));
        QVERIFY(!built);
        QVERIFY(!hub.moments.contains(HubKeeper::Moment::Bulk));
        // Nor is it said to be building it any more (#219).
        QCOMPARE(hub.mark().value_or(GraphBuilt::Mark{}).state, GraphBuilt::State::Stopped);
        // Nor taken up again at once.
        QVERIFY(hub.worked());
        QCOMPARE(asked, 2);
        QVERIFY(!hub.ask({{"request", "work-cancel"}}).value("cancelled"_L1).toBool());
    }

    void nothingIsSentWhereHostedIsNotAllowed() {
        Hub hub;
        const QString started = hub.start(u"The Lighthouse"_s, u"https://ai.example.com/v1"_s);
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QVERIFY(hub.worked());
        const QJsonObject status = hub.ask({{"request", "work"}});
        const QJsonObject unavailable = status.value("unavailable"_L1).toObject();
        QCOMPARE(unavailable.value("capability"_L1).toString(), u"quick"_s);
        QCOMPARE(unavailable.value("why"_L1).toString(), u"hosted-not-allowed"_s);
        QVERIFY(status.value("last"_L1).toArray().isEmpty());
        QVERIFY(hub.graph().first.isEmpty());
    }

    void whatTheHubBuiltReachesAComputerByTheSync() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        const qint64 book = laptop.add(u"The Lighthouse"_s);
        QVERIFY(book > 0);
        const SyncSummary sent = hub.syncWith(laptop.library);
        QVERIFY2(sent.error.isEmpty() && sent.problems.isEmpty(), qPrintable(sent.error + sent.problems.join(u'\n')));
        QVERIFY(hub.worked());
        QCOMPARE(outcome(hub.ask({{"request", "work"}})), u"done"_s);
        QVERIFY(Graph::ofReader(laptop.library, book).ofBook(book).nodes.isEmpty());
        const SyncSummary back = hub.syncWith(laptop.library);
        QVERIFY2(back.error.isEmpty() && back.problems.isEmpty(), qPrintable(back.error + back.problems.join(u'\n')));
        QStringList names;
        for (const GraphNode &node : Graph::ofReader(laptop.library, book).ofBook(book).nodes)
            names << node.name;
        QVERIFY2(names.contains(u"Mara Venn"_s), qPrintable(names.join(u", "_s)));
    }
};

QTEST_GUILESS_MAIN(HubWorkTests)
#include "hubwork_tests.moc"
