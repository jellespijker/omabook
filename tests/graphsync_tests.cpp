// The knowledge graph between two computers: what a sync carries of it,
// merged on each side, never ahead of the reader (graphsync.h).

#include <QDir>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "filepicker.h"
#include "fixtures.h"
#include "graph.h"
#include "graphsync.h"
#include "library.h"
#include "settings.h"
#include "sync.h"

namespace {

bool has(const GraphView &view, const QString &name) {
    return std::ranges::any_of(view.nodes, [&](const GraphNode &node) { return node.name == name; });
}

GraphNode named(const Graph &graph, const QString &kind, const QString &name) { return *graph.node(graph.named({}, kind, name)); }

struct NullPicker final : FilePicker {
    void pick(Purpose, const QString &) override {}
};

// Two libraries, "Here" and "There", of the same book, and the stand-in ssh
// that makes There another computer. There holds another book first, so the
// same book has another number on each side, as it does on two computers.
struct TwoComputers {
    QTemporaryDir dir;
    QString ssh;
    Library here, there;
    qint64 bookHere = 0, bookThere = 0;

    TwoComputers() : ssh(Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)), here(dir.filePath("Here")), there(dir.filePath("There")) {}

    bool open() {
        if (!here.open() || !there.open() || !Fixtures::epub(dir.filePath("book.epub"), "The Ash and the Mists")
            || !Fixtures::epub(dir.filePath("other.epub"), "Only There"))
            return false;
        there.add(dir.filePath("other.epub"));
        bookHere = here.add(dir.filePath("book.epub")).id;
        bookThere = there.add(dir.filePath("book.epub")).id;
        return bookHere > 0 && bookThere > 0 && bookHere != bookThere;
    }
    SyncSummary sync() { return syncLibrary(here, Remote("laptop:" + dir.filePath("There"), {ssh})); }
};

} // namespace

class GraphSyncTests : public QObject {
    Q_OBJECT

private slots:
    void whatWasReadHereArrivesThereAtItsMoments() {
        TwoComputers two;
        QVERIFY(two.open());
        {
            Graph graph(two.here);
            const qint64 kelsier =
                graph.upsert({}, "character", "Kelsier", "A thief with a plan.", {}, GraphPlace{two.bookHere, 1, 0.2}, "Kelsier smiled");
            graph.learn(kelsier, "alias", "the Survivor", GraphMoment{{two.bookHere, 4, 0.5}, "the Survivor of Hathsin"});
            const qint64 vin = graph.upsert({}, "character", "Vin", "A street urchin.", {}, GraphPlace{two.bookHere, 1, 0.6});
            graph.link(kelsier, vin, "mentor of", "teaches her Allomancy", GraphPlace{two.bookHere, 2, 0.1}, "he showed her");
            graph.logReading(two.bookHere, 0, 40, "2026-11-15");
        }
        const SyncSummary summary = two.sync();
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QVERIFY(summary.graphTaken == 0 && summary.graphSent > 0);

        Graph there(two.there);
        const GraphNode kelsier = named(there, "character", "Kelsier");
        QCOMPARE(kelsier.summary, QString("A thief with a plan."));
        QCOMPARE(there.firstMet(kelsier.id, two.bookThere)->place.section, 1);
        QCOMPARE(there.firstMet(kelsier.id, two.bookThere)->anchor, QString("Kelsier smiled"));
        // The other name arrives with its moment, so a reader before it doesn't know it.
        QVERIFY(kelsier.aliases().contains("the Survivor"));
        const GraphView early = there.ofBook(two.bookThere, GraphPlace{two.bookThere, 2, 0});
        QVERIFY(has(early, "Kelsier"));
        for (const GraphNode &node : early.nodes)
            QVERIFY2(!node.aliases().contains("the Survivor"), "a name revealed later is known too early");
        QVERIFY(!there.ofBook(two.bookThere).edges.isEmpty());
        const QList<ReadingDay> log = there.readingLog(two.bookThere);
        QCOMPARE(log.size(), 1);
        QCOMPARE(log.first().to, 40.0);
        // Of the book only There has, nothing is said.
        QVERIFY(there.named({}, "character", "Vin") > 0);
    }

    void bothSidesLearnAndTheSecondSyncChangesNothing() {
        TwoComputers two;
        QVERIFY(two.open());
        {
            Graph here(two.here);
            here.upsert({}, "character", "Vin", "A street urchin.", {}, GraphPlace{two.bookHere, 1, 0.5});
            Graph there(two.there);
            there.upsert({}, "place", "Luthadel", "The capital.", {}, GraphPlace{two.bookThere, 1, 0.1});
            // The same thing met there earlier: the earlier place stands on both.
            there.upsert({}, "character", "Vin", {}, {}, GraphPlace{two.bookThere, 0, 0.9});
            there.logReading(two.bookThere, 10, 60, "2026-11-16");
        }
        const SyncSummary first = two.sync();
        QVERIFY2(first.problems.isEmpty(), qPrintable(first.problems.join('\n')));
        QVERIFY(first.graphTaken > 0 && first.graphSent > 0);
        for (Library *library : {&two.here, &two.there}) {
            const Graph graph(*library);
            const qint64 book = library == &two.here ? two.bookHere : two.bookThere;
            QVERIFY(graph.named({}, "place", "Luthadel") > 0);
            const GraphNode vin = named(graph, "character", "Vin");
            QCOMPARE(vin.summary, QString("A street urchin."));
            QCOMPARE(graph.firstMet(vin.id, book)->place.section, 0);
            QCOMPARE(graph.readingLog(book).size(), 1);
        }
        const SyncSummary again = two.sync();
        QVERIFY2(again.problems.isEmpty(), qPrintable(again.problems.join('\n')));
        QCOMPARE(again.graphTaken, 0);
        QCOMPARE(again.graphSent, 0);
    }

