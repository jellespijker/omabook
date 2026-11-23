// A library's device id is its own (#209, ADR 0001): a sync with a library
// of the same device id, or of the id of another computer known here by
// another name, is refused before anything is written on either side, and
// says what to do; a library given a device id of its own then syncs. A
// computer is known by its machine where it says one (#215): renamed, it
// syncs on under its new name; a copy on another machine is refused. Libraries of the test's own, over the
// stand-in ssh, never the user's.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSqlQuery>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "bookmeta.h"
#include "changelog.h"
#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "spaces.h"
#include "sync.h"
#include "syncidentity.h"

using Fixtures::whyNot;

namespace {

[[nodiscard]] qint64 entry(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Ann Author"};
    return library.addEntry(meta, true).id;
}

// What a sync could write of a library: its log, whom it knows and what they
// hold, its global ids, its clock and its books, in one line to compare.
[[nodiscard]] QString written(const Library &library) {
    QStringList counted;
    for (const char *table : {"ops", "seen", "space_members", "member_vectors", "global_ids", "library_state", "books"}) {
        QSqlQuery query = ChangeLog::query(library, QString("SELECT COUNT(*) FROM %1").arg(table));
        counted << QString("%1 %2").arg(table).arg(query.next() ? query.value(0).toLongLong() : -1);
    }
    return counted.join(", ") + ", clock " + QString::number(Spaces::lastHlc(library));
}

// A copy of the library as it stands, in `folder`: its device id with it.
[[nodiscard]] bool copied(const Library &library, const QString &folder) {
    return QDir().mkpath(folder) && library.copyInto(Library::databaseIn(folder)).has_value();
}

// Two computers, "here" and "laptop", each with a library and synced once, so paired.
struct Paired {
    Paired() : here(dir.filePath("Here")), laptop(dir.filePath("Laptop")) {
        QVERIFY(here.open() && laptop.open());
        QVERIFY(entry(here, "Dune") > 0);
        ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        const SyncSummary paired = syncLibrary(here, to("laptop", "Laptop"));
        QVERIFY2(paired.error.isEmpty() && paired.problems.isEmpty(), qPrintable(paired.error + paired.problems.join('\n')));
    }

    // Each command there within two minutes: one that hangs is a sync's problem, said, never a test that waits.
    [[nodiscard]] Remote to(const QString &host, const QString &folder) const {
        return Remote(host + ":" + dir.filePath(folder), {"timeout", "120", ssh});
    }

    QTemporaryDir dir;
    Library here, laptop;
    QString ssh;
};

} // namespace

