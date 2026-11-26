// A computer leaves a book's graph to the hub that built it, or is building it
// (#219): who built a book's graph and when travels with the book, so after a
// sync with the hub the computer reading the book sends its model nothing for
// it, and a book the hub has begun isn't begun here too; unless the hub hasn't
// got on with it for a while, or the person asks. Where the graph's count is
// shown, it says it was built on the hub.
//
// The hub is in this process, in temporary folders, its session over the pipe
// (hubworking.h); the laptop is the window's reader and companion without the
// window, its model a stand-in AI server of its own that counts what it is sent.

#include <QDateTime>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "airegistry.h"
#include "assistant.h"
#include "backend.h"
#include "filepicker.h"
#include "graphbuilt.h"
#include "graphjobs.h"
#include "hubworking.h"
#include "reader.h"
#include "settings.h"

using namespace HubWorking;
using namespace Qt::StringLiterals;

namespace {

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// The laptop: its library, its reader and companion, the graph growing as
// chapters are finished, by its own stand-in model.
struct Laptop {
    QTemporaryDir dir;
    FakeServer server;
    NoPicker picker;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;
    std::optional<Assistant> assistant;

    Laptop() {
        qputenv("XDG_CACHE_HOME", dir.filePath(u"cache"_s).toUtf8());
        server.answering = answered;
        QVERIFY(settle(dir.filePath(u"config.json"_s), server.url()));
        settings.emplace(dir.filePath(u"config.json"_s));
        settings->setLibraryPath(dir.filePath(u"Library"_s));
        settings->setGraph(u"auto"_s);
        backend.emplace(
            &*settings, &picker, nullptr, AiRegistry::answerer(*settings), [](QObject *) { return QList<MetadataSource *>(); },
            dir.filePath(u"plugins"_s), u"-"_s);
        reader.emplace(&*backend, QString());
        assistant.emplace(&*reader, AiRegistry::answerer(*settings));
        assistant->setGraphMode([this] { return settings->graph(); });
        assistant->setGraphWait(0);
    }

    Library &library() { return *backend->library(); }

    qint64 add(const QString &title) {
        const QString file = dir.filePath(title + u".epub"_s);
        const qint64 id = longBook(file, title) ? library().add(file).id : 0;
        backend->reload();
        return id;
    }

    // The book read from its start to its end, a chapter after another, and
    // waited for until the graph isn't growing. What it asked its model.
    [[nodiscard]] int read(qint64 book) {
        const int before = chats(server);
        reader->openBook(book);
        if (!QTest::qWaitFor([this] { return reader->isOpen(); }))
            return -1;
        for (int section = 1; section < reader->count(); ++section) {
            reader->setSection(section);
            QTest::qWait(50);
            if (!QTest::qWaitFor([this] { return !assistant->graphBusy(); }, 20000))
                return -1;
        }
        return chats(server) - before;
    }
};

// The laptop's book, sent to the hub. The hub's work is then up to the test.
[[nodiscard]] QString shared(Hub &hub, Laptop &laptop, qint64 &book) {
    if (const QString started = hub.start(); !started.isEmpty())
        return started;
    book = laptop.add(u"The Lighthouse"_s);
    if (book == 0)
        return u"no book added"_s;
    const SyncSummary sent = hub.syncWith(laptop.library());
    return sent.error + sent.problems.join(u'\n');
}

} // namespace

