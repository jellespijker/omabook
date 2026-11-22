// The hub daemon (#179): an Omabook without a window, the one writer to the
// libraries it keeps (ADR 0013). Two sessions that write at once take turns,
// and both writes are kept; a program that isn't the daemon is refused the
// libraries it keeps, while the daemon runs and not after; a daemon killed
// comes back and takes over what it left (its socket, its hold on the
// libraries); a session takes only the hub's own requests; what comes before a
// write that changes much (#181's moment snapshot) runs inside its lock and
// can refuse it; the hub's jobs run when they are due, missed ones at once;
// its secrets come from systemd's credentials or a file only it may read; and
// its systemd units start it again whenever it stops.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <chrono>
#include <memory>

#include "fixtures.h"
#include "hubdaemon.h"
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubrequests.h"
#include "hubschedule.h"
#include "library.h"
#include "libraryhold.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// A folder of libraries for a hub, with one library in it called `name`, made
// and closed again before the hub holds it.
[[nodiscard]] bool hubFolder(const QString &folder, const QString &name = u"home"_s) {
    Library library(QDir(folder).filePath(name));
    return library.open();
}

// A program's own home, so that no test touches the person's.
struct Home {
    QTemporaryDir dir;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();

    Home() {
        for (const char *folder : {"config", "data", "cache", "state", "runtime"})
            QDir().mkpath(dir.filePath(QString::fromLatin1(folder)));
        environment.insert(u"HOME"_s, dir.path());
        environment.insert(u"XDG_CONFIG_HOME"_s, dir.filePath(u"config"_s));
        environment.insert(u"XDG_DATA_HOME"_s, dir.filePath(u"data"_s));
        environment.insert(u"XDG_CACHE_HOME"_s, dir.filePath(u"cache"_s));
        environment.insert(u"XDG_STATE_HOME"_s, dir.filePath(u"state"_s));
        environment.insert(u"XDG_RUNTIME_DIR"_s, dir.filePath(u"runtime"_s));
        environment.insert(u"DBUS_SESSION_BUS_ADDRESS"_s, u"unix:path="_s + dir.filePath(u"no-bus"_s));
        for (const char *systemd : {"STATE_DIRECTORY", "RUNTIME_DIRECTORY", "CREDENTIALS_DIRECTORY"})
            environment.remove(QString::fromLatin1(systemd));
    }

    // The hub's place as `omabook hub` finds it in this home.
    [[nodiscard]] HubPlace place() const {
        return HubPlace(
            {.state = dir.filePath(u"state/omabook-hub"_s), .runtime = dir.filePath(u"runtime/omabook-hub"_s), .credentials = {}});
    }

    void start(QProcess &omabook, const QStringList &arguments) const {
        omabook.setProcessEnvironment(environment);
        omabook.start(QString::fromLatin1(OMABOOK_BINARY), arguments);
    }

    // Runs omabook with `input` on its standard input, waiting for it in this
    // event loop (an in-process daemon answers in it); its exit code and output.
    [[nodiscard]] std::pair<int, QString> run(const QStringList &arguments, const QByteArray &input = {}) const {
        QProcess omabook;
        start(omabook, arguments);
        omabook.write(input);
        omabook.closeWriteChannel();
        if (!QTest::qWaitFor([&] { return omabook.state() == QProcess::NotRunning; }, 30000))
            return {-1, u"still running"_s};
        return {omabook.exitCode(), QString::fromUtf8(omabook.readAllStandardOutput() + omabook.readAllStandardError())};
    }
};

// The answers a session printed, one a line.
[[nodiscard]] QList<QJsonObject> answers(const QString &printed) {
    QList<QJsonObject> objects;
    for (const QString &line : printed.split('\n', Qt::SkipEmptyParts))
        objects << QJsonDocument::fromJson(line.toUtf8()).object();
    return objects;
}

// A daemon run by this test, on its own thread's event loop: the test's.
struct InProcess {
    Home home;
    HubKeeper keeper{home.dir.filePath(u"libraries"_s)};
    HubRequests requests = HubRequests::usual();
    HubDaemon daemon{keeper, requests};

    [[nodiscard]] QString start() {
        if (!hubFolder(keeper.folder()))
            return u"no library made"_s;
        if (const auto opened = keeper.open(); !opened)
            return whyNot(opened);
        return whyNot(daemon.listen(home.place().socket()));
    }
};

} // namespace

