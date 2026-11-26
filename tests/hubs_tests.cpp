// More than one hub (#196, ADR 0011): hubs are just more peers. Two computers
// that sync with three hubs, in whatever order, end with the same library on
// all five, and once they have, a round of syncs moves nothing and writes
// nothing to any change log: the hubs don't fight. Automatic sync takes what
// one hub brought to the others, a little later, and never back to the one
// it came from; a hub out of reach doesn't hold up the others, and each hub
// that was out of reach is tried as the network comes back. A book's graph is
// built once, by the hub that claimed it: another hub that learns of the
// claim by a sync leaves the book alone, and a hub whose claim was overtaken
// keeps nothing of its work.
//
// Everything is in temporary folders: the hubs' keepers, their computers' keys
// and the session's pipe, two computers' libraries, and a stand-in AI server
// for each hub's work, all in this process.

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>

#include "crypto.h"
#include "fakeaiserver.h"
#include "fixtures.h"
#include "graph.h"
#include "graphbuilder.h"
#include "hubautorun.h"
#include "hubautosync.h"
#include "hubcomputers.h"
#include "hubkeeper.h"
#include "hubremote.h"
#include "hubrequests.h"
#include "hubsession.h"
#include "hubsync.h"
#include "hubwork.h"
#include "library.h"
#include "person.h"
#include "spaces.h"
#include "sshkey.h"
#include "sync.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// What a library holds, as every peer should end up holding it: each book by its global id, with what travels of it.
[[nodiscard]] QStringList contents(const Library &library) {
    QStringList books;
    for (const Book &book : library.books()) {
        QStringList hashes;
        for (const FormatFile &file : library.formatFiles(book.id))
            hashes << file.hash;
        hashes.sort();
        books << QStringList{Spaces::globalId(library, u"book"_s, book.id), book.meta.title, hashes.join(u","_s), book.shelf,
                             QString::number(book.rating)}
                     .join(u" | "_s);
    }
    books.sort();
    return books;
}

// How many operations a library's change log holds, read beside whoever has it open.
[[nodiscard]] qint64 opsIn(const QString &root) {
    const QString connection = u"ops-"_s + QUuid::createUuid().toString(QUuid::Id128);
    qint64 count = -1;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(u"QSQLITE"_s, connection);
        database.setDatabaseName(Library::databaseIn(root));
        database.setConnectOptions(u"QSQLITE_OPEN_READONLY"_s);
        if (database.open()) {
            QSqlQuery query(database);
            if (query.exec(u"SELECT COUNT(*) FROM ops"_s) && query.next())
                count = query.value(0).toLongLong();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return count;
}

// Whether a sync carried anything either way.
[[nodiscard]] int moved(const SyncSummary &summary) {
    return summary.received + summary.sent + summary.placesTaken + summary.placesSent + summary.detailsTaken + summary.detailsSent
           + summary.readingTaken + summary.readingSent + summary.removedHere + summary.removedThere + summary.collectionsTaken
           + summary.collectionsSent + summary.graphTaken + summary.graphSent + summary.annotationsTaken + summary.annotationsSent
           + summary.pluginFilesTaken + summary.pluginFilesSent;
}

[[nodiscard]] QString problems(const SyncSummary &summary) { return summary.error + summary.problems.join(u'\n'); }

// What the stand-in model finds in every passage it is sent.
const char *const found = R"({"nodes": [{"kind": "character", "name": "Mara Venn", "summary": "Keeps the lighthouse.", "section": 0}],
 "edges": []})";

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

[[nodiscard]] int chats(const FakeServer &server) {
    return static_cast<int>(std::ranges::count_if(server.received, [](const auto &request) { return request.line.startsWith("POST"); }));
}

