#pragma once

// A hub that builds its books' graphs (#192), in this process and in temporary
// folders, for the tests of its work (hubwork_tests) and of what a computer
// makes of it (hubgraphs_tests, #219): its keeper of one library ("home"), its
// work asking a stand-in AI server on this computer, its sync, and one computer
// linked (the laptop) through the session's pipe (HubSession).

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include "crypto.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbuilder.h"
#include "graphbuilt.h"
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

namespace HubWorking {

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

[[nodiscard]] inline QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// What the stand-in model finds in every passage it is sent.
inline const char *const found =
    R"({"nodes": [{"kind": "character", "name": "Mara Venn", "summary": "Keeps the lighthouse.", "section": 0}],
 "edges": []})";

// The stand-in AI server's answer: the graph's JSON, streamed when asked to be, and its one model.
[[nodiscard]] inline FakeServer::Reply answered(const FakeServer::Request &request) {
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

// Settings whose answers and quick work go to the server at `url`, never hosted.
[[nodiscard]] inline bool settle(const QString &file, const QString &url) {
    const QJsonObject own{{"provider", "openai"}, {"hosted", false}};
    return Fixtures::write(file, compact({{"ai_provider", "openai"},
                                          {"ai_endpoint", QJsonObject{{"preset", "custom"}, {"base_url", url}}},
                                          {"capabilities", QJsonObject{{"answers", own}, {"quick", own}}}}));
}

// A book long enough for several requests: three chapters, each one of its own.
[[nodiscard]] inline bool longBook(const QString &file, const QString &title) {
    QList<Fixtures::Chapter> chapters;
    const QString paragraph = u"<p>Mara Venn climbed the stairs of the lighthouse again, as she did every night, and lit the lamp.</p>"_s;
    for (const QString &name : {u"One"_s, u"Two"_s, u"Three"_s})
        chapters << Fixtures::Chapter{.title = name, .body = paragraph.repeated(180)};
    return Fixtures::chapteredEpub(file, title, chapters);
}

// The requests a server was sent to answer (not the list of its models).
[[nodiscard]] inline int chats(const FakeServer &server) {
    return static_cast<int>(std::ranges::count_if(server.received, [](const auto &request) { return request.line.startsWith("POST"); }));
}

// The hub, called "homehub".
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
        work = std::make_unique<HubWork>(keeper, dir.filePath(u"state"_s), dir.filePath(u"state/config.json"_s), u"homehub"_s);
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

    // Who built the graph of the hub's book, and when, as its library says.
    [[nodiscard]] std::optional<GraphBuilt::Mark> mark() {
        std::optional<GraphBuilt::Mark> said;
        std::ignore = keeper.whole([&](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            const Library &library = *libraries.front();
            said = GraphBuilt::of(library, library.books().value(0).id);
            return {};
        });
        return said;
    }

    [[nodiscard]] SyncSummary syncWith(Library &library) {
        HubSession session(computers, laptop, [this](const QByteArray &request) { return requests.answerLine(request, keeper).trimmed(); });
        std::ignore = session.answer(compact(HubSession::hello()));
        return HubRemote::sync(library, u"hub"_s, [&session](const QByteArray &line) { return session.answer(line); }, {}, {});
    }
};

[[nodiscard]] inline QString outcome(const QJsonObject &status) {
    return status.value("last"_L1).toArray().first()["outcome"_L1].toString();
}

} // namespace HubWorking