    void theNewerSummaryWins() {
        TwoComputers two;
        QVERIFY(two.open());
        Graph here(two.here);
        const qint64 vin = here.upsert({}, "character", "Vin", "A street urchin.", {}, GraphPlace{two.bookHere, 1, 0.5});
        QVERIFY(two.sync().problems.isEmpty());
        // Read on There: a later summary, at a later place.
        {
            Graph there(two.there);
            const qint64 hers = there.named({}, "character", "Vin");
            QTest::qWait(1100); // a second later, as `updated` counts
            there.learn(hers, "summary", "A Mistborn, trained by Kelsier.", GraphMoment{{two.bookThere, 6, 0.3}, "she burned pewter"});
        }
        QVERIFY(two.sync().problems.isEmpty());
        QCOMPARE(here.node(vin)->summary, QString("A Mistborn, trained by Kelsier."));
        // Before that place, the earlier summary is still all a reader knows.
        const GraphView early = here.ofBook(two.bookHere, GraphPlace{two.bookHere, 2, 0});
        for (const GraphNode &node : early.nodes)
            if (node.name == "Vin")
                QCOMPARE(node.summary, QString("A street urchin."));
    }

    void aThingKnownUnderAnotherNameOnEachSideSettlesInOneSync() {
        TwoComputers two;
        QVERIFY(two.open());
        {
            Graph here(two.here);
            here.upsert({}, "character", "Vin", "A street urchin.", {}, GraphPlace{two.bookHere, 1, 0.2});
            const qint64 valette =
                here.upsert({}, "character", "Valette Renoux", "A noblewoman at court.", {}, GraphPlace{two.bookHere, 2, 0.4});
            const qint64 luthadel = here.upsert({}, "place", "Luthadel", {}, {}, GraphPlace{two.bookHere, 1, 0.1});
            here.link(valette, luthadel, "lives in", "at Keep Renoux", GraphPlace{two.bookHere, 2, 0.5});

            Graph there(two.there);
            const qint64 vin = there.upsert({}, "character", "Vin", {}, {}, GraphPlace{two.bookThere, 1, 0.1});
            there.learn(vin, "alias", "Valette Renoux", GraphMoment{{two.bookThere, 2, 0.1}, "Lady Valette Renoux"});
        }
        const SyncSummary first = two.sync();
        QVERIFY2(first.problems.isEmpty(), qPrintable(first.problems.join('\n')));
        QVERIFY(first.graphTaken > 0 && first.graphSent > 0);
        for (Library *library : {&two.here, &two.there}) {
            const Graph graph(*library);
            const qint64 book = library == &two.here ? two.bookHere : two.bookThere;
            const GraphNode vin = named(graph, "character", "Vin");
            QCOMPARE(graph.named({}, "character", "Valette Renoux"), vin.id);
            QVERIFY(vin.aliases().contains("Valette Renoux"));
            QCOMPARE(graph.firstMet(vin.id, book)->place.fraction, 0.1);
            QVERIFY(!graph.around(vin.id, 1).edges.isEmpty());
        }
        const SyncSummary again = two.sync();
        QVERIFY2(again.problems.isEmpty(), qPrintable(again.problems.join('\n')));
        QCOMPARE(again.graphTaken, 0);
        QCOMPARE(again.graphSent, 0);
    }

    void aThingKnownByAnotherNameLearnsBothNamesInOneSync() {
        TwoComputers two;
        QVERIFY(two.open());
        {
            Graph here(two.here);
            here.upsert({}, "character", "Valette Renoux", "A noblewoman at court.", {}, GraphPlace{two.bookHere, 2, 0.4});
            Graph there(two.there);
            const qint64 vin = there.upsert({}, "character", "Vin", {}, {}, GraphPlace{two.bookThere, 1, 0.2});
            there.learn(vin, "alias", "Valette Renoux", GraphMoment{{two.bookThere, 2, 0.1}, "Valette"});
        }
        const SyncSummary first = two.sync();
        QVERIFY2(first.problems.isEmpty(), qPrintable(first.problems.join('\n')));
        QVERIFY(first.graphTaken > 0 && first.graphSent > 0);
        for (Library *library : {&two.here, &two.there}) {
            const Graph graph(*library);
            const qint64 byVin = graph.named({}, "character", "Vin");
            const qint64 byValette = graph.named({}, "character", "Valette Renoux");
            QVERIFY(byVin > 0);
            QCOMPARE(byValette, byVin);
        }
        const SyncSummary again = two.sync();
        QVERIFY2(again.problems.isEmpty(), qPrintable(again.problems.join('\n')));
        QCOMPARE(again.graphTaken, 0);
        QCOMPARE(again.graphSent, 0);
    }

