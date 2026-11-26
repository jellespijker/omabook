// Phones and e-readers through the hub (#193): the hub serves its library's
// OPDS catalog and KOReader's progress sync itself, always on, with no
// computer awake; a place KOReader sends is written through the hub's keeper,
// its one writer (ADR 0013), and reaches a computer by the normal sync; a
// wrong password or token is refused; it listens only where the reach chosen
// allows, and nowhere when it is off; and what is chosen, set from a computer
// through the session, is kept in the hub's own settings and secrets, served
// again when the daemon starts again.
//
// Everything is in temporary folders: a hub's keeper and its state, its
// server on this computer only, and a computer's library synced through a
// session, all in this process.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <span>

#include "crypto.h"
#include "fakedevice.h"
#include "fixtures.h"
#include "hubcomputers.h"
#include "hubdevices.h"
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubremote.h"
#include "hubrequests.h"
#include "hubsession.h"
#include "hubsync.h"
#include "koreaderdocument.h"
#include "kosyncclient.h"
#include "library.h"
#include "person.h"
#include "sshkey.h"
#include "sync.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

[[nodiscard]] quint16 freePort() {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, 0) ? probe.serverPort() : 0;
}

[[nodiscard]] bool canListen(quint16 port) {
    QTcpServer probe;
    return probe.listen(QHostAddress::Any, port);
}

[[nodiscard]] QHash<QByteArray, QByteArray> withToken(const QString &token) {
    return {{"Authorization", "Basic " + ("kobo:" + token.toUtf8()).toBase64()}};
}

[[nodiscard]] QByteArray body(const QJsonObject &object) { return compact(object); }

// A hub of one library ("home") with a book of three chapters in it, its
// sync, its phones and e-readers, and one computer linked: the laptop.
struct Hub {
    QTemporaryDir dir;
    HubKeeper keeper{dir.filePath(u"libraries"_s)};
    HubRequests requests = HubRequests::usual();
    HubSync sync{dir.filePath(u"scratch"_s)};
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    std::unique_ptr<HubDevices> devices;
    QString laptop;
    QString document; // the book's file, as KOReader names it

    [[nodiscard]] HubPlace place() const {
        return HubPlace({.state = dir.filePath(u"state"_s), .runtime = dir.filePath(u"run"_s), .credentials = {}});
    }

    [[nodiscard]] QString start() {
        QDir().mkpath(dir.filePath(u"scratch"_s));
        QDir().mkpath(dir.filePath(u"state"_s));
        {
            // Made before the keeper holds it, as the hub's folder is at setup.
            Library made(dir.filePath(u"libraries/home"_s));
            const QString file = dir.filePath(u"three.epub"_s);
            const QString words(400, u'w');
            if (!made.open()
                || !Fixtures::chapteredEpub(file, u"Three Chapters"_s,
                                            {{u"One"_s, u"<p>"_s + words + u"</p>"_s},
                                             {u"Two"_s, u"<p>"_s + words + u"</p>"_s},
                                             {u"Three"_s, u"<p>"_s + words + words + u"</p>"_s}}))
                return u"no library made"_s;
            const qint64 id = made.add(file).id;
            if (id <= 0)
                return u"no book added"_s;
            document = KoreaderDocument::hash(made.formatPath(id, u"epub"_s));
        }
        if (const auto opened = keeper.open(); !opened)
            return whyNot(opened);
        sync.addTo(requests);
        restart();
        return link();
    }

    // The daemon started again: its phones and e-readers as its settings say.
    void restart(HubDevices::Around around = {}) {
        devices.reset();
        around.tailscale = dir.filePath(u"no-tailscale"_s);
        devices = std::make_unique<HubDevices>(keeper, place(), std::move(around));
        devices->addTo(requests);
        devices->follow();
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

    // What the laptop's session is answered, as the daemon would answer it.
    [[nodiscard]] QJsonObject ask(const QJsonObject &request) {
        HubSession session(computers, laptop, [this](const QByteArray &line) { return requests.answerLine(line, keeper).trimmed(); });
        std::ignore = session.answer(compact(HubSession::hello()));
        return QJsonDocument::fromJson(session.answer(compact(request))).object();
    }

    // Served on this computer only, at a free port, the catalog and KOReader's sync for "reader".
    [[nodiscard]] QJsonObject serve(const QString &password = u"secret"_s) {
        return ask({{"request", "devices-set"},
                    {"options", QJsonObject{{"serve", "localhost"},
                                            {"port", freePort()},
                                            {"opds", true},
                                            {"koreader_serve", true},
                                            {"koreader_serve_user", "reader"}}},
                    {"password", password}});
    }

    [[nodiscard]] Progress progress() {
        Progress read;
        std::ignore = keeper.whole([&read](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            const Library &library = *libraries.front();
            read = library.progress(library.books().value(0).id);
            return {};
        });
        return read;
    }

    [[nodiscard]] SyncSummary syncWith(Library &library) {
        HubSession session(computers, laptop, [this](const QByteArray &request) { return requests.answerLine(request, keeper).trimmed(); });
        std::ignore = session.answer(compact(HubSession::hello()));
        return HubRemote::sync(library, u"hub"_s, [&session](const QByteArray &line) { return session.answer(line); }, {}, {});
    }
};

const QJsonObject read75{
    {"progress", "/body/DocFragment[3]/body/p[1]/text().0"}, {"percentage", 0.75}, {"device", "Kobo"}, {"device_id", "KOBO1"}};

} // namespace