class SyncIdentityTests : public QObject {
    Q_OBJECT

private slots:
    // Its own id, said by another library: one is a copy of the other.
    void aLibraryOfThisDeviceIdIsRefused() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QJsonObject theirs{{"device", Spaces::deviceId(library)}, {"name", "laptop"}, {"vector", QJsonObject()}};
        const auto checked = SyncIdentity::check(library, theirs, "laptop");
        QVERIFY(!checked);
        QCOMPARE(checked.error().code(), Failure::Code::SyncDeviceOurs);
        QVERIFY2(whyNot(checked).contains("omabook identity --new-device"), qPrintable(whyNot(checked)));
        // An Omabook from before the log says no device: nothing to tell.
        QVERIFY(SyncIdentity::check(library, QJsonObject{{"books", QJsonArray()}}, "laptop"));
    }

    // The id of a computer known here by another name, said by a computer of
    // a third: two libraries speaking as one computer. By its own name, or no
    // name, it is that computer.
    void theIdOfAnotherKnownComputerIsRefused() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        Spaces::pair(library, "personal:desktop-device", "desktop-device", "desktop");
        const auto taken = SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "newbox"}}, "newbox");
        QVERIFY(!taken);
        QCOMPARE(taken.error().code(), Failure::Code::SyncDeviceTaken);
        QVERIFY2(whyNot(taken).contains("desktop") && whyNot(taken).contains("omabook identity --new-device"), qPrintable(whyNot(taken)));
        QVERIFY2(SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "desktop"}}, "desktop"), "the desktop itself");
        QVERIFY2(SyncIdentity::check(library, {{"device", "desktop-device"}}, "desktop"), "a name not said");
        QVERIFY2(SyncIdentity::check(library, {{"device", "another-device"}, {"name", "newbox"}}, "newbox"), "a computer not met yet");
    }

    // A known computer says the machine it runs on (#215): the same machine
    // under another host name is that computer, renamed, and said to be; a
    // machine that says none (a container without a machine id) is known by
    // its name, as is one paired before computers said their machines.
    void aRenamedComputerIsKnownByItsMachine() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString machine = SyncIdentity::machineOf("the desktop machine", "desktop-device");
        QVERIFY(!machine.isEmpty() && !machine.contains("desktop machine"));
        Spaces::pair(library, "personal:desktop-device", "desktop-device", "desktop", machine);
        const auto renamed =
            SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "newbox"}, {"machine", machine}}, "newbox");
        QVERIFY2(renamed, qPrintable(whyNot(renamed)));
        QCOMPARE(renamed->renamedFrom, QString("desktop"));
        const auto same =
            SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "desktop"}, {"machine", machine}}, "desktop");
        QVERIFY2(same && same->renamedFrom.isEmpty(), "not renamed");
        QVERIFY2(!SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "newbox"}}, "newbox"), "no machine said");
        Spaces::pair(library, "personal:older-device", "older-device", "older");
        QVERIFY2(!SyncIdentity::check(library, {{"device", "older-device"}, {"name", "newbox"}, {"machine", machine}}, "newbox"),
                 "paired before machines were said");
    }

    // The machine is salted with the library's device id: one machine says
    // another for each library, and no one machine id in clear (#215).
    void aMachineIsSaidForItsLibraryAlone() {
        const QString one = SyncIdentity::machineOf("the desktop machine", "desktop-device");
        QCOMPARE(SyncIdentity::machineOf("the desktop machine\n", "desktop-device"), one);
        QVERIFY(SyncIdentity::machineOf("the desktop machine", "laptop-device") != one);
        QVERIFY(SyncIdentity::machineOf("another machine", "desktop-device") != one);
        QVERIFY2(SyncIdentity::machineOf("", "desktop-device").isEmpty(), "a machine without an id says none");
    }

    // A copy of another known computer's library on a machine of the same
    // host name (#215): its machine is another, so it is refused as a copy.
    void aCopyOnAnotherMachineOfTheSameNameIsRefused() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        Spaces::pair(library, "personal:desktop-device", "desktop-device", "desktop",
                     SyncIdentity::machineOf("the desktop machine", "desktop-device"));
        const QString another = SyncIdentity::machineOf("another machine", "desktop-device");
        const auto copy =
            SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "desktop"}, {"machine", another}}, "desktop");
        QVERIFY(!copy);
        QCOMPARE(copy.error().code(), Failure::Code::SyncDeviceTaken);
        QVERIFY2(whyNot(copy).contains("omabook identity --new-device"), qPrintable(whyNot(copy)));
        QVERIFY2(SyncIdentity::check(library, {{"device", "desktop-device"}, {"name", "desktop"}}, "desktop"),
                 "a machine not said is known by its name");
    }

    // Both sides of a sync renamed since the last (#215): the sync goes on,
    // each knows the other by the name it has now, and the sync says so.
    void aRenamedComputerSyncsAndIsKnownByItsNewName() {
        Paired two;
        if (SyncIdentity::machine(Spaces::deviceId(two.here)).isEmpty())
            QSKIP("this computer has no machine id");
        const QString host = QSysInfo::machineHostName();
        const auto known = [](const Library &library, const QString &device) {
            for (const Spaces::Computer &computer : Spaces::computers(library))
                if (computer.device == device)
                    return computer;
            return Spaces::Computer{};
        };
        const QString laptop = Spaces::deviceId(two.laptop);
        const QString here = Spaces::deviceId(two.here);
        QCOMPARE(known(two.here, laptop).machine, SyncIdentity::machine(laptop));
        QCOMPARE(known(two.laptop, here).machine, SyncIdentity::machine(here));
        for (Library *library : {&two.here, &two.laptop})
            ChangeLog::exec(*library, "UPDATE space_members SET name = 'oldbox' WHERE person = ?", {library == &two.here ? laptop : here});
        const SyncSummary synced = syncLibrary(two.here, two.to("laptop", "Laptop"));
        QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty(), qPrintable(synced.error + synced.problems.join('\n')));
        QCOMPARE(synced.computersRenamed.size(), 2);
        for (const QString &line : synced.computersRenamed)
            QVERIFY2(line.contains("oldbox") && line.contains(host), qPrintable(line));
        QCOMPARE(known(two.here, laptop).name, host);
        Library there(two.dir.filePath("Laptop"));
        QVERIFY(there.open());
        QCOMPARE(known(there, here).name, host);
    }

    // A device id of its own: what it writes from then on is under it, and it
    // is one of one's own computers wherever the old one was.
    void aLibraryIsGivenADeviceIdOfItsOwn() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(entry(library, "Dune") > 0);
        const QString was = Spaces::deviceId(library);
        const SyncIdentity::Renewed renewed = SyncIdentity::renew(library);
        QCOMPARE(renewed.was, was);
        QVERIFY(!renewed.now.isEmpty() && renewed.now != was);
        QCOMPARE(Spaces::deviceId(library), renewed.now);
        Library reopened(dir.filePath("Library"));
        QVERIFY(reopened.open());
        QCOMPARE(Spaces::deviceId(reopened), renewed.now);
        QVERIFY(Spaces::isOwnComputer(reopened, was));
        QStringList members;
        for (const Spaces::Computer &computer : Spaces::computers(reopened))
            members << computer.device;
        QVERIFY2(members.contains(renewed.now) && members.contains(was), qPrintable(members.join(", ")));
        QVERIFY(entry(reopened, "Emma") > 0);
        QSqlQuery mine = ChangeLog::query(reopened, "SELECT COUNT(*) FROM ops AS o JOIN log_ids AS d ON d.id = o.device WHERE d.name = ?",
                                          {renewed.now});
        QVERIFY(mine.next() && mine.value(0).toLongLong() > 0);
    }

    // A copy of this library, synced with: refused, nothing written on either
    // side, and the message says what to do. Given an id of its own, by the
    // command, the copy syncs.
    void aCopyIsRefusedBeforeAnythingIsWrittenUntilItHasAnIdOfItsOwn() {
        QTemporaryDir dir;
        Library here(dir.filePath("Here"));
        QVERIFY(here.open());
        QVERIFY(entry(here, "Dune") > 0);
        QVERIFY(copied(here, dir.filePath("Copy")));
        const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        const Remote copy("copy:" + dir.filePath("Copy"), {"timeout", "120", ssh});
        const QString hereBefore = written(here);
        QString copyBefore;
        {
            Library there(dir.filePath("Copy"));
            QVERIFY(there.open());
            copyBefore = written(there);
        }
        for (const bool dryRun : {true, false}) {
            const SyncSummary refused = syncLibrary(here, copy, {}, {.dryRun = dryRun});
            QVERIFY2(refused.error.contains("device id") && refused.error.contains("omabook identity --new-device"),
                     qPrintable(refused.error));
            QCOMPARE(written(here), hereBefore);
            Library there(dir.filePath("Copy"));
            QVERIFY(there.open());
            QCOMPARE(written(there), copyBefore);
        }

        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", dir.filePath("home"));
        environment.insert("XDG_CONFIG_HOME", dir.filePath("home/.config"));
        environment.insert("XDG_DATA_HOME", dir.filePath("home/.local/share"));
        environment.insert("XDG_STATE_HOME", dir.filePath("home/.local/state"));
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        QProcess renew;
        renew.setProcessEnvironment(environment);
        renew.start(OMABOOK_BINARY, {"identity", "--new-device", "--library", dir.filePath("Copy")});
        QVERIFY(renew.waitForFinished(60000));
        const QString said = QString::fromUtf8(renew.readAllStandardOutput() + renew.readAllStandardError());
        QVERIFY2(renew.exitCode() == 0 && said.contains("device id"), qPrintable(said));

        const SyncSummary synced = syncLibrary(here, copy);
        QVERIFY2(synced.error.isEmpty() && synced.problems.isEmpty(), qPrintable(synced.error + synced.problems.join('\n')));
    }

    // Asked by the computer that starts a sync, the other side checks it too,
    // before anything is written there: the laptop's device id, said by a
    // computer of another name, is refused; by the laptop's own, it is not.
    void theOtherSideRefusesTheIdOfAnotherKnownComputer() {
        Paired two;
        const QString laptop = Spaces::deviceId(two.laptop);
        const auto asked = [&two](const QJsonObject &theirs) {
            const QString file = two.dir.filePath("identity.json");
            if (!Fixtures::write(file, QJsonDocument(theirs).toJson()))
                return QJsonObject{{"written", false}};
            QProcess process;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("HOME", two.dir.filePath("there-home"));
            environment.insert("XDG_CONFIG_HOME", two.dir.filePath("there-home/.config"));
            environment.insert("XDG_DATA_HOME", two.dir.filePath("there-home/.local/share"));
            environment.insert("XDG_STATE_HOME", two.dir.filePath("there-home/.local/state"));
            environment.insert("QT_QPA_PLATFORM", "offscreen");
            process.setProcessEnvironment(environment);
            process.start(OMABOOK_BINARY, {"sync", "--identity", file, "--library", two.dir.filePath("Here")});
            process.waitForFinished(60000);
            return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        };
        const QString hereBefore = written(two.here);
        const QJsonObject refused = asked({{"device", laptop}, {"name", "newbox"}});
        QVERIFY2(refused["refused"].toString().contains("omabook identity --new-device"), qPrintable(QJsonDocument(refused).toJson()));
        QCOMPARE(written(two.here), hereBefore);
        const QJsonObject welcome = asked({{"device", laptop}, {"name", QSysInfo::machineHostName()}});
        QVERIFY2(welcome.isEmpty(), qPrintable(QJsonDocument(welcome).toJson()));
    }
};

QTEST_GUILESS_MAIN(SyncIdentityTests)
#include "syncidentity_tests.moc"