// A book long enough for several requests: three chapters.
[[nodiscard]] bool longBook(const QString &file, const QString &title) {
    QList<Fixtures::Chapter> chapters;
    const QString paragraph = u"<p>Mara Venn climbed the stairs of the lighthouse again, as she did every night, and lit the lamp.</p>"_s;
    for (const QString &name : {u"One"_s, u"Two"_s, u"Three"_s})
        chapters << Fixtures::Chapter{.title = name, .body = paragraph.repeated(180)};
    return Fixtures::chapteredEpub(file, title, chapters);
}

// A person's computer: a library of its own.
struct Computer {
    QTemporaryDir dir;
    Library library{dir.filePath(u"library"_s)};

    Computer() { QVERIFY(library.open()); }

    // A book with a file, `title`, the same file on every computer that adds it.
    qint64 add(const QString &title) {
        const QString file = dir.filePath(title + u".epub"_s);
        return longBook(file, title) ? library.add(file).id : 0;
    }
};

// One of the person's hubs: its keeper of one library ("home"), its requests
// with the sync and its work among them, a stand-in model for that work, and
// the person's laptop and desktop linked by their keys.
struct Hub {
    QTemporaryDir dir;
    FakeServer server;
    HubSync sync{dir.filePath(u"scratch"_s)};
    HubKeeper keeper{dir.filePath(u"libraries"_s)};
    HubRequests requests = HubRequests::usual();
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    std::unique_ptr<HubWork> work;
    QHash<QString, QString> fingerprints;
    QHash<QString, bool> paired; // whose library paired with the hub's already

    [[nodiscard]] QString start(const Person &person) {
        server.answering = answered;
        QDir().mkpath(dir.filePath(u"scratch"_s));
        QDir().mkpath(dir.filePath(u"state"_s));
        const QJsonObject own{{"provider", "openai"}, {"hosted", false}};
        if (!Fixtures::write(dir.filePath(u"state/config.json"_s),
                             compact({{"ai_provider", "openai"},
                                      {"ai_endpoint", QJsonObject{{"preset", "custom"}, {"base_url", server.url()}}},
                                      {"capabilities", QJsonObject{{"answers", own}, {"quick", own}}}})))
            return u"no settings written"_s;
        if (Library made(dir.filePath(u"libraries/home"_s)); !made.open())
            return u"no library made"_s;
        sync.addTo(requests);
        if (const auto opened = keeper.open(); !opened)
            return whyNot(opened);
        work = std::make_unique<HubWork>(keeper, dir.filePath(u"state"_s), dir.filePath(u"state/config.json"_s));
        work->addTo(requests);
        for (const QString &name : {u"laptop"_s, u"desktop"_s}) {
            const auto key = SshKey::generate(dir.filePath(name), name);
            if (!key)
                return whyNot(key);
            const QString line = SshKey::line(*key);
            const Person::Signature signature = person.sign(Crypto::bytes(HubComputers::linkMessage(name, line)));
            const auto linked = computers.link({.name = name,
                                                .key = line,
                                                .identity = person.id(),
                                                .signature = QString::fromLatin1(Crypto::toByteArray(signature).toBase64(
                                                    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))});
            if (!linked)
                return whyNot(linked);
            fingerprints.insert(name, linked->fingerprint);
        }
        return {};
    }

    [[nodiscard]] QString library() const { return dir.filePath(u"libraries/home"_s); }

    // `computer`'s library synced with the hub's, through a session of its own; paired at its first sync.
    [[nodiscard]] SyncSummary syncWith(const QString &computer, Library &library) {
        HubSession session(computers, fingerprints.value(computer),
                           [this](const QByteArray &request) { return requests.answerLine(request, keeper).trimmed(); });
        std::ignore = session.answer(compact(HubSession::hello()));
        SyncOptions options;
        options.pair = !std::exchange(paired[computer], true);
        return HubRemote::sync(library, u"hub"_s, [&session](const QByteArray &line) { return session.answer(line); }, {}, options);
    }

    [[nodiscard]] QStringList held() {
        QStringList books;
        std::ignore = keeper.whole([&books](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            books = contents(*libraries.front());
            return {};
        });
        return books;
    }