class HubTests : public QObject {
    Q_OBJECT

private slots:
    void twoSessionsWritingAtOnceTakeTurns() {
        InProcess hub;
        // A write that reads first and writes later, slowly: two of them that
        // met would both read no books, and both add "Book 1".
        bool inside = false;
        bool met = false;
        hub.requests.add(u"shelve"_s, [&inside, &met](const QJsonObject &, HubKeeper &keeper) {
            return keeper.write({}, HubKeeper::Moment::None, [&inside, &met](Library &library) -> std::expected<QJsonObject, Failure> {
                met = met || inside;
                inside = true;
                const auto before = library.books().size();
                QThread::msleep(300);
                const auto added = library.addEntry({.title = u"Book %1"_s.arg(before + 1)}, true);
                inside = false;
                return QJsonObject{{"book", added.id}};
            });
        });
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QProcess first;
        QProcess second;
        hub.home.start(first, {u"hub"_s, u"session"_s});
        hub.home.start(second, {u"hub"_s, u"session"_s});
        for (QProcess *session : {&first, &second}) {
            session->write(R"({"request": "shelve"})"
                           "\n");
            session->closeWriteChannel();
        }
        QVERIFY(QTest::qWaitFor([&] { return first.state() == QProcess::NotRunning && second.state() == QProcess::NotRunning; }, 30000));
        for (QProcess *session : {&first, &second}) {
            const QString said = QString::fromUtf8(session->readAllStandardOutput());
            QCOMPARE(session->exitCode(), 0);
            QVERIFY2(answers(said).value(0).value("ok").toBool(), qPrintable(said));
        }
        QVERIFY(!met);
        QStringList titles;
        std::ignore = hub.keeper.write({}, HubKeeper::Moment::None, [&titles](Library &library) -> std::expected<QJsonObject, Failure> {
            for (const Book &book : library.books())
                titles << book.meta.title;
            return QJsonObject{};
        });
        titles.sort();
        QCOMPARE(titles, (QStringList{u"Book 1"_s, u"Book 2"_s}));
    }

    void aWriteFromOutsideTheDaemonIsRefused() {
        const Home home;
        const QString folder = home.dir.filePath(u"libraries"_s);
        QVERIFY(hubFolder(folder));
        const QString library = QDir(folder).filePath(u"home"_s);
        auto keeper = std::make_unique<HubKeeper>(folder);
        QVERIFY2(keeper->open(), "opened");
        const auto [refused, why] = home.run({u"add"_s, u"--library"_s, library, u"--paper"_s, u"--title"_s, u"Elsewhere"_s});
        QVERIFY2(refused != 0, qPrintable(why));
        QVERIFY2(why.contains(u"kept by the hub"_s), qPrintable(why));
        // The daemon's own program may: a worker of its own, on a thread of its own.
        Library ours(library);
        QVERIFY(ours.open());
        // Once the hub has stopped, the library is anyone's again.
        keeper.reset();
        const auto [added, said] = home.run({u"add"_s, u"--library"_s, library, u"--paper"_s, u"--title"_s, u"Elsewhere"_s});
        QVERIFY2(added == 0, qPrintable(said));
    }

    void theDaemonComesBackAfterItIsKilled() {
        Home home;
        const QString folder = home.dir.filePath(u"libraries"_s);
        QVERIFY(hubFolder(folder));
        // Not set up: systemd is told not to start it again and again.
        QCOMPARE(home.run({u"hub"_s, u"run"_s}).first, 78);
        const auto startHub = [&home](QProcess &daemon, const QStringList &more) {
            home.start(daemon, QStringList{u"hub"_s, u"run"_s} + more);
            QByteArray said;
            return QTest::qWaitFor([&daemon, &said] { return (said += daemon.readAllStandardOutput()).contains("takes requests at"); },
                                   30000);
        };
        QProcess daemon;
        QVERIFY(startHub(daemon, {u"--libraries"_s, folder}));
        const auto [asked, status] = home.run({u"hub"_s, u"status"_s, u"--json"_s});
        QVERIFY2(asked == 0, qPrintable(status));
        daemon.kill();
        QVERIFY(daemon.waitForFinished(10000));
        // It left its socket and its hold behind, which hold nothing now.
        QVERIFY(QFileInfo::exists(home.place().socket()));
        QVERIFY(QFileInfo::exists(QDir(folder).filePath(u"home/"_s + QString::fromLatin1(LibraryHold::fileName))));
        const auto [listed, books] = home.run({u"list"_s, u"--library"_s, QDir(folder).filePath(u"home"_s)});
        QVERIFY2(listed == 0, qPrintable(books));
        // Back, as systemd starts it again, with the folder chosen before.
        QProcess again;
        QVERIFY(startHub(again, {}));
        const auto [back, answered] = home.run({u"hub"_s, u"status"_s, u"--json"_s});
        QVERIFY2(back == 0, qPrintable(answered));
        const QJsonObject libraries = QJsonDocument::fromJson(answered.toUtf8()).object().value("libraries").toArray().at(0).toObject();
        QCOMPARE(libraries.value("name").toString(), u"home"_s);
        // A second one beside it is refused, rather than fight it.
        const auto [second, why] = home.run({u"hub"_s, u"run"_s});
        QVERIFY2(second != 0 && why.contains(u"running here already"_s), qPrintable(why));
        again.kill();
        QVERIFY(again.waitForFinished(10000));
    }

