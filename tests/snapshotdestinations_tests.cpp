// The destinations of a computer's snapshots (#182, ADR 0014): a second disk
// (a folder of the test's own) and one away from home (restic's rest: backend,
// served by FakeResticServer, since rest-server isn't in the build image),
// each its own repository with its own password, shown once, copied to with
// `restic copy` and thinned as the computer's repository is; a disk that isn't
// there is noticed, an unreachable one doesn't stop the others, credentials
// are secrets and never in the choices, and the hub's daemon copies at night.
// In folders of the test's own, never the user's library or home.

#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>
#include <utility>

#include "bookmeta.h"
#include "fakeresticserver.h"
#include "fixtures.h"
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubsnapshots.h"
#include "library.h"
#include "snapshotdestinations.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;

namespace {

// A computer of the test's own: its snapshots, set up, of a library with a book.
struct Computer {
    QTemporaryDir dir;
    QString library = dir.filePath(u"Library"_s);
    QString state = dir.filePath(u"state/omabook/snapshots"_s);
    QString repository = dir.filePath(u"repository"_s);
    QString key;
    SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};

    [[nodiscard]] Snapshots snapshots() const { return Snapshots(SnapshotPlace::in(state), contents); }
    [[nodiscard]] SnapshotDestinations destinations() const { return SnapshotDestinations(SnapshotPlace::in(state)); }

    [[nodiscard]] bool setUp() {
        Snapshots made = snapshots();
        const auto given = made.setUp(repository);
        key = given.value_or(QString());
        return given.has_value();
    }

    // A snapshot of the library with a book called `title` added first.
    [[nodiscard]] QString snapshotWith(const QString &title) const {
        Library opened(library);
        BookMeta meta;
        meta.title = title;
        if (!opened.open() || opened.addEntry(meta, true).id <= 0)
            return {};
        const auto taken = snapshots().take(opened, SnapshotReason::Asked);
        return taken ? taken->id : QString();
    }
};

[[nodiscard]] QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

[[nodiscard]] QStringList titlesOf(const QString &folder) {
    Library library(folder);
    library.keepFormat();
    if (!library.open())
        return {u"(not opened)"_s};
    QStringList titles;
    for (const Book &book : library.books())
        titles << book.meta.title;
    titles.sort();
    return titles;
}

// restic on `location` with the password `key` alone, and what else goes in its environment.
struct Raw {
    QString location, key;
    QList<std::pair<QString, QString>> environment;

    [[nodiscard]] std::pair<int, QByteArray> run(const QStringList &arguments) const {
        QProcessEnvironment given = QProcessEnvironment::systemEnvironment();
        given.insert(u"RESTIC_PASSWORD"_s, key);
        for (const auto &[name, value] : environment)
            given.insert(name, value);
        QProcess restic;
        restic.setProcessEnvironment(given);
        restic.start(u"restic"_s, QStringList{u"--repo"_s, location} + arguments);
        restic.waitForFinished(120000);
        return {restic.exitCode(), restic.readAllStandardOutput()};
    }
    [[nodiscard]] QJsonArray snapshots() const { return QJsonDocument::fromJson(run({u"snapshots"_s, u"--json"_s}).second).array(); }
    // The titles of the library in snapshot `id`, restored from here into `target`.
    [[nodiscard]] QStringList titlesIn(const QString &id, const QString &target) const {
        if (run({u"restore"_s, id, u"--target"_s, target}).first != 0)
            return {u"(not restored)"_s};
        QString database;
        for (QDirIterator found(target, {u"omabook.db"_s}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); found.hasNext();)
            database = found.next();
        const QString folder = target + u"-library"_s;
        if (database.isEmpty() || !QDir().mkpath(folder) || !QFile::copy(database, Library::databaseIn(folder)))
            return {u"(no database)"_s};
        return titlesOf(folder);
    }
};

[[nodiscard]] QStringList idsOf(const QJsonArray &snapshots, const char *field = "id") {
    QStringList ids;
    for (const QJsonValue snapshot : snapshots)
        ids << snapshot.toObject().value(QLatin1StringView(field)).toString();
    ids.sort();
    return ids;
}

const QList<std::pair<QString, QByteArray>> restCredentials{{u"RESTIC_REST_USERNAME"_s, "reader"},
                                                            {u"RESTIC_REST_PASSWORD"_s, "open sesame"}};

} // namespace

class SnapshotDestinationsTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(!QStandardPaths::findExecutable(u"restic"_s).isEmpty()); }

    void aSecondDiskGetsTheSnapshotsUnderAPasswordOfItsOwn() {
        Computer computer;
        QVERIFY(computer.setUp());
        const QString first = computer.snapshotWith(u"First"_s);
        QVERIFY(!first.isEmpty());
        const QString disk = computer.dir.filePath(u"usb/omabook"_s);
        QVERIFY(QDir().mkpath(computer.dir.filePath(u"usb"_s)));
        SnapshotDestinations destinations = computer.destinations();
        const auto key = destinations.add({.name = u"usb"_s, .location = disk, .credentials = {}}, {});
        QVERIFY2(key, qPrintable(Fixtures::whyNot(key)));
        QVERIFY(!key->isEmpty() && *key != computer.key);
        QCOMPARE(destinations.find(u"usb"_s)->kind(), SnapshotDestination::Kind::Disk);
        // Its password is a secret, 0600, and in the choices nowhere.
        const QString secret = computer.state + u"/secrets/"_s + SnapshotDestinations::passwordSecret(u"usb"_s);
        QCOMPARE(QFile::permissions(secret) & ~(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser),
                 QFile::Permissions());
        QVERIFY(!readFile(computer.state + u"/destinations.json"_s).contains(key->toUtf8()));

        const auto copied = destinations.copy(u"usb"_s, QDateTime::currentDateTime());
        QVERIFY2(copied, qPrintable(Fixtures::whyNot(copied)));
        QVERIFY(copied->done && copied->size > 0);
        const Raw there{.location = disk, .key = *key, .environment = {}};
        QCOMPARE(idsOf(there.snapshots(), "original"), QStringList{first});
        QCOMPARE(there.titlesIn(idsOf(there.snapshots()).value(0), computer.dir.filePath(u"restored"_s)), QStringList{u"First"_s});
        // The computer's password doesn't open it.
        QVERIFY((Raw{.location = disk, .key = computer.key, .environment = {}}.run({u"snapshots"_s}).first != 0));

        // A second copy takes only the new one; the state says what it holds.
        const QString second = computer.snapshotWith(u"Second"_s);
        QVERIFY(destinations.copy(u"usb"_s, QDateTime::currentDateTime()));
        QStringList both{first, second};
        both.sort();
        QCOMPARE(idsOf(there.snapshots(), "original"), both);
        const auto state = destinations.stateOf(u"usb"_s);
        QVERIFY(state && state->done && state->copied.isValid());
        QVERIFY(idsOf(there.snapshots()).contains(state->latestId) && state->latest.isValid());
        const auto tested = destinations.test(u"usb"_s);
        QVERIFY2(tested, qPrintable(Fixtures::whyNot(tested)));
        QCOMPARE(tested->snapshots, 2);
    }

    void aDiskThatIsntThereIsNoticed() {
        Computer computer;
        QVERIFY(computer.setUp());
        QVERIFY(!computer.snapshotWith(u"First"_s).isEmpty());
        const QString mount = computer.dir.filePath(u"usb"_s);
        QVERIFY(QDir().mkpath(mount));
        SnapshotDestinations destinations = computer.destinations();
        QVERIFY(destinations.add({.name = u"usb"_s, .location = mount + u"/omabook"_s, .credentials = {}}, {}));
        // Unplugged: the mount point is there, empty.
        QVERIFY(QDir().rename(mount + u"/omabook"_s, computer.dir.filePath(u"elsewhere"_s)));
        const auto copied = destinations.copy(u"usb"_s, QDateTime::currentDateTime());
        QVERIFY(!copied);
        QCOMPARE(copied.error().code(), Failure::Code::DestinationAbsent);
        QVERIFY(!QFileInfo::exists(mount + u"/omabook"_s)); // nothing made in its place
        const auto state = destinations.stateOf(u"usb"_s);
        QVERIFY(state && !state->done && state->absent && state->tried.isValid());
        QCOMPARE(destinations.test(u"usb"_s).error().code(), Failure::Code::DestinationAbsent);
        // Plugged in again, it gets them.
        QVERIFY(QDir().rename(computer.dir.filePath(u"elsewhere"_s), mount + u"/omabook"_s));
        QVERIFY(destinations.copy(u"usb"_s, QDateTime::currentDateTime()));
        QVERIFY(!destinations.stateOf(u"usb"_s)->absent);
        // A disk that isn't there can't be added either.
        QCOMPARE(destinations.add({.name = u"gone"_s, .location = computer.dir.filePath(u"no-disk/omabook"_s), .credentials = {}}, {})
                     .error()
                     .code(),
                 Failure::Code::DestinationAbsent);
    }

    void oneAwayFromHomeHasItsCredentialsKeptAsSecrets() {
        Computer computer;
        QVERIFY(computer.setUp());
        const QString first = computer.snapshotWith(u"First"_s);
        const FakeResticServer server(computer.dir.filePath(u"away"_s), "reader", "open sesame");
        SnapshotDestinations destinations = computer.destinations();
        const SnapshotDestination away{
            .name = u"away"_s, .location = server.location(), .credentials = {u"RESTIC_REST_USERNAME"_s, u"RESTIC_REST_PASSWORD"_s}};
        const auto key = destinations.add(away, restCredentials);
        QVERIFY2(key, qPrintable(Fixtures::whyNot(key)));
        QCOMPARE(destinations.find(u"away"_s)->kind(), SnapshotDestination::Kind::Away);
        QVERIFY(server.answered() > 0);
        const QByteArray choices = readFile(computer.state + u"/destinations.json"_s);
        QVERIFY(choices.contains("RESTIC_REST_PASSWORD") && !choices.contains("open sesame") && !choices.contains(key->toUtf8()));
        QVERIFY(QFileInfo::exists(computer.state + u"/secrets/"_s + SnapshotDestinations::credentialsSecret(u"away"_s)));

        const auto copied = destinations.copy(u"away"_s, QDateTime::currentDateTime());
        QVERIFY2(copied, qPrintable(Fixtures::whyNot(copied)));
        const Raw there{.location = server.location(),
                        .key = *key,
                        .environment = {{u"RESTIC_REST_USERNAME"_s, u"reader"_s}, {u"RESTIC_REST_PASSWORD"_s, u"open sesame"_s}}};
        QCOMPARE(idsOf(there.snapshots(), "original"), QStringList{first});
        QCOMPARE(there.titlesIn(idsOf(there.snapshots()).value(0), computer.dir.filePath(u"restored"_s)), QStringList{u"First"_s});
        // Without them it is refused.
        QVERIFY((Raw{.location = server.location(), .key = *key, .environment = {}}.run({u"snapshots"_s}).first != 0));
        QVERIFY(server.refused() > 0);
    }

    void whatTheChoicesWouldHoldInTheClearIsRefused() {
        Computer computer;
        QVERIFY(computer.setUp());
        SnapshotDestinations destinations = computer.destinations();
        const auto code = [&destinations](const SnapshotDestination &destination) {
            const auto added = destinations.add(destination, {});
            return added ? Failure::Code::Said : added.error().code();
        };
        QCOMPARE(code({.name = u"away"_s, .location = u"rest:https://reader:open-sesame@example.org/"_s, .credentials = {}}),
                 Failure::Code::DestinationPasswordInPlace);
        QCOMPARE(code({.name = u"Not A Name"_s, .location = computer.dir.filePath(u"x"_s), .credentials = {}}),
                 Failure::Code::DestinationNameInvalid);
        QCOMPARE(code({.name = u"same"_s, .location = computer.repository, .credentials = {}}), Failure::Code::DestinationIsTheRepository);
        // A credential named, with no value given.
        QCOMPARE(code({.name = u"s3"_s, .location = u"s3:https://example.org/bucket"_s, .credentials = {u"AWS_SECRET_ACCESS_KEY"_s}}),
                 Failure::Code::DestinationCredentialMissing);
        QVERIFY(destinations.add({.name = u"usb"_s, .location = computer.dir.filePath(u"usb"_s), .credentials = {}}, {}));
        QCOMPARE(code({.name = u"usb"_s, .location = computer.dir.filePath(u"usb2"_s), .credentials = {}}),
                 Failure::Code::DestinationNameTaken);
        QCOMPARE(destinations.list().size(), 1);
    }

    void anUnreachableOneLetsTheOthersGoOn() {
        Computer computer;
        QVERIFY(computer.setUp());
        QVERIFY(!computer.snapshotWith(u"First"_s).isEmpty());
        SnapshotDestinations destinations = computer.destinations();
        QString awayLocation;
        {
            const FakeResticServer server(computer.dir.filePath(u"away"_s), "reader", "open sesame");
            awayLocation = server.location();
            QVERIFY(destinations.add(
                {.name = u"away"_s, .location = awayLocation, .credentials = {restCredentials.at(0).first, restCredentials.at(1).first}},
                restCredentials));
        } // and now it is gone
        QVERIFY(destinations.add({.name = u"usb"_s, .location = computer.dir.filePath(u"usb"_s), .credentials = {}}, {}));
        QVERIFY(!destinations.backedUp());
        const SnapshotDestinations::Round round = destinations.copyAll(QDateTime::currentDateTime());
        QCOMPARE(round.copied, QStringList{u"usb"_s});
        QCOMPARE(round.failed.size(), 1);
        QCOMPARE(round.failed.at(0).first, u"away"_s);
        QVERIFY(!destinations.stateOf(u"away"_s)->done && !destinations.stateOf(u"away"_s)->said.isEmpty());
        QVERIFY(destinations.stateOf(u"usb"_s)->done);
        QVERIFY(!destinations.backedUp()); // nothing away went through
    }

    void threeCopiesTwoMediaOneAwayIsBackedUp() {
        Computer computer;
        QVERIFY(computer.setUp());
        QVERIFY(!computer.snapshotWith(u"First"_s).isEmpty());
        const FakeResticServer server(computer.dir.filePath(u"away"_s), {}, {});
        SnapshotDestinations destinations = computer.destinations();
        QVERIFY(destinations.add({.name = u"usb"_s, .location = computer.dir.filePath(u"usb"_s), .credentials = {}}, {}));
        QVERIFY(destinations.copyAll(QDateTime::currentDateTime()).failed.isEmpty());
        QVERIFY(!destinations.backedUp()); // two copies, none away
        QVERIFY(destinations.add({.name = u"away"_s, .location = server.location(), .credentials = {}}, {}));
        QVERIFY(destinations.copyAll(QDateTime::currentDateTime()).failed.isEmpty());
        QVERIFY(destinations.backedUp());
    }

    void aDestinationIsThinnedAsTheComputersRepositoryIs() {
        Computer computer;
        QVERIFY(computer.setUp());
        const QString recent = computer.snapshotWith(u"First"_s);
        Library opened(computer.library);
        QVERIFY(opened.open());
        const QString tag = Snapshots::libraryTag(opened);
        const Raw here{.location = computer.repository, .key = computer.key, .environment = {}};
        // Moment snapshots of the library from a day long ago: the newest of that day is kept, the rest not.
        const auto longAgo = [&](const QString &time) {
            return here.run({u"backup"_s, u"--json"_s, u"--time"_s, u"2024-01-01 "_s + time, u"--tag"_s, u"omabook,"_s + tag + u",moment"_s,
                             computer.library});
        };
        QCOMPARE(longAgo(u"10:00:00"_s).first, 0);
        QCOMPARE(longAgo(u"12:00:00"_s).first, 0);
        SnapshotDestinations destinations = computer.destinations();
        const QString disk = computer.dir.filePath(u"usb"_s);
        const auto key = destinations.add({.name = u"usb"_s, .location = disk, .credentials = {}}, {});
        QVERIFY(key);
        const QDateTime now = QDateTime::currentDateTime();
        const auto thinned = computer.snapshots().thin(tag, now);
        QVERIFY(thinned && *thinned == 1);
        QVERIFY(destinations.copy(u"usb"_s, now));
        const Raw there{.location = disk, .key = *key, .environment = {}};
        QCOMPARE(there.snapshots().size(), 2);
        // One copied there before it was thinned here is forgotten there all the same.
        QCOMPARE(longAgo(u"11:00:00"_s).first, 0);
        QVERIFY(destinations.copy(u"usb"_s, now));
        QCOMPARE(here.snapshots().size(), 3);
        QCOMPARE(there.snapshots().size(), 2);
        QVERIFY(idsOf(there.snapshots(), "original").contains(recent));
    }

    void removingForgetsItsSecretsButNotWhatIsKeptThere() {
        Computer computer;
        QVERIFY(computer.setUp());
        const FakeResticServer server(computer.dir.filePath(u"away"_s), "reader", "open sesame");
        SnapshotDestinations destinations = computer.destinations();
        QVERIFY(destinations.add(
            {.name = u"away"_s, .location = server.location(), .credentials = {restCredentials.at(0).first, restCredentials.at(1).first}},
            restCredentials));
        QVERIFY(destinations.remove(u"away"_s));
        QVERIFY(destinations.list().isEmpty());
        QVERIFY(!destinations.stateOf(u"away"_s));
        QVERIFY(!QFileInfo::exists(computer.state + u"/secrets/"_s + SnapshotDestinations::passwordSecret(u"away"_s)));
        QVERIFY(!QFileInfo::exists(computer.state + u"/secrets/"_s + SnapshotDestinations::credentialsSecret(u"away"_s)));
        QVERIFY(QFileInfo::exists(computer.dir.filePath(u"away/config"_s)));
        QCOMPARE(destinations.remove(u"away"_s).error().code(), Failure::Code::DestinationUnknown);
    }

    void theHubCopiesAtNightWithItsOwnSecrets() {
        QTemporaryDir dir;
        const HubPlace place{{.state = dir.filePath(u"state/omabook-hub"_s), .runtime = dir.filePath(u"runtime"_s), .credentials = {}}};
        const QString libraries = dir.filePath(u"libraries"_s);
        {
            Library library(libraries + u"/home"_s);
            BookMeta meta;
            meta.title = u"Kept"_s;
            QVERIFY(library.open() && library.addEntry(meta, true).id > 0);
        }
        const SnapshotContents contents{.settingsFile = dir.filePath(u"config.json"_s), .pluginsFolder = dir.filePath(u"plugins"_s)};
        Snapshots made(SnapshotPlace::ofHub(place), contents);
        QVERIFY(made.setUp(dir.filePath(u"repository"_s)));
        SnapshotDestinations added(SnapshotPlace::ofHub(place));
        const auto key = added.add({.name = u"usb"_s, .location = dir.filePath(u"usb"_s), .credentials = {}}, {});
        QVERIFY2(key, qPrintable(Fixtures::whyNot(key)));
        QCOMPARE(QString::fromLatin1(place.secret(SnapshotDestinations::passwordSecret(u"usb"_s)).value_or(QByteArray())), *key);
        QVERIFY(QDir().mkpath(dir.filePath(u"usb2"_s)));
        QVERIFY(added.add({.name = u"usb2"_s, .location = dir.filePath(u"usb2/omabook"_s), .credentials = {}}, {}));

        // As the daemon starts: both read again from the hub's state.
        const Snapshots snapshots(SnapshotPlace::ofHub(place), contents);
        const SnapshotDestinations destinations(SnapshotPlace::ofHub(place));
        HubKeeper keeper(libraries);
        QVERIFY(keeper.open());
        QVERIFY(HubSnapshots::nightly(snapshots)(keeper));
        QVERIFY(HubSnapshots::destinations(destinations)(keeper));
        const Raw usb{.location = dir.filePath(u"usb"_s), .key = *key, .environment = {}};
        QCOMPARE(usb.snapshots().size(), 1);
        // One unplugged: the job says so, to be tried again, and the other is copied all the same.
        QVERIFY(QDir(dir.filePath(u"usb2/omabook"_s)).removeRecursively());
        QVERIFY(HubSnapshots::nightly(snapshots)(keeper));
        const auto night = HubSnapshots::destinations(destinations)(keeper);
        QVERIFY(!night);
        QCOMPARE(night.error().code(), Failure::Code::DestinationsNotCopied);
        QVERIFY(night.error().text().contains(u"usb2"_s));
        QCOMPARE(usb.snapshots().size(), 2);
    }

    void theCommandLineAddsListsTestsAndRemoves() {
        QTemporaryDir dir;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const auto &[name, folder] : {std::pair{"HOME", "home"},
                                           {"XDG_CONFIG_HOME", "home/.config"},
                                           {"XDG_CACHE_HOME", "home/.cache"},
                                           {"XDG_DATA_HOME", "home/.local/share"},
                                           {"XDG_STATE_HOME", "home/.local/state"}})
            environment.insert(QString::fromLatin1(name), dir.filePath(QString::fromLatin1(folder)));
        environment.insert(u"QT_QPA_PLATFORM"_s, u"offscreen"_s);
        environment.remove(u"CREDENTIALS_DIRECTORY"_s);
        QVERIFY(QDir().mkpath(dir.filePath(u"home/.config/omabook"_s)));
        QVERIFY(Fixtures::write(dir.filePath(u"home/.config/omabook/config.json"_s), "{\"ai_provider\": \"off\"}"));
        const auto omabook = [&environment](const QStringList &arguments, const QByteArray &typed = {}) {
            QProcess process;
            process.setProcessEnvironment(environment);
            process.start(QStringLiteral(OMABOOK_BINARY), arguments);
            process.write(typed);
            process.closeWriteChannel();
            process.waitForFinished(120000);
            return std::pair{process.exitCode(), QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
        };
        const QString library = dir.filePath(u"Library"_s);
        QCOMPARE(omabook({u"snapshot"_s, u"setup"_s, u"--repository"_s, dir.filePath(u"repository"_s)}).first, 0);
        {
            Library made(library);
            QVERIFY(made.open());
        }
        QCOMPARE(omabook({u"snapshot"_s, u"now"_s, u"--library"_s, library}).first, 0);

        const QString disk = dir.filePath(u"usb/omabook"_s);
        QVERIFY(QDir().mkpath(dir.filePath(u"usb"_s)));
        const auto [added, addSaid] = omabook({u"snapshot"_s, u"destination"_s, u"add"_s, u"usb"_s, disk, u"--json"_s});
        QVERIFY2(added == 0, qPrintable(addSaid));
        const QJsonObject made = QJsonDocument::fromJson(addSaid.toUtf8()).object();
        QVERIFY2(!made.value("recovery_key"_L1).toString().isEmpty(), qPrintable(addSaid));
        QCOMPARE(made.value("kind"_L1).toString(), u"disk"_s);

        // One away, its password typed rather than in the environment or on the command line.
        const FakeResticServer server(dir.filePath(u"away"_s), "reader", "open sesame");
        environment.insert(u"RESTIC_REST_USERNAME"_s, u"reader"_s);
        const auto [away, awaySaid] = omabook({u"snapshot"_s, u"destination"_s, u"add"_s, u"away"_s, server.location(), u"--credential"_s,
                                               u"RESTIC_REST_USERNAME"_s, u"--credential"_s, u"RESTIC_REST_PASSWORD"_s},
                                              "open sesame\n");
        QVERIFY2(away == 0, qPrintable(awaySaid));
        QVERIFY2(awaySaid.contains(u"recovery key"_s), qPrintable(awaySaid));
        environment.remove(u"RESTIC_REST_USERNAME"_s);
        QVERIFY(!readFile(dir.filePath(u"home/.local/state/omabook/snapshots/destinations.json"_s)).contains("open sesame"));

        const auto [copied, copySaid] = omabook({u"snapshot"_s, u"destination"_s, u"copy"_s});
        QVERIFY2(copied == 0, qPrintable(copySaid));
        const auto [tested, testSaid] = omabook({u"snapshot"_s, u"destination"_s, u"test"_s, u"away"_s});
        QVERIFY2(tested == 0 && testSaid.contains(u"1 snapshot"_s), qPrintable(testSaid));

        const auto [listed, list] = omabook({u"snapshot"_s, u"destination"_s, u"list"_s, u"--json"_s});
        QVERIFY2(listed == 0, qPrintable(list));
        const QJsonObject json = QJsonDocument::fromJson(list.toUtf8()).object();
        QVERIFY2(json.value("backed_up"_L1).toBool(), qPrintable(list));
        const QJsonArray destinations = json.value("destinations"_L1).toArray();
        QCOMPARE(destinations.size(), 2);
        QCOMPARE(destinations.at(0).toObject().value("name"_L1).toString(), u"usb"_s);
        QVERIFY(destinations.at(0).toObject().value("copied"_L1).toBool());

        // Unplugged, the copy says so and fails; the list shows it.
        QVERIFY(QDir().rename(disk, dir.filePath(u"elsewhere"_s)));
        const auto [absent, absentSaid] = omabook({u"snapshot"_s, u"destination"_s, u"copy"_s});
        QVERIFY2(absent != 0 && absentSaid.contains(u"usb"_s) && absentSaid.contains(u"plugged in"_s), qPrintable(absentSaid));
        const auto [listedAgain, listAgain] = omabook({u"snapshot"_s, u"destination"_s, u"list"_s});
        QVERIFY2(listedAgain == 0 && listAgain.contains(u"isn't there"_s), qPrintable(listAgain));

        const auto [removed, removeSaid] = omabook({u"snapshot"_s, u"destination"_s, u"remove"_s, u"usb"_s});
        QVERIFY2(removed == 0, qPrintable(removeSaid));
        const auto [last, lastList] = omabook({u"snapshot"_s, u"destination"_s, u"list"_s, u"--json"_s});
        QCOMPARE(QJsonDocument::fromJson(lastList.toUtf8()).object().value("destinations"_L1).toArray().size(), 1);
    }
};

QTEST_MAIN(SnapshotDestinationsTests)
#include "snapshotdestinations_tests.moc"