    // The hub's work looked at, and waited for until nothing is under way.
    [[nodiscard]] bool worked() {
        if (!work->look())
            return false;
        return QTest::qWaitFor([this] { return !work->busy(); }, 30000);
    }

    // The names of what the graph of the hub's book holds.
    [[nodiscard]] QStringList graph() {
        QStringList names;
        std::ignore = keeper.whole([&names](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            const Library &library = *libraries.front();
            const qint64 book = library.books().value(0).id;
            for (const GraphNode &node : Graph::ofReader(library, book).ofBook(book).nodes)
                names << node.name;
            return {};
        });
        names.sort();
        return names;
    }

    [[nodiscard]] QString lastOutcome() {
        return requests.answer({{"request", "work"}}, keeper).value("last"_L1).toArray().first()["outcome"_L1].toString();
    }
};

// The person's hubs, by name, each started, all of one owner.
struct Hubs {
    std::map<QString, std::unique_ptr<Hub>> hubs;

    [[nodiscard]] QString start(const QStringList &names) {
        auto person = Person::generate();
        if (!person)
            return person.error();
        for (const QString &name : names) {
            auto hub = std::make_unique<Hub>();
            if (const QString why = hub->start(*person); !why.isEmpty())
                return name + u": "_s + why;
            hubs.emplace(name, std::move(hub));
        }
        return {};
    }
    Hub &operator[](const QString &name) { return *hubs.at(name); }
};

} // namespace

class HubsTests : public QObject {
    Q_OBJECT

private slots:
    // Three hubs and two computers, each computer syncing with the hubs in an
    // order of its own, end with one library on all five; changed on both sides,
    // through different hubs, they end the same again, one change of the same
    // thing kept everywhere. Then nothing goes back and forth any more.
    void threeHubsAndTwoComputersConverge() {
        Hubs hubs;
        const QString started = hubs.start({u"north"_s, u"south"_s, u"west"_s});
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        QVERIFY(laptop.add(u"Alpha"_s) > 0);
        QVERIFY(laptop.library.setReading(1, u"reading"_s, 4, {}));
        QVERIFY(desktop.add(u"Beta"_s) > 0);
        QVERIFY(desktop.add(u"Gamma"_s) > 0);
        // Each computer syncs with the hubs in its own order, the two taking turns.
        const auto round = [&](const QStringList &laptopOrder, const QStringList &desktopOrder) {
            for (qsizetype at = 0; at < laptopOrder.size(); ++at)
                for (const auto &[name, library, hub] : {std::tuple{u"laptop"_s, &laptop.library, laptopOrder.at(at)},
                                                         std::tuple{u"desktop"_s, &desktop.library, desktopOrder.at(at)}}) {
                    const SyncSummary synced = hubs[hub].syncWith(name, *library);
                    QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty(),
                             qPrintable(name + u" with "_s + hub + u": "_s + problems(synced)));
                }
        };
        const auto everywhere = [&](qsizetype books) {
            const QStringList one = contents(laptop.library);
            QCOMPARE(one.size(), books);
            QCOMPARE(contents(desktop.library), one);
            for (const auto &[name, hub] : hubs.hubs)
                QCOMPARE(hub->held(), one);
        };
        round({u"north"_s, u"south"_s, u"west"_s}, {u"west"_s, u"north"_s, u"south"_s});
        round({u"north"_s, u"south"_s, u"west"_s}, {u"west"_s, u"north"_s, u"south"_s});
        everywhere(3);

