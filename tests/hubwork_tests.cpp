// The hub's background work (#192): a new book's graph is built on the hub,
// whole, by the quick model of the hub's own settings, a job at a time, beside
// the daemon, which goes on answering meanwhile; how far it got is asked by
// request; a job cancelled leaves nothing of itself; a job done is never done
// again; nothing is sent where the hub's settings don't allow hosted quick
// work; and what the hub built reaches a computer by the normal sync.
//
// Everything is in temporary folders: a hub's keeper and its state, a stand-in
// AI server on this computer, and a computer's library synced through a
// session, all in this process.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>

#include "crypto.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbuilder.h"
#include "hubcomputers.h"
#include "hubkeeper.h"
#include "hubremote.h"
#include "hubrequests.h"
#include "hubsession.h"
#include "hubsync.h"
#include "hubwork.h"
#include "library.h"
#include "person.h"
#include "sshkey.h"
#include "sync.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// What the stand-in model finds in every passage it is sent.
const char *const found = R"({"nodes": [{"kind": "character", "name": "Mara Venn", "summary": "Keeps the lighthouse.", "section": 0}],
 "edges": []})";

// The stand-in AI server's answer: the graph's JSON, streamed when asked to be, and its one model.
[[nodiscard]] FakeServer::Reply answered(const FakeServer::Request &request) {
    if (request.line.startsWith("GET"))
        return {.status = "200 OK", .body = R"({"data": [{"id": "graph-model"}]})", .type = "application/json"};
    const QJsonObject delta{{"content", QString::fromUtf8(found)}};
    if (request.json().value("stream").toBool())
        return {.status = "200 OK",
                .body = "data: " + compact({{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", delta}}}}})
                        + "\n\ndata: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n",
                .type = "text/event-stream"};
    const QJsonObject message{{"role", "assistant"}, {"content", QString::fromUtf8(found)}};
    return {.status = "200 OK", .body = compact({{"choices", QJsonArray{QJsonObject{{"message", message}}}}}), .type = "application/json"};
}

// The hub's settings: its quick work by the server at `url`, never hosted.
[[nodiscard]] bool settle(const QString &file, const QString &url) {
    const QJsonObject own{{"provider", "openai"}, {"hosted", false}};
    return Fixtures::write(file, compact({{"ai_provider", "openai"},
                                          {"ai_endpoint", QJsonObject{{"preset", "custom"}, {"base_url", url}}},
                                          {"capabilities", QJsonObject{{"answers", own}, {"quick", own}}}}));
}

// A book long enough for several requests: three chapters, each one of its own.
[[nodiscard]] bool longBook(const QString &file, const QString &title) {
    QList<Fixtures::Chapter> chapters;
    const QString paragraph = u"<p>Mara Venn climbed the stairs of the lighthouse again, as she did every night, and lit the lamp.</p>"_s;
    for (const QString &name : {u"One"_s, u"Two"_s, u"Three"_s})
        chapters << Fixtures::Chapter{.title = name, .body = paragraph.repeated(180)};
    return Fixtures::chapteredEpub(file, title, chapters);
}

[[nodiscard]] int chats(const FakeServer &server) {
    return static_cast<int>(std::ranges::count_if(server.received, [](const auto &request) { return request.line.startsWith("POST"); }));
}

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

// A hub of one library ("home"), its work asking the stand-in server, its sync, and one computer linked: the laptop.
struct Hub {
    QTemporaryDir dir;
    FakeServer server;
    HubKeeper keeper{dir.filePath(u"libraries"_s)};
    HubRequests requests = HubRequests::usual();
    HubSync sync{dir.filePath(u"scratch"_s)};
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    std::unique_ptr<HubWork> work;
    QString laptop;
    QList<HubKeeper::Moment> moments;

    // With a book called `title` in its library already, unless empty.
    [[nodiscard]] QString start(const QString &title = {}, const QString &url = {}) {
        server.answering = answered;
        QDir().mkpath(dir.filePath(u"scratch"_s));
        QDir().mkpath(dir.filePath(u"state"_s));
        if (!settle(dir.filePath(u"state/config.json"_s), url.isEmpty() ? server.url() : url))
            return u"no settings written"_s;
        {
            Library made(dir.filePath(u"libraries/home"_s));
            if (!made.open())
                return u"no library made"_s;
            const QString file = dir.filePath(u"book.epub"_s);
            if (!title.isEmpty() && (!longBook(file, title) || made.add(file).id == 0))
                return u"no book added"_s;
        }
        keeper.before([this](Library &, HubKeeper::Moment moment) -> std::expected<void, Failure> {
            moments << moment;
            return {};
        });
        if (const auto opened = keeper.open(); !opened)
            return whyNot(opened);
        work = std::make_unique<HubWork>(keeper, dir.filePath(u"state"_s), dir.filePath(u"state/config.json"_s));
        work->addTo(requests);
        sync.addTo(requests);
        return link();
    }

    [[nodiscard]] QString link() {
        const auto person = Person::generate();
        if (!person)
            return person.error();
        const auto key = SshKey::generate(dir.filePath(u"laptop"_s), u"laptop"_s);
        if (!key)
            return whyNot(key);
        const QString line = SshKey::line(*key);
        const Person::Signature signature = person->sign(Crypto::bytes(HubComputers::linkMessage(u"laptop"_s, line)));
        const auto linked = computers.link({.name = u"laptop"_s,
                                            .key = line,
                                            .identity = person->id(),
                                            .signature = QString::fromLatin1(Crypto::toByteArray(signature).toBase64(
                                                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))});
        if (!linked)
            return whyNot(linked);
        laptop = linked->fingerprint;
        return {};
    }

    // What a request is answered, as the daemon would.
    [[nodiscard]] QJsonObject ask(const QJsonObject &request) { return requests.answer(request, keeper); }

    // The hub's work looked at, and waited for until nothing is under way.
    [[nodiscard]] bool worked() {
        if (!work->look())
            return false;
        return QTest::qWaitFor([this] { return !work->busy(); }, 30000);
    }

    // The names of what the graph of the hub's book holds, and how far it is built.
    [[nodiscard]] std::pair<QStringList, bool> graph() {
        QStringList names;
        bool built = false;
        std::ignore = keeper.whole([&](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            const Library &library = *libraries.front();
            const qint64 book = library.books().value(0).id;
            for (const GraphNode &node : Graph::ofReader(library, book).ofBook(book).nodes)
                names << node.name;
            built = GraphBuild::builtTo(library, book).has_value();
            return {};
        });
        return {names, built};
    }

    [[nodiscard]] SyncSummary syncWith(Library &library) {
        HubSession session(computers, laptop, [this](const QByteArray &request) { return requests.answerLine(request, keeper).trimmed(); });
        std::ignore = session.answer(compact(HubSession::hello()));
        return HubRemote::sync(library, u"hub"_s, [&session](const QByteArray &line) { return session.answer(line); }, {}, {});
    }
};

[[nodiscard]] QString outcome(const QJsonObject &status) { return status.value("last"_L1).toArray().first()["outcome"_L1].toString(); }

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
        hub.server.answering = [&hub, &seen](const FakeServer::Request &request) {
            // The daemon answers while the model is asked: the session's status, and a sync, aren't held up.
            if (request.line.startsWith("POST"))
                seen << hub.ask({{"request", "work"}}) << hub.ask({{"request", "status"}});
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
