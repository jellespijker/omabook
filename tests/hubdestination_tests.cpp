// Another of the person's hubs as a destination of a hub's snapshots (#196,
// ADR 0014, GLOSSARY: Destination): "hub:<host>", a restic repository kept on
// that hub for the sending hub, reached over the session with it (HubStore
// there, HubStoreBridge here), never a shell. The hub that keeps it can't read
// it (the password stays with the sender), it is laid out as restic lays out
// a disk's so it can be restored from there too, it counts as a copy away from
// home, the checks of #184 reach it, and a hub out of reach stops no other
// destination. What the session's store is asked is the key's computer's
// alone, and of restic's files alone.
//
// Everything is in temporary folders: the hub that sends, with its snapshots
// of a library, and the hub that keeps, with its computers' keys and its
// stores, over the session's pipe in this process; restic is the real one.

#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <memory>

#include "bookmeta.h"
#include "crypto.h"
#include "fixtures.h"
#include "hubcomputers.h"
#include "hubsession.h"
#include "hubstore.h"
#include "hubstorebridge.h"
#include "library.h"
#include "person.h"
#include "snapshotdestinations.h"
#include "snapshothealth.h"
#include "snapshots.h"
#include "sshkey.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

constexpr auto secretTitle = "The Lichen Ledger of Marrow Bay"_L1;

// The hub that keeps the snapshots: its computers' keys, the sending hub
// ("home") and the person's desktop among them, and its stores.
struct Keeping {
    QTemporaryDir dir;
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    QHash<QString, QString> fingerprints;
    std::atomic<bool> reachable = true;
    std::atomic<int> sessions = 0;

    [[nodiscard]] QString stores() const { return HubStore::in(dir.filePath(u"state"_s)); }

    [[nodiscard]] QString start() {
        const auto person = Person::generate();
        if (!person)
            return person.error();
        for (const QString &name : {u"home"_s, u"desktop"_s}) {
            const auto key = SshKey::generate(dir.filePath(u"keys/"_s + name), name);
            if (!key)
                return whyNot(key);
            const QString line = SshKey::line(*key);
            const Person::Signature signature = person->sign(Crypto::bytes(HubComputers::linkMessage(name, line)));
            const auto linked = computers.link({.name = name,
                                                .key = line,
                                                .identity = person->id(),
                                                .signature = QString::fromLatin1(Crypto::toByteArray(signature).toBase64(
                                                    QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))});
            if (!linked)
                return whyNot(linked);
            fingerprints.insert(name, linked->fingerprint);
        }
        return {};
    }

    // A session as `computer`'s key opens it, its hello said; the daemon refuses all it is handed.
    [[nodiscard]] std::shared_ptr<HubSession> session(const QString &computer) {
        auto opened = std::make_shared<HubSession>(
            computers, fingerprints.value(computer),
            [](const QByteArray &) { return QByteArray(R"({"ok": false, "error": "no daemon"})"); }, stores());
        std::ignore = opened->answer(compact(HubSession::hello()));
        return opened;
    }

    // How the sending hub reaches this one: over a session as "home", when it is in reach.
    [[nodiscard]] SnapshotDestinations::HubReach reach() {
        return HubStoreBridge::reach([this](const QString &host) -> std::expected<HubStoreBridge::Ask, Failure> {
            if (host != u"away"_s || !reachable)
                return std::unexpected(Failure(Failure::Code::HubUnreachable, {host, u"Connection timed out"_s}));
            ++sessions;
            auto opened = session(u"home"_s);
            return HubStoreBridge::Ask([opened](const QByteArray &line) { return opened->answer(line); });
        });
    }
};

// The hub that sends: its snapshots, set up, of a library with one book.
struct Sending {
    QTemporaryDir dir;
    QString state = dir.filePath(u"state/snapshots"_s);
    SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::in(state), contents); }
    [[nodiscard]] SnapshotDestinations destinations() const { return SnapshotDestinations(SnapshotPlace::in(state)); }

    [[nodiscard]] QString start() {
        Snapshots made = snapshots();
        if (const auto given = made.setUp(dir.filePath(u"repository"_s)); !given)
            return whyNot(given);
        Library library(dir.filePath(u"Library"_s));
        BookMeta meta;
        meta.title = secretTitle;
        if (!library.open() || library.addEntry(meta, true).id <= 0)
            return u"no library"_s;
        const auto taken = snapshots().take(library, SnapshotReason::Asked);
        return taken ? QString() : whyNot(taken);
    }
};

// restic on a repository in a folder, with `key` alone.
[[nodiscard]] std::pair<int, QByteArray> restic(const QString &repository, const QString &key, const QStringList &arguments) {
    QProcessEnvironment given = QProcessEnvironment::systemEnvironment();
    given.insert(u"RESTIC_PASSWORD"_s, key);
    QProcess run;
    run.setProcessEnvironment(given);
    run.start(u"restic"_s, QStringList{u"--repo"_s, repository} + arguments);
    run.waitForFinished(120000);
    return {run.exitCode(), run.readAllStandardOutput()};
}