        // Each changes through one hub only, and the same book on both.
        const auto id = [](const Library &library, const QString &title) { return library.books(title).value(0).id; };
        QVERIFY(laptop.library.setReading(id(laptop.library, u"Beta"_s), u"read"_s, 3, u"2026-11-01"_s));
        QVERIFY(laptop.library.setReading(id(laptop.library, u"Gamma"_s), u"reading"_s, 2, {}));
        QVERIFY(desktop.library.setReading(id(desktop.library, u"Gamma"_s), u"read"_s, 5, u"2026-11-02"_s));
        QVERIFY(laptop.add(u"Delta"_s) > 0);
        QVERIFY(hubs[u"north"_s].syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QVERIFY(hubs[u"south"_s].syncWith(u"desktop"_s, desktop.library).error.isEmpty());
        round({u"west"_s, u"north"_s, u"south"_s}, {u"north"_s, u"west"_s, u"south"_s});
        round({u"west"_s, u"north"_s, u"south"_s}, {u"north"_s, u"west"_s, u"south"_s});
        // One of the two changes of Gamma's reading holds on all five, whichever it is.
        everywhere(4);
        const Book gamma = *laptop.library.book(id(laptop.library, u"Gamma"_s));
        QVERIFY(gamma.shelf == u"read"_s || gamma.shelf == u"reading"_s);

        // Settled: every sync of another round moves nothing, and no change log grows.
        std::map<QString, qint64> before{{u"laptop"_s, opsIn(laptop.library.root())}, {u"desktop"_s, opsIn(desktop.library.root())}};
        for (const auto &[name, hub] : hubs.hubs)
            before.emplace(name, opsIn(hub->library()));
        for (const auto &[name, hub] : hubs.hubs)
            for (const auto &[computer, library] : {std::pair{u"laptop"_s, &laptop.library}, std::pair{u"desktop"_s, &desktop.library}}) {
                const SyncSummary again = hub->syncWith(computer, *library);
                QVERIFY2(again.error.isEmpty() && again.problems.isEmpty(), qPrintable(problems(again)));
                QVERIFY2(moved(again) == 0, qPrintable(computer + u" with "_s + name + u": "_s
                                                       + QString::fromUtf8(QJsonDocument(again.toJson()).toJson(QJsonDocument::Compact))));
            }
        std::map<QString, qint64> after{{u"laptop"_s, opsIn(laptop.library.root())}, {u"desktop"_s, opsIn(desktop.library.root())}};
        for (const auto &[name, hub] : hubs.hubs)
            after.emplace(name, opsIn(hub->library()));
        QCOMPARE(after, before);
    }

    // What a sync with one hub brought is a change for the other hubs, which
    // sync a little later, as after a change of the person's; never for the hub
    // it came from, and a sync that brings nothing new starts no other.
    void whatOneHubBroughtIsAChangeForTheOthers() {
        QTemporaryDir dir;
        HubAutoSync automatic(dir.path());
        QVERIFY(automatic.turn(u"north"_s, true));
        QVERIFY(automatic.turn(u"south"_s, true));
        const QStringList hosts{u"north"_s, u"south"_s};
        const QDateTime start(QDate(2026, 11, 26), QTime(9, 0), QTimeZone::UTC);
        const auto synced = [&automatic](const QString &host, const QDateTime &at, int received) {
            automatic.began(at);
            SyncSummary summary;
            summary.received = received;
            std::ignore = automatic.done(host, summary, at);
        };
        for (const QString &host : hosts) {
            QCOMPARE(automatic.due(hosts, start).value_or(QString()), host);
            synced(host, start, 0);
        }
        QCOMPARE(automatic.due(hosts, start).value_or(QString()), QString());
        const QDateTime later = start.addSecs(5 * 60);
        synced(u"north"_s, later, 1);
        QCOMPARE(automatic.due(hosts, later).value_or(QString()), QString());
        const QDateTime settled = later.addSecs(automatic.times().settle.count());
        QCOMPARE(automatic.due(hosts, settled).value_or(QString()), u"south"_s);
        // South takes it, and brings nothing back: north isn't due.
        synced(u"south"_s, settled, 0);
        QCOMPARE(automatic.due(hosts, settled.addSecs(automatic.times().settle.count())).value_or(QString()), QString());
        // What south brings goes to north.
        const QDateTime again = settled.addSecs(60);
        synced(u"south"_s, again, 2);
        QCOMPARE(automatic.due(hosts, again.addSecs(automatic.times().settle.count())).value_or(QString()), u"north"_s);
    }

