#pragma once

// The person's hub, in this process and in temporary folders, for the tests
// of the window's side of the hubs (hubspanel_tests, hubautosync_tests): its
// keeper of one library, its requests with the snapshots' among them, its
// computers' keys, and the session's pipe (HubSession), as hubsync_tests has
// it, and its phones and e-readers (#193). Linking does there what `omabook hub
// allow` does over the person's ssh.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutex>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>

#include "bookmeta.h"
#include "crypto.h"
#include "fixtures.h"
#include "hubcomputers.h"
#include "hubdesk.h"
#include "hubdevices.h"
#include "hubkeeper.h"
#include "hubremote.h"
#include "hubrequests.h"
#include "hubsession.h"
#include "hubsnapshots.h"
#include "hubsync.h"
#include "library.h"
#include "person.h"
#include "snapshothealth.h"
#include "snapshots.h"
#include "spaces.h"
#include "sshkey.h"
#include "sync.h"

namespace HubTests {

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

[[nodiscard]] inline QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

[[nodiscard]] inline QString signedBy(const Person &person, const QString &name, const QString &line) {
    const Person::Signature signature = person.sign(Crypto::bytes(HubComputers::linkMessage(name, line)));
    return QString::fromLatin1(Crypto::toByteArray(signature).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// The person's hub, at "hub", with this computer's side of the links beside
// it. Whatever the panel asks, on its worker, is answered on the window's
// thread, where the hub's libraries were opened.
class HubHere : public HubDesk::Hubs {
public:
    QTemporaryDir dir;
    HubSync syncs{dir.filePath(u"scratch"_s)};
    mutable HubKeeper keeper{dir.filePath(u"libraries"_s)}; // written as the daemon writes, whoever asks
    HubRequests requests = HubRequests::usual();
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    Snapshots snapshots{SnapshotPlace::in(dir.filePath(u"snapshots"_s)),
                        SnapshotContents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)}};
    std::optional<SnapshotHealth> health;
    std::optional<HubDevices> devices; // its server on the window's thread, as the daemon's is on its own
    std::optional<Person> person;
    mutable QObject home; // on the window's thread
    std::atomic<bool> reachable = true;
    // Its background work (#192): a book's graph under way, until it is cancelled.
    bool working = true;
    // How many syncs were begun with it, previews among them.
    std::atomic<int> begun = 0;

    // The hub's library paired with another computer's library and not this one's (#214).
    [[nodiscard]] bool pairedElsewhere() {
        const QString other = QUuid::createUuid().toString(QUuid::Id128);
        return keeper
            .whole([&other](std::span<Library *const> libraries) -> std::expected<void, Failure> {
                Spaces::pair(*libraries.front(), u"personal:"_s + other, other, u"desktop"_s);
                return {};
            })
            .has_value();
    }

    // The hub with a library of one book, its snapshots set up and their
    // password gone, and the person's desktop linked to it already; and this
    // computer's library, of another book.
    [[nodiscard]] QString start() {
        QDir().mkpath(dir.filePath(u"scratch"_s));
        {
            Library here(laptopLibrary());
            if (!here.open() || !Fixtures::epub(dir.filePath(u"Lichen.epub"_s), u"Lichen"_s)
                || here.add(dir.filePath(u"Lichen.epub"_s)).id <= 0)
                return u"no library made here"_s;
        }
        {
            Library made(dir.filePath(u"libraries/home"_s));
            BookMeta meta;
            meta.title = u"Moss"_s;
            if (!made.open() || made.addEntry(meta, true).id <= 0)
                return u"no library made"_s;
        }
        if (const auto opened = keeper.open(); !opened)
            return whyNot(opened);
        if (const auto made = snapshots.setUp(dir.filePath(u"repository"_s)); !made)
            return whyNot(made);
        if (!QFile::remove(dir.filePath(u"snapshots/password"_s)))
            return u"no password to take away"_s;
        syncs.addTo(requests);
        health.emplace(snapshots);
        requests.add(u"snapshots"_s, HubSnapshots::status(*health));
        requests.add(u"work"_s, [this](const QJsonObject &, HubKeeper &) -> std::expected<QJsonObject, Failure> {
            if (!working)
                return QJsonObject{{"waiting", 0}, {"last", QJsonArray{}}};
            return QJsonObject{{"waiting", 0}, {"running", QJsonObject{{"title", "Moss"}, {"kind", "graph"}, {"done", 1}, {"total", 3}}}};
        });
        requests.add(u"work-cancel"_s, [this](const QJsonObject &, HubKeeper &) -> std::expected<QJsonObject, Failure> {
            const bool was = std::exchange(working, false);
            return QJsonObject{{"cancelled", was}};
        });
        devices.emplace(keeper, HubPlace({.state = dir.filePath(u"hub-state"_s), .runtime = dir.filePath(u"hub-run"_s), .credentials = {}}),
                        HubDevices::Around{.tailscale = {}, .interfaces = [] { return QList<QNetworkInterface>(); }});
        devices->addTo(requests);
        devices->follow();
        auto generated = Person::generate();
        if (!generated)
            return generated.error();
        person.emplace(std::move(*generated));
        const auto desktop = allow(u"desktop"_s);
        return desktop ? QString() : whyNot(desktop);
    }

    [[nodiscard]] QString laptopLibrary() const { return dir.filePath(u"laptop"_s); }
    // How many books the hub's library has, and this computer's.
    [[nodiscard]] qsizetype onHub() {
        qsizetype books = 0;
        std::ignore = keeper.whole([&books](std::span<Library *const> libraries) -> std::expected<void, Failure> {
            books = libraries.front()->books().size();
            return {};
        });
        return books;
    }
    [[nodiscard]] qsizetype here() const {
        Library library(laptopLibrary());
        return library.open() ? library.books().size() : -1;
    }

    [[nodiscard]] QStringList names() const {
        QStringList names;
        for (const HubComputers::Computer &computer : computers.computers())
            names << computer.name;
        return names;
    }
    void takeAway(const QString &name) { std::ignore = computers.unlink(name); }

    [[nodiscard]] QList<HubLinks::Hub> linked() const override {
        const QMutexLocker locked(&m_lock);
        return m_links;
    }

    [[nodiscard]] std::expected<HubLinks::Hub, Failure> add(const QString &host, const QString &computer) override {
        return onHome<std::expected<HubLinks::Hub, Failure>>([&]() -> std::expected<HubLinks::Hub, Failure> {
            if (host != u"hub"_s || !reachable)
                return std::unexpected(Failure(Failure::Code::HubUnreachable, {host, u"Could not resolve hostname"_s}));
            const auto allowed = allow(computer);
            if (!allowed)
                return std::unexpected(allowed.error());
            const HubLinks::Hub hub{
                .host = host, .computer = allowed->name, .fingerprint = allowed->fingerprint, .linked = QDateTime::currentDateTimeUtc()};
            const QMutexLocker locked(&m_lock);
            m_links << hub;
            return hub;
        });
    }

    [[nodiscard]] std::expected<HubLinks::Hub, Failure> remove(const QString &host) override {
        const QList<HubLinks::Hub> links = linked();
        const auto hub = std::ranges::find_if(links, [&host](const HubLinks::Hub &each) { return each.host == host; });
        if (hub == links.end())
            return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
        if (const auto unlinked = ask(host, {{"request", "unlink"}}); !unlinked && unlinked.error().code() != Failure::Code::HubNotLinked)
            return std::unexpected(unlinked.error());
        const QMutexLocker locked(&m_lock);
        const HubLinks::Hub gone = *hub;
        m_links.removeIf([&host](const HubLinks::Hub &each) { return each.host == host; });
        return gone;
    }

    // As HubLinks::ask has it: the hello, then the request, over the session.
    [[nodiscard]] std::expected<QJsonObject, Failure> ask(const QString &host, const QJsonObject &request) const override {
        const QList<HubLinks::Hub> links = linked();
        const auto hub = std::ranges::find_if(links, [&host](const HubLinks::Hub &each) { return each.host == host; });
        if (hub == links.end())
            return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
        return onHome<std::expected<QJsonObject, Failure>>([&]() -> std::expected<QJsonObject, Failure> {
            if (!reachable)
                return std::unexpected(Failure(Failure::Code::HubUnreachable, {host, u"Connection timed out"_s}));
            // The pipe the daemon's socket would be.
            HubSession session(computers, hub->fingerprint,
                               [this](const QByteArray &line) { return requests.answerLine(line, keeper).trimmed(); });
            const QJsonObject greeting = QJsonDocument::fromJson(session.answer(compact(HubSession::hello()))).object();
            if (greeting.contains("linked"_L1) && !greeting.value("linked"_L1).toBool())
                return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
            const QJsonObject answer = QJsonDocument::fromJson(session.answer(compact(request))).object();
            if (!answer.value("ok"_L1).toBool())
                return std::unexpected(Failure::said(answer.value("error"_L1).toString()));
            return answer;
        });
    }

    // This computer's library synced with the hub's, through one session, on the
    // worker as HubLinks' would be; each line answered where the hub lives.
    [[nodiscard]] SyncSummary sync(const QString &host, const SyncOptions &options) override {
        SyncSummary summary;
        ++begun;
        if (!reachable) {
            summary.error = Failure(Failure::Code::HubUnreachable, {host, u"Connection timed out"_s}).text();
            summary.unreached = true;
            return summary;
        }
        const QList<HubLinks::Hub> links = linked();
        const auto hub = std::ranges::find_if(links, [&host](const HubLinks::Hub &each) { return each.host == host; });
        Library library(laptopLibrary());
        if (hub == links.end() || !library.open(&summary.error))
            return summary;
        auto session = onHome<std::unique_ptr<HubSession>>([&] {
            auto opened = std::make_unique<HubSession>(
                computers, hub->fingerprint, [this](const QByteArray &line) { return requests.answerLine(line, keeper).trimmed(); });
            std::ignore = opened->answer(compact(HubSession::hello())); // its greeting, read by ask()'s tests
            return opened;
        });
        return HubRemote::sync(
            library, host, [&](const QByteArray &line) { return onHome<QByteArray>([&] { return session->answer(line); }); }, {}, options);
    }

private:
    // What `omabook hub allow` does on the hub: the computer's key, signed by the person, installed.
    std::expected<HubComputers::Computer, Failure> allow(const QString &name) {
        const auto key = SshKey::generate(dir.filePath(u"keys/"_s + name), name);
        if (!key)
            return std::unexpected(key.error());
        const QString line = SshKey::line(*key);
        return computers.link({.name = name, .key = line, .identity = person->id(), .signature = signedBy(*person, name, line)});
    }

    template <typename Answer> Answer onHome(const std::function<Answer()> &work) const {
        if (QThread::currentThread() == home.thread())
            return work();
        std::optional<Answer> answered;
        QMetaObject::invokeMethod(&home, [&] { answered.emplace(work()); }, Qt::BlockingQueuedConnection);
        return std::move(*answered);
    }

    mutable QMutex m_lock;
    QList<HubLinks::Hub> m_links;
};

} // namespace HubTests