class HubPhonesTests : public QObject {
    Q_OBJECT

private slots:
    // Off until a computer chooses where it may be reached; then the catalog lists the hub library's books.
    void theHubServesItsLibrarysCatalog() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QJsonObject status = hub.ask({{"request", "devices"}});
        QVERIFY2(status.value("ok"_L1).toBool(), qPrintable(QString::fromUtf8(compact(status))));
        QCOMPARE(status.value("options"_L1).toObject().value("serve"_L1).toString(), u"off"_s);
        QVERIFY(status.value("address"_L1).toString().isEmpty());
        QCOMPARE(status.value("libraries"_L1).toArray(), QJsonArray{u"home"_s});

        status = hub.serve();
        QVERIFY2(status.value("ok"_L1).toBool(), qPrintable(QString::fromUtf8(compact(status))));
        const QString address = status.value("address"_L1).toString();
        QVERIFY2(address.startsWith(u"http://127.0.0.1:"_s), qPrintable(address));
        const QString token = status.value("token"_L1).toString();
        QCOMPARE(token.size(), 24);
        QVERIFY(status.value("password"_L1).toBool());
        QVERIFY(!status.contains("said"_L1) || status.value("said"_L1).toString().isEmpty());

        const FakeDevice::Answer books = FakeDevice::ask("GET", QUrl(address + u"/opds/books"_s), withToken(token));
        QCOMPARE(books.status, 200);
        QVERIFY2(books.body.contains("Three Chapters"), books.body.constData());
        // And a book's file, as an app downloads it.
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/t/"_s + token + u"/opds"_s)).status, 200);
        // A wrong token is refused.
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/opds/books"_s), withToken(u"wrong"_s)).status, 401);
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/t/wrong/opds"_s)).status, 401);
    }

    // KOReader's place is the hub library's, written through the keeper, and the laptop has it after a sync.
    void aKoreaderPlaceLandsThroughTheKeeperAndReachesAComputer() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QTemporaryDir laptopDir;
        Library laptop(laptopDir.filePath(u"library"_s));
        QVERIFY(laptop.open());
        SyncSummary synced = hub.syncWith(laptop);
        QVERIFY2(synced.error.isEmpty(), qPrintable(synced.error));
        QCOMPARE(laptop.books().size(), 1);
        const qint64 book = laptop.books().first().id;

        const QJsonObject status = hub.serve();
        const QString address = status.value("address"_L1).toString();
        QVERIFY2(!address.isEmpty(), qPrintable(QString::fromUtf8(compact(status))));
        const QByteArray key = KosyncClient::keyOf(u"secret"_s).toUtf8();
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/users/auth"_s), FakeDevice::koreader("reader", key)).status, 200);
        QJsonObject progress = read75;
        progress.insert("document"_L1, hub.document);
        const FakeDevice::Answer put =
            FakeDevice::ask("PUT", QUrl(address + u"/syncs/progress"_s), FakeDevice::koreader("reader", key), body(progress));
        QCOMPARE(put.status, 200);
        const Progress there = hub.progress();
        QCOMPARE(there.place.section, 2);
        QCOMPARE(qRound(there.percent), 75);
        // Asked back, KOReader is told its own place.
        const QJsonObject got =
            FakeDevice::ask("GET", QUrl(address + u"/syncs/progress/"_s + hub.document), FakeDevice::koreader("reader", key)).json();
        QCOMPARE(got.value("device_id"_L1).toString(), u"KOBO1"_s);

        // The laptop, waking later, syncs it.
        QCOMPARE(qRound(laptop.progress(book).percent), 0);
        synced = hub.syncWith(laptop);
        QVERIFY2(synced.error.isEmpty(), qPrintable(synced.error));
        QCOMPARE(laptop.progress(book).place.section, 2);
        QCOMPARE(qRound(laptop.progress(book).percent), 75);
    }

    void aWrongPasswordIsRefused() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const QString address = hub.serve().value("address"_L1).toString();
        QVERIFY(!address.isEmpty());
        const QByteArray wrong = KosyncClient::keyOf(u"not it"_s).toUtf8();
        const FakeDevice::Answer auth = FakeDevice::ask("GET", QUrl(address + u"/users/auth"_s), FakeDevice::koreader("reader", wrong));
        QCOMPARE(auth.status, 401);
        QJsonObject progress = read75;
        progress.insert("document"_L1, hub.document);
        QCOMPARE(FakeDevice::ask("PUT", QUrl(address + u"/syncs/progress"_s), FakeDevice::koreader("reader", wrong), body(progress)).status,
                 401);
        // Nor someone else by the right password.
        const QByteArray key = KosyncClient::keyOf(u"secret"_s).toUtf8();
        QCOMPARE(FakeDevice::ask("PUT", QUrl(address + u"/syncs/progress"_s), FakeDevice::koreader("other", key), body(progress)).status,
                 401);
        QCOMPARE(qRound(hub.progress().percent), 0);
    }

    // What a computer chose is the hub's own: kept in its settings, its secrets for it alone, and served again on restart.
    void itsChoicesOutliveTheDaemon() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const QJsonObject before = hub.serve();
        const QString address = before.value("address"_L1).toString();
        QVERIFY(!address.isEmpty());
        QFile settings(hub.place().settings());
        QVERIFY(settings.open(QIODevice::ReadOnly));
        const QByteArray kept = settings.readAll();
        QVERIFY2(kept.contains("\"devices\""), kept.constData());
        QVERIFY2(!kept.contains("secret") && !kept.contains(before.value("token"_L1).toString().toUtf8()), kept.constData());
        for (const char *secret : {"devices-opds", "devices-koreader-serve"}) {
            const QFileInfo file(QDir(hub.dir.filePath(u"state/secrets"_s)).filePath(QString::fromLatin1(secret)));
            QVERIFY2(file.exists(), secret);
            QCOMPARE(file.permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
        }

        hub.restart();
        const QJsonObject after = hub.ask({{"request", "devices"}});
        QCOMPARE(after.value("address"_L1).toString(), address);
        QCOMPARE(after.value("token"_L1).toString(), before.value("token"_L1).toString());
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/opds/books"_s), withToken(after.value("token"_L1).toString())).status, 200);

        // A new token: the old one no longer opens the catalog.
        const QJsonObject renewed = hub.ask({{"request", "devices-set"}, {"new_token", true}});
        const QString token = renewed.value("token"_L1).toString();
        QVERIFY(!token.isEmpty() && token != before.value("token"_L1).toString());
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/opds/books"_s), withToken(before.value("token"_L1).toString())).status, 401);
        QCOMPARE(FakeDevice::ask("GET", QUrl(address + u"/opds/books"_s), withToken(token)).status, 200);
    }

    // Never more than the reach chosen: off is nowhere, and the tailnet is nowhere on a hub that isn't on one.
    void itListensOnlyWhereItMay() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const QJsonObject serving = hub.serve();
        const QString address = serving.value("address"_L1).toString();
        const quint16 port = static_cast<quint16>(serving.value("options"_L1).toObject().value("port"_L1).toInt());
        QVERIFY(!address.isEmpty() && port > 0);
        QVERIFY(!canListen(port));

        QJsonObject off = hub.ask({{"request", "devices-set"}, {"options", QJsonObject{{"serve", "off"}}}});
        QVERIFY(off.value("ok"_L1).toBool());
        QVERIFY(off.value("address"_L1).toString().isEmpty());
        QVERIFY(canListen(port));

        hub.restart({.tailscale = {}, .interfaces = [] { return QList<QNetworkInterface>(); }});
        const QJsonObject tailnet = hub.ask({{"request", "devices-set"}, {"options", QJsonObject{{"serve", "tailnet"}}}});
        QVERIFY(tailnet.value("ok"_L1).toBool());
        QVERIFY(tailnet.value("address"_L1).toString().isEmpty());
        QCOMPARE(tailnet.value("problem"_L1).toString(), u"no-tailnet"_s);
        QVERIFY(canListen(port));
    }

    // A reach, a port or a library the hub doesn't know is refused, and nothing changes.
    void aWrongChoiceIsRefused() {
        Hub hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        for (const QJsonObject &options :
             {QJsonObject{{"serve", "everywhere"}}, QJsonObject{{"port", 70000}}, QJsonObject{{"library", "elsewhere"}}}) {
            const QJsonObject refused = hub.ask({{"request", "devices-set"}, {"options", options}});
            QVERIFY2(!refused.value("ok"_L1).toBool(), qPrintable(QString::fromUtf8(compact(options))));
        }
        QCOMPARE(hub.ask({{"request", "devices"}}).value("options"_L1).toObject().value("serve"_L1).toString(), u"off"_s);
        // The session hands both requests on, and nothing else of them.
        QVERIFY(HubSession::relayed().contains(u"devices"_s));
        QVERIFY(HubSession::relayed().contains(u"devices-set"_s));
    }
};

QTEST_GUILESS_MAIN(HubPhonesTests)
#include "hubphones_tests.moc"