    // When the network is back, every hub that couldn't be reached is tried at once, not only the first.
    void everyHubOutOfReachIsTriedAsTheNetworkComesBack() {
        QTemporaryDir dir;
        HubAutoSync automatic(dir.path());
        const QStringList hosts{u"north"_s, u"south"_s};
        const QDateTime now(QDate(2026, 11, 26), QTime(9, 0), QTimeZone::UTC);
        SyncSummary unreached;
        unreached.unreached = true;
        unreached.error = u"out of reach"_s;
        for (const QString &host : hosts) {
            QVERIFY(automatic.turn(host, true));
            automatic.began(now);
            std::ignore = automatic.done(host, unreached, now);
        }
        QCOMPARE(automatic.due(hosts, now).value_or(QString()), QString());
        automatic.cameIntoReach();
        for (const QString &host : hosts) {
            QCOMPARE(automatic.due(hosts, now).value_or(QString()), host);
            automatic.began(now);
            std::ignore = automatic.done(host, SyncSummary{}, now);
        }
        QCOMPARE(automatic.due(hosts, now).value_or(QString()), QString());
    }

    // The background service syncs with each hub in turn: one out of reach
    // doesn't stop the others, what the desktop gave one reaches this computer
    // and the next hub, and the hub that was down gets it once it is back.
    void oneHubDownTheOthersGoOn() {
        Hubs hubs;
        const QString started = hubs.start({u"north"_s, u"south"_s, u"west"_s});
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Computer laptop;
        Computer desktop;
        QVERIFY(laptop.add(u"Alpha"_s) > 0);
        QVERIFY(desktop.add(u"Beta"_s) > 0);
        for (const auto &[name, hub] : hubs.hubs)
            for (const auto &[computer, library] : {std::pair{u"laptop"_s, &laptop.library}, std::pair{u"desktop"_s, &desktop.library}})
                QVERIFY(hub->syncWith(computer, *library).error.isEmpty());
        QTemporaryDir state;
        QDateTime now(QDate(2026, 11, 26), QTime(9, 0), QTimeZone::UTC);
        bool northDown = true;
        QStringList tried;
        HubAutoRun service({.folder = state.path(),
                            .hosts = [] { return QStringList{u"north"_s, u"south"_s, u"west"_s}; },
                            .sync =
                                [&](const QString &host) {
                                    tried << host;
                                    if (host == u"north"_s && northDown) {
                                        SyncSummary summary;
                                        summary.unreached = true;
                                        summary.error = u"north: Connection timed out"_s;
                                        return summary;
                                    }
                                    return hubs[host].syncWith(u"laptop"_s, laptop.library);
                                },
                            .now = [&now] { return now; },
                            .wait = [] { return SyncWait::Reason::None; },
                            .notify = {},
                            .every = 1h});
        {
            HubAutoSync automatic(state.path());
            for (const QString &host : {u"north"_s, u"south"_s, u"west"_s})
                QVERIFY(automatic.turn(host, true));
        }
        QVERIFY(desktop.add(u"Gamma"_s) > 0);
        QVERIFY(hubs[u"south"_s].syncWith(u"desktop"_s, desktop.library).error.isEmpty());
        for (int tick = 0; tick < 4; ++tick)
            service.tick();
        QCOMPARE(tried, (QStringList{u"north"_s, u"south"_s, u"west"_s}));
        QCOMPARE(laptop.library.books(u"Gamma"_s).size(), 1);
        QCOMPARE(hubs[u"west"_s].held(), contents(laptop.library));
        // North is tried again in a while, and given it once it answers.
        northDown = false;
        now = now.addSecs(HubAutoSync::Times{}.again.count());
        service.tick();
        QCOMPARE(tried.last(), u"north"_s);
        QCOMPARE(hubs[u"north"_s].held(), contents(laptop.library));
        QCOMPARE(hubs[u"north"_s].held().size(), 3);
    }