    void aBundleNamesBooksByTheirFiles() {
        TwoComputers two;
        QVERIFY(two.open());
        Graph(two.here).upsert({}, "character", "Kelsier", {}, {}, GraphPlace{two.bookHere, 1, 0.2});
        const QJsonObject bundle = GraphSync::bundle(two.here);
        const QJsonObject book = bundle.value("books").toObject().value(QString::number(two.bookHere)).toObject();
        QCOMPARE(book.value("hashes").toArray().first().toString(), two.here.formatFiles(two.bookHere).first().hash);
        // A bundle of another shape is left alone.
        QCOMPARE(GraphSync::take(two.there, QJsonObject{{"version", 2}, {"nodes", bundle.value("nodes")}}), 0);
        QCOMPARE(Graph(two.there).named({}, "character", "Kelsier"), 0);
    }

    void theSummaryStatusAndLogSayWhatTheGraphDid() {
        TwoComputers two;
        QVERIFY(two.open());
        Graph(two.here).upsert({}, "character", "Kelsier", {}, {}, GraphPlace{two.bookHere, 1, 0.2});
        Graph(two.there).upsert({}, "place", "Luthadel", {}, {}, GraphPlace{two.bookThere, 1, 0.1});

        // 1. The log line at the end of syncLibrary.
        static QStringList syncLogs;
        syncLogs.clear();
        static QtMessageHandler previous = nullptr;
        previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &message) {
            if (qstrcmp(context.category, "omabook.sync") == 0)
                syncLogs << message;
            if (previous)
                previous(type, context, message);
        });
        const auto restoreLog = qScopeGuard([] { qInstallMessageHandler(previous); });
        const SyncSummary logged = two.sync();
        QVERIFY2(logged.problems.isEmpty(), qPrintable(logged.problems.join('\n')));
        QVERIFY(std::ranges::any_of(syncLogs, [](const QString &line) {
            return line.contains("graph taken") && line.contains("graph sent");
        }));

        // 2. The plain summary of `omabook sync` on the command line.
        Graph(two.here).upsert({}, "character", "Vin", {}, {}, GraphPlace{two.bookHere, 1, 0.3});
        Graph(two.there).upsert({}, "place", "Keep Venture", {}, {}, GraphPlace{two.bookThere, 1, 0.4});
        QVERIFY(QDir(two.dir.path()).mkpath("bin"));
        QFile::link(two.ssh, two.dir.filePath("bin/ssh"));
        QProcess cli;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("HOME", two.dir.filePath("cli-home"));
        env.insert("XDG_CONFIG_HOME", two.dir.filePath("cli-home/.config"));
        env.insert("XDG_DATA_HOME", two.dir.filePath("cli-home/.local/share"));
        env.insert("XDG_STATE_HOME", two.dir.filePath("cli-home/.local/state"));
        env.insert("PATH", two.dir.filePath("bin") + ":" + env.value("PATH"));
        cli.setProcessEnvironment(env);
        cli.start(OMABOOK_BINARY, {"sync", "laptop:" + two.dir.filePath("There"), "--library", two.here.root()});
        QVERIFY(cli.waitForFinished(30000));
        QCOMPARE(cli.exitCode(), 0);
        const QString stdoutText = QString::fromUtf8(cli.readAllStandardOutput());
        QVERIFY2(stdoutText.contains("2 graph changes taken") && stdoutText.contains("2 graph changes sent"),
                 qPrintable(stdoutText));

        // 3. The window's sync status in Backend::sync().
        Graph(two.here).upsert({}, "character", "Elend", {}, {}, GraphPlace{two.bookHere, 1, 0.5});
        Graph(two.there).upsert({}, "place", "Urteau", {}, {}, GraphPlace{two.bookThere, 1, 0.6});
        Settings settings(two.dir.filePath("window.json"));
        settings.setLibraryPath(two.here.root());
        settings.setSyncTarget("laptop:" + two.dir.filePath("There"));
        NullPicker picker;
        Backend backend(&settings, &picker, nullptr, {}, [](QObject *) { return QList<MetadataSource *>(); },
                        two.dir.filePath("plugins"), "-");
        backend.setSsh({two.ssh});
        QSignalSpy messaged(&backend, &Backend::messageChanged);
        backend.sync();
        QTRY_VERIFY_WITH_TIMEOUT(!messaged.isEmpty(), 30000);
        const QString statusMessage = backend.message();
        QVERIFY2(statusMessage.contains("graph change") && statusMessage.contains("taken") && statusMessage.contains("sent"),
                 qPrintable(statusMessage));
    }
};

QTEST_MAIN(GraphSyncTests)
#include "graphsync_tests.moc"