class HubGraphsTests : public QObject {
    Q_OBJECT

private slots:
    void aBookTheHubBuiltIsNotBuiltAgain() {
        Hub hub;
        Laptop laptop;
        qint64 book = 0;
        const QString ready = shared(hub, laptop, book);
        QVERIFY2(ready.isEmpty(), qPrintable(ready));
        QVERIFY(hub.worked());
        QCOMPARE(outcome(hub.ask({{"request", "work"}})), u"done"_s);
        const SyncSummary back = hub.syncWith(laptop.library());
        QVERIFY2(back.error.isEmpty() && back.problems.isEmpty(), qPrintable(back.error + back.problems.join(u'\n')));
        // Who built it, and when, came with the book.
        const auto mark = GraphBuilt::of(laptop.library(), book);
        QVERIFY(mark.has_value());
        QCOMPARE(mark->state, GraphBuilt::State::Built);
        QCOMPARE(mark->by, u"homehub"_s);
        QVERIFY(mark->hub);
        QVERIFY(qAbs(mark->at.secsTo(QDateTime::currentDateTime())) < 60);
        // So did how far: to the end, as the hub built it.
        QVERIFY(GraphBuild::builtTo(laptop.library(), book).has_value());
        // Read from start to end, the laptop asks its model nothing for the graph.
        QCOMPARE(laptop.read(book), 0);
        // Where the graph's count is shown, it says where it was built.
        QCOMPARE(laptop.backend->graphJobs()->builtElsewhere(book), u"built on homehub"_s);
        // A book the hub never had is built here as it is read.
        const qint64 other = laptop.add(u"The Harbour"_s);
        QVERIFY(laptop.read(other) > 0);
        QVERIFY(laptop.backend->graphJobs()->builtElsewhere(other).isEmpty());
    }

    void aBookTheHubIsBuildingIsNotBegunHere() {
        Hub hub;
        Laptop laptop;
        qint64 book = 0;
        const QString ready = shared(hub, laptop, book);
        QVERIFY2(ready.isEmpty(), qPrintable(ready));
        // The hub begins, and before it has had an answer the laptop syncs.
        QVERIFY(hub.work->look());
        QVERIFY(hub.work->busy());
        const SyncSummary back = hub.syncWith(laptop.library());
        QVERIFY2(back.error.isEmpty() && back.problems.isEmpty(), qPrintable(back.error + back.problems.join(u'\n')));
        const auto mark = GraphBuilt::of(laptop.library(), book);
        QVERIFY(mark.has_value());
        QCOMPARE(mark->state, GraphBuilt::State::Building);
        QCOMPARE(laptop.backend->graphJobs()->builtElsewhere(book), u"being built on homehub"_s);
        QVERIFY(!GraphBuild::builtTo(laptop.library(), book).has_value());
        // Read on, the laptop leaves it to the hub.
        QCOMPARE(laptop.read(book), 0);
        // Asked by hand, it builds the graph itself.
        laptop.assistant->growGraph();
        QVERIFY(QTest::qWaitFor([&] { return chats(laptop.server) > 0 && !laptop.assistant->graphBusy(); }, 20000));
        QVERIFY(GraphBuild::builtTo(laptop.library(), book).has_value());
        QVERIFY(QTest::qWaitFor([&] { return !hub.work->busy(); }, 30000));
    }

    void aHubThatDoesNotGetOnWithItIsWaitedForNoLonger() {
        const QDateTime now = QDateTime::currentDateTime();
        const GraphBuilt::Mark building{.state = GraphBuilt::State::Building, .by = u"homehub"_s, .hub = true, .at = now, .to = {}};
        QVERIFY(GraphBuilt::leftToHub(building, now));
        QVERIFY(GraphBuilt::leftToHub(building, now.addSecs(60 * 60)));
        QVERIFY(!GraphBuilt::leftToHub(building, now.addSecs(std::chrono::seconds(GraphBuilt::patience).count() + 1)));
        // One the hub stopped, or a computer's own, is no reason to wait.
        GraphBuilt::Mark stopped = building;
        stopped.state = GraphBuilt::State::Stopped;
        QVERIFY(!GraphBuilt::leftToHub(stopped, now));
        GraphBuilt::Mark own = building;
        own.hub = false;
        QVERIFY(!GraphBuilt::leftToHub(own, now));
        QVERIFY(!GraphBuilt::leftToHub(std::nullopt, now));
        // A mark is read as it was written; anything else is none.
        const auto again = GraphBuilt::Mark::read(building.written());
        QVERIFY(again.has_value());
        QCOMPARE(again->by, building.by);
        QCOMPARE(again->state, building.state);
        QCOMPARE(again->at.toSecsSinceEpoch(), now.toSecsSinceEpoch());
        QVERIFY(!GraphBuilt::Mark::read(u"not json"_s).has_value());
    }
};

QTEST_MAIN(HubGraphsTests)
#include "hubgraphs_tests.moc"