    // The first hub to take up a book's graph claims it in the library's
    // change log; another hub that learns of the claim by a sync leaves the
    // book to it, and gets the graph by the sync once it is built.
    void aBooksGraphIsBuiltOnceByTheHubThatClaimedIt() {
        Hubs hubs;
        const QString started = hubs.start({u"north"_s, u"south"_s});
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Hub &north = hubs[u"north"_s];
        Hub &south = hubs[u"south"_s];
        Computer laptop;
        QVERIFY(laptop.add(u"The Lighthouse"_s) > 0);
        QVERIFY(north.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QVERIFY(south.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        // While north builds it, the laptop syncs with both: south learns of north's claim.
        std::optional<bool> carried;
        north.server.answering = [&](const FakeServer::Request &request) {
            if (request.line.startsWith("POST") && !carried)
                carried = north.syncWith(u"laptop"_s, laptop.library).error.isEmpty()
                          && south.syncWith(u"laptop"_s, laptop.library).error.isEmpty();
            return answered(request);
        };
        QVERIFY(north.worked());
        QVERIFY(carried.value_or(false));
        QCOMPARE(north.lastOutcome(), u"done"_s);
        QVERIFY(south.worked());
        QCOMPARE(chats(south.server), 0);
        QVERIFY(!south.requests.answer({{"request", "work"}}, south.keeper).contains("running"_L1));
        // The graph north built reaches south by the sync, and south builds none of its own.
        QVERIFY(north.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QVERIFY(south.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QVERIFY(north.graph().contains(u"Mara Venn"_s));
        QCOMPARE(south.graph(), north.graph());
        QVERIFY(south.worked());
        QCOMPARE(chats(south.server), 0);
    }

    // Two hubs that took up the same book before either knew of the other:
    // the later claim holds once a sync carries both, and the hub whose claim
    // was overtaken keeps nothing of its work, so the graph is built once.
    void aHubWhoseClaimWasOvertakenKeepsNothing() {
        Hubs hubs;
        const QString started = hubs.start({u"north"_s, u"south"_s});
        QVERIFY2(started.isEmpty(), qPrintable(started));
        Hub &north = hubs[u"north"_s];
        Hub &south = hubs[u"south"_s];
        Computer laptop;
        QVERIFY(laptop.add(u"The Lighthouse"_s) > 0);
        QVERIFY(north.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QVERIFY(south.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        std::optional<bool> overtaken;
        north.server.answering = [&](const FakeServer::Request &request) {
            if (request.line.startsWith("POST") && !overtaken) {
                // South takes it up too, a moment later, and the laptop carries south's claim to north.
                QTest::qWait(1100);
                overtaken = south.work->look().has_value() && south.work->busy()
                            && south.syncWith(u"laptop"_s, laptop.library).error.isEmpty()
                            && north.syncWith(u"laptop"_s, laptop.library).error.isEmpty();
            }
            return answered(request);
        };
        QVERIFY(north.worked());
        QVERIFY(QTest::qWaitFor([&south] { return !south.work->busy(); }, 30000));
        QVERIFY(overtaken.value_or(false));
        QCOMPARE(north.lastOutcome(), u"failed"_s);
        QCOMPARE(south.lastOutcome(), u"done"_s);
        QVERIFY(north.graph().isEmpty());
        QVERIFY(south.graph().contains(u"Mara Venn"_s));
        QVERIFY(south.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QVERIFY(north.syncWith(u"laptop"_s, laptop.library).error.isEmpty());
        QCOMPARE(north.graph(), south.graph());
    }
};

QTEST_GUILESS_MAIN(HubsTests)
#include "hubs_tests.moc"