    void theUnitsStartItAgainWheneverItStops() {
        for (const char *variant : {"user", "system"}) {
            QFile unit(QString::fromLatin1(OMABOOK_SOURCE_DIR)
                       + u"/share/systemd/%1/omabook-hub.service.in"_s.arg(QString::fromLatin1(variant)));
            QVERIFY(unit.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(unit.readAll());
            for (const QString &line :
                 {u"ExecStart=\"@omabook_installed@\" hub run"_s, u"Restart=always"_s, u"RestartPreventExitStatus=78"_s,
                  u"StateDirectory=omabook-hub"_s, u"RuntimeDirectory=omabook-hub"_s, u"ImportCredential=omabook-hub.*"_s})
                QVERIFY2(text.contains(u"\n"_s + line + u"\n"_s), qPrintable(QString::fromLatin1(variant) + u": "_s + line));
        }
    }

    void aSessionTakesOnlyTheHubsRequests() {
        Home nothing;
        const auto [unanswered, notRunning] = nothing.run({u"hub"_s, u"session"_s}, R"({"request": "status"})"
                                                                                    "\n");
        QCOMPARE(unanswered, 1);
        QVERIFY2(notRunning.contains(u"No hub is running here"_s), qPrintable(notRunning));
        InProcess hub;
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const auto [code, said] =
            hub.home.run({u"hub"_s, u"session"_s}, "{\"request\": \"status\"}\n{\"request\": \"shell\", \"command\": \"rm -rf ~\"}\n"
                                                   "rm -rf ~\n[1, 2]\n");
        QCOMPARE(code, 0);
        const QList<QJsonObject> answered = answers(said);
        QCOMPARE(answered.size(), 4);
        QVERIFY2(answered.at(0).value("ok").toBool(), qPrintable(said));
        QCOMPARE(answered.at(0).value("libraries").toArray().at(0).toObject().value("name").toString(), u"home"_s);
        QCOMPARE(answered.at(0).value("version").toString(), QString::fromLatin1(OMABOOK_VERSION));
        QCOMPARE(answered.at(1).value("error").toString(), u"The hub takes no request called shell."_s);
        for (const qsizetype refused : {2, 3})
            QVERIFY2(!answered.at(refused).value("ok").toBool()
                         && answered.at(refused).value("error").toString().contains(u"one line of JSON"_s),
                     qPrintable(said));
    }

    void whatComesBeforeAWriteThatChangesMuchRunsInItsLock() {
        QTemporaryDir dir;
        QVERIFY(hubFolder(dir.path()));
        HubKeeper keeper(dir.path());
        QStringList before;
        bool refuse = false;
        keeper.before([&](Library &, HubKeeper::Moment moment) -> std::expected<void, Failure> {
            // Inside the write's lock: nothing else has the library meanwhile.
            if (!keeper.busy())
                return std::unexpected(Failure::said(u"outside the lock"_s));
            before << HubKeeper::momentName(moment);
            if (refuse)
                return std::unexpected(Failure::said(u"no snapshot"_s));
            return {};
        });
        QVERIFY2(keeper.open(), "opened");
        int written = 0;
        const auto write = [&written](Library &) -> std::expected<QJsonObject, Failure> {
            ++written;
            return QJsonObject{};
        };
        QVERIFY(keeper.write({}, HubKeeper::Moment::None, write));
        QVERIFY(keeper.write(u"home"_s, HubKeeper::Moment::Sync, write));
        refuse = true;
        const auto refused = keeper.write({}, HubKeeper::Moment::Restore, write);
        QCOMPARE(whyNot(refused), u"no snapshot"_s);
        QCOMPARE(before, (QStringList{u"sync"_s, u"restore"_s}));
        QCOMPARE(written, 2);
        QVERIFY(!keeper.busy());
        // Work inside work could only wait for itself: refused, not hung.
        const auto inner = keeper.write({}, HubKeeper::Moment::None, [&keeper, &write](Library &) -> std::expected<QJsonObject, Failure> {
            const auto again = keeper.write({}, HubKeeper::Moment::None, write);
            if (again)
                return std::unexpected(Failure::said(u"the lock was taken twice"_s));
            return QJsonObject{};
        });
        QVERIFY2(inner, qPrintable(whyNot(inner)));
        QCOMPARE(keeper.write(u"elsewhere"_s, HubKeeper::Moment::None, write).error().code(), Failure::Code::HubNoSuchLibrary);
    }

    void anotherHubCantHoldTheSameLibraries() {
        QTemporaryDir dir;
        QVERIFY(hubFolder(dir.path()));
        const auto held = LibraryHold::take(dir.filePath(u"home"_s));
        QVERIFY2(held, qPrintable(whyNot(held)));
        // Another hold in the same program is another hub as far as the file goes.
        const auto again = LibraryHold::take(dir.filePath(u"home"_s), 100ms);
        QVERIFY(!again);
        QCOMPARE(again.error().code(), Failure::Code::LibraryKeptByHub);
        QVERIFY(!LibraryHold::heldElsewhere(dir.filePath(u"home"_s)));
    }

    void theHubsJobsRunWhenTheyAreDue() {
        const QDateTime night(QDate(2026, 11, 22), QTime(3, 0));
        QCOMPARE(HubSchedule::dueDaily({}, QTime(3, 0), night.addSecs(5)), night.addSecs(5));
        QCOMPARE(HubSchedule::dueDaily(night, QTime(3, 0), night), night.addDays(1));
        QCOMPARE(HubSchedule::dueDaily(night.addSecs(-3600), QTime(3, 0), night), night);
        QTemporaryDir dir;
        QVERIFY(hubFolder(dir.filePath(u"libraries"_s)));
        HubKeeper keeper(dir.filePath(u"libraries"_s));
        QVERIFY(keeper.open());
        int snapshots = 0;
        int checks = 0;
        const auto plan = [&](HubSchedule &schedule) {
            schedule.daily(u"snapshot"_s, QTime(3, 0), [&snapshots](HubKeeper &kept) {
                ++snapshots;
                return kept.whole([](std::span<Library *const> libraries) -> std::expected<void, Failure> {
                    if (libraries.size() != 1)
                        return std::unexpected(Failure::said(u"not every library"_s));
                    return {};
                });
            });
            schedule.every(u"check"_s, std::chrono::days(7), [&checks](HubKeeper &) -> std::expected<void, Failure> {
                ++checks;
                return std::unexpected(Failure::said(u"restic check failed"_s));
            });
        };
        {
            HubSchedule schedule(keeper, dir.filePath(u"state"_s));
            plan(schedule);
            // Never run before: at once.
            schedule.runDue(night);
            QCOMPARE(snapshots, 1);
            QCOMPARE(checks, 1);
            QVERIFY(schedule.outcome(u"snapshot"_s)->done);
            QCOMPARE(schedule.outcome(u"check"_s)->said, u"restic check failed"_s);
            schedule.runDue(night.addSecs(60));
            QCOMPARE(snapshots, 1);
            QCOMPARE(checks, 1);
        }
        // Kept across a start: the failed check is tried again an hour on, the
        // snapshot the next night, or at once when that was missed.
        HubSchedule schedule(keeper, dir.filePath(u"state"_s));
        plan(schedule);
        schedule.runDue(night.addSecs(3600));
        QCOMPARE(checks, 2);
        QCOMPARE(snapshots, 1);
        schedule.runDue(night.addDays(3));
        QCOMPARE(snapshots, 2);
    }

    void theHubsSecretsAreItsAlone() {
        QTemporaryDir dir;
        const HubPlace place(
            {.state = dir.filePath(u"state"_s), .runtime = dir.filePath(u"run"_s), .credentials = dir.filePath(u"credentials"_s)});
        QVERIFY2(place.keepSecret(u"restic-password"_s, "correct horse"), "kept");
        QCOMPARE(place.secret(u"restic-password"_s).value_or(QByteArray()), QByteArray("correct horse"));
        QFile file(dir.filePath(u"state/secrets/restic-password"_s));
        QCOMPARE(file.permissions() & (QFile::ReadGroup | QFile::ReadOther), QFile::Permissions());
        QVERIFY(file.setPermissions(file.permissions() | QFile::ReadOther));
        QCOMPARE(place.secret(u"restic-password"_s).error().code(), Failure::Code::HubSecretOpen);
        // systemd's credential, when it gives one, comes first.
        QVERIFY(QDir().mkpath(dir.filePath(u"credentials"_s)));
        QVERIFY(Fixtures::write(dir.filePath(u"credentials/omabook-hub.restic-password"_s), "from systemd"));
        QCOMPARE(place.secret(u"restic-password"_s).value_or(QByteArray()), QByteArray("from systemd"));
        QVERIFY(!place.secret(u"../state/secrets/restic-password"_s));
        QVERIFY(!place.keepSecret(u"../../elsewhere"_s, "no"));
    }
};

QTEST_GUILESS_MAIN(HubTests)
#include "hub_tests.moc"