// Whether any file under `folder` holds `text` as it is.
[[nodiscard]] bool anyHolds(const QString &folder, const QByteArray &text) {
    for (QDirIterator each(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); each.hasNext();) {
        QFile file(each.next());
        if (file.open(QIODevice::ReadOnly) && file.readAll().contains(text))
            return true;
    }
    return false;
}

[[nodiscard]] QJsonObject asked(HubSession &session, const QJsonObject &request) {
    return QJsonDocument::fromJson(session.answer(compact(request))).object();
}

} // namespace

class HubDestinationTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty()); }
    void cleanup() { SnapshotDestinations::reachHubsBy({}); }

    // The keeping hub holds the snapshots whole, laid out as restic lays out a
    // disk's, in its stores and in the sending hub's folder there; none of it
    // can be read there without the key, which only the sending hub has.
    void aHubKeepsAnothersSnapshotsItCantRead() {
        Keeping away;
        Sending home;
        const QString started = away.start() + home.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        SnapshotDestinations::reachHubsBy(away.reach());
        SnapshotDestinations destinations = home.destinations();
        const auto key = destinations.add({.name = u"offsite"_s, .location = u"hub:away"_s, .credentials = {}}, {});
        QVERIFY2(key, qPrintable(whyNot(key)));
        QCOMPARE(destinations.find(u"offsite"_s)->kind(), SnapshotDestination::Kind::Hub);
        QCOMPARE(destinations.find(u"offsite"_s)->hub(), u"away"_s);
        const auto copied = destinations.copy(u"offsite"_s, QDateTime::currentDateTime());
        QVERIFY2(copied, qPrintable(whyNot(copied)));
        QVERIFY(copied->done);
        QVERIFY(!copied->latestId.isEmpty());

        const QString kept = away.stores() + u"/home/offsite"_s;
        QVERIFY(QFileInfo::exists(kept + u"/config"_s));
        QVERIFY(!anyHolds(away.stores(), QByteArray(secretTitle.data(), secretTitle.size())));
        QVERIFY(!anyHolds(away.stores(), key->toUtf8()));
        // Restored there with the key the sending hub was shown, the library is whole.
        const auto [listed, snapshots] = restic(kept, *key, {u"snapshots"_s, u"--json"_s});
        QCOMPARE(listed, 0);
        QCOMPARE(QJsonDocument::fromJson(snapshots).array().size(), 1);
        QTemporaryDir restored;
        QCOMPARE(restic(kept, *key, {u"restore"_s, u"latest"_s, u"--target"_s, restored.path()}).first, 0);
        QVERIFY(anyHolds(restored.path(), QByteArray(secretTitle.data(), secretTitle.size())));
        // Without it, nothing.
        QVERIFY(restic(kept, u"not the key"_s, {u"snapshots"_s}).first != 0);

        // A second copy sends only what is new.
        Library library(home.dir.filePath(u"Library"_s));
        BookMeta more;
        more.title = u"Moss"_s;
        QVERIFY(library.open() && library.addEntry(more, true).id > 0);
        QVERIFY(home.snapshots().take(library, SnapshotReason::Asked));
        const auto again = destinations.copy(u"offsite"_s, QDateTime::currentDateTime());
        QVERIFY2(again, qPrintable(whyNot(again)));
        QCOMPARE(QJsonDocument::fromJson(restic(kept, *key, {u"snapshots"_s, u"--json"_s}).second).array().size(), 2);
    }

    // With a second disk, the other hub makes the snapshots backed up (3-2-1):
    // it counts as the copy away from home, and the checks reach it as one.
    void anotherHubCountsAsTheCopyAwayAndIsChecked() {
        Keeping away;
        Sending home;
        const QString started = away.start() + home.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        SnapshotDestinations::reachHubsBy(away.reach());
        SnapshotDestinations destinations = home.destinations();
        QVERIFY(QDir().mkpath(home.dir.filePath(u"usb"_s)));
        QVERIFY(destinations.add({.name = u"usb"_s, .location = home.dir.filePath(u"usb/omabook"_s), .credentials = {}}, {}));
        QVERIFY(destinations.add({.name = u"offsite"_s, .location = u"hub:away"_s, .credentials = {}}, {}));
        QVERIFY(!destinations.backedUp());
        const SnapshotDestinations::Round round = destinations.copyAll(QDateTime::currentDateTime());
        QCOMPARE(round.copied, (QStringList{u"usb"_s, u"offsite"_s}));
        QVERIFY(destinations.backedUp());
        QCOMPARE(destinations.json(*destinations.find(u"offsite"_s)).value("kind"_L1).toString(), u"hub"_s);

        const SnapshotHealth health(home.snapshots());
        health.check(SnapshotHealth::Depth::Light, QDateTime::currentDateTime());
        health.check(SnapshotHealth::Depth::Data, QDateTime::currentDateTime());
        const QList<SnapshotWarnings::Repository> repositories = health.repositories();
        const auto offsite = std::ranges::find(repositories, u"offsite"_s, &SnapshotWarnings::Repository::name);
        QVERIFY(offsite != repositories.end());
        QCOMPARE(offsite->medium, SnapshotWarnings::Medium::Away);
        QVERIFY2(offsite->check.done && offsite->dataCheck.done, qPrintable(offsite->check.said + offsite->dataCheck.said));
    }

    // A hub out of reach is a copy that didn't go through, kept as such, and stops no other destination.
    void aHubOutOfReachStopsNoOtherDestination() {
        Keeping away;
        Sending home;
        const QString started = away.start() + home.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        SnapshotDestinations::reachHubsBy(away.reach());
        SnapshotDestinations destinations = home.destinations();
        QVERIFY(destinations.add({.name = u"offsite"_s, .location = u"hub:away"_s, .credentials = {}}, {}));
        QVERIFY(QDir().mkpath(home.dir.filePath(u"usb"_s)));
        QVERIFY(destinations.add({.name = u"usb"_s, .location = home.dir.filePath(u"usb/omabook"_s), .credentials = {}}, {}));
        away.reachable = false;
        const SnapshotDestinations::Round round = destinations.copyAll(QDateTime::currentDateTime());
        QCOMPARE(round.copied, QStringList{u"usb"_s});
        QCOMPARE(round.failed.size(), 1);
        QCOMPARE(round.failed.first().first, u"offsite"_s);
        QVERIFY(!destinations.stateOf(u"offsite"_s)->done);
        QVERIFY(!destinations.backedUp());
        // Back in reach, it is copied to again.
        away.reachable = true;
        const auto copied = destinations.copy(u"offsite"_s, QDateTime::currentDateTime());
        QVERIFY2(copied, qPrintable(whyNot(copied)));
        // Only looked at, a destination on a hub reaches nothing.
        const int before = away.sessions;
        QVERIFY(destinations.repositoryOf(*destinations.find(u"offsite"_s)));
        QCOMPARE(away.sessions.load(), before);
    }

    // The store answers only for the key's own computer, and only of restic's files.
    void theStoreIsTheKeysComputersAndHoldsOnlyResticsFiles() {
        Keeping away;
        const QString started = away.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const auto home = away.session(u"home"_s);
        const auto greeted = QJsonDocument::fromJson(away.session(u"desktop"_s)->answer(compact(HubSession::hello()))).object();
        QVERIFY(greeted.value("requests").toArray().contains(u"store"_s));
        QVERIFY(asked(*home, {{"request", "store"}, {"repository", "offsite"}, {"op", "create"}}).value("ok").toBool());
        const QString id = QString(u"ab"_s).repeated(32);
        const QJsonObject put{
            {"request", "store"}, {"repository", "offsite"}, {"op", "put"}, {"path", u"snapshots/"_s + id}, {"at", 0}, {"data", "c2VhbGVk"},
            {"last", true}};
        QVERIFY(asked(*home, put).value("ok").toBool());
        QVERIFY(QFileInfo::exists(away.stores() + u"/home/offsite/snapshots/"_s + id));
        // Another computer's store is its own: the desktop sees nothing of home's, whatever the line says.
        const auto desktop = away.session(u"desktop"_s);
        QJsonObject listing{{"request", "store"}, {"repository", "offsite"}, {"op", "list"}, {"type", "snapshots"}, {"computer", "home"}};
        QVERIFY(asked(*desktop, listing).value("files").toArray().isEmpty());
        QCOMPARE(asked(*home, listing).value("files").toArray().size(), 1);
        // Nothing but restic's files, and nowhere but the repository.
        for (const QString &path :
             {u"../desktop/offsite/config"_s, u"/etc/passwd"_s, u"snapshots/../../x"_s, u"data/"_s + id + u"/.."_s, u"plugins/"_s + id})
            QVERIFY2(!asked(*home, {{"request", "store"}, {"repository", "offsite"}, {"op", "get"}, {"path", path}}).value("ok").toBool(),
                     qPrintable(path));
        for (const QString &repository : {u"../desktop"_s, u""_s, u"Off Site"_s})
            QVERIFY(!asked(*home, {{"request", "store"}, {"repository", repository}, {"op", "create"}}).value("ok").toBool());
        QVERIFY(!QFileInfo::exists(away.stores() + u"/desktop/offsite"_s));
        // A data file is kept as on a disk, in the folder of its id's first two digits.
        QJsonObject data = put;
        data.insert("path", u"data/"_s + id);
        QVERIFY(asked(*home, data).value("ok").toBool());
        QVERIFY(QFileInfo::exists(away.stores() + u"/home/offsite/data/ab/"_s + id));
    }
};

QTEST_GUILESS_MAIN(HubDestinationTests)
#include "hubdestination_tests.moc"
