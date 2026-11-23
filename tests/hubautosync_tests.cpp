// Automatic sync with a hub (#191): off until switched on, and then syncing
// by itself a little after a change once the window was left alone, on the
// timer, and when the hub is back in reach; never while another sync runs,
// and not on battery. A sync the hub refuses is shown with the hub and told
// the desktop once, here or by the background service, and not tried again
// until the person looks. `omabook hub sync --auto` and `--background` switch
// it from the command line.
//
// The clock is the test's; the hub is in this process, over the session's
// pipe (hubhere.h), as the Hubs panel's tests have it.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <chrono>
#include <memory>
#include <optional>

#include "fixtures.h"
#include "hubautorun.h"
#include "hubautosync.h"
#include "hubdesk.h"
#include "hubhere.h"
#include "library.h"
#include "serveservice.h"
#include "syncwait.h"

using Fixtures::whyNot;
using HubTests::HubHere;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

constexpr int slow = 30000;

// This computer, linked to the hub as laptop, its automatic sync's clock the test's.
struct Here {
    QTemporaryDir dir;
    QDateTime now{QDate(2026, 11, 23), QTime(9, 0), QTimeZone::UTC};
    SyncWait::Reason wait = SyncWait::Reason::None;
    QStringList told;
    HubHere *hub = nullptr;
    std::optional<HubDesk> desk;

    [[nodiscard]] QString folder() const { return dir.filePath(u"hubs"_s); }

    [[nodiscard]] HubDesk::Automatic automatic() {
        return {.folder = folder(),
                .now = [this] { return now; },
                .wait = [this] { return wait; },
                .notify = [this](const QString &title, const QString &body) { told << title + u"\n"_s + body; },
                .service = {.folder = dir.filePath(u"systemd/user"_s),
                            .systemctl = dir.filePath(u"bin/systemctl"_s),
                            .omabook = u"/usr/bin/omabook"_s},
                // Only when the test says.
                .every = std::chrono::hours(1),
                .times = {}};
    }

    [[nodiscard]] QString start() {
        auto made = std::make_unique<HubHere>();
        hub = made.get();
        if (const QString why = hub->start(); !why.isEmpty())
            return why;
        desk.emplace(std::move(made), automatic());
        desk->link(u"hub"_s, u"laptop"_s);
        if (!idle() || desk->failed())
            return u"not linked: "_s + desk->status();
        return {};
    }

    [[nodiscard]] bool idle() {
        return QTest::qWaitFor([this] { return desk->doing().isEmpty(); }, slow);
    }
    // The clock on by `by`, and the desk looking.
    [[nodiscard]] bool pass(std::chrono::seconds by) {
        now = now.addSecs(by.count());
        desk->tick();
        return idle();
    }
    [[nodiscard]] QVariantMap first() const { return desk->hubs().value(0).toMap(); }

    // A book added to this computer's library, as the window would see it change.
    [[nodiscard]] bool add(const QString &title) {
        Library library(hub->laptopLibrary());
        const QString file = dir.filePath(title + u".epub"_s);
        if (!library.open() || !Fixtures::epub(file, title) || library.add(file).id <= 0)
            return false;
        desk->libraryChanged();
        return true;
    }
};

[[nodiscard]] bool program(const QString &path, const QByteArray &script) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(script) == script.size()
           && file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
}

[[nodiscard]] QByteArray contents(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class HubAutoSyncTests : public QObject {
    Q_OBJECT

private slots:
    // Switched off, as it is until the person switches it on, nothing syncs by itself.
    void offSyncsNothing() {
        Here here;
        QVERIFY2(here.start().isEmpty(), qPrintable(here.start()));
        QVERIFY(here.desk->automatic());
        QCOMPARE(here.first().value("auto").toBool(), false);
        QVERIFY(here.add(u"Fern"_s));
        for (int hour = 0; hour < 3; ++hour)
            QVERIFY(here.pass(1h));
        QCOMPARE(here.hub->begun.load(), 0);
        QCOMPARE(here.hub->onHub(), 1);
        QVERIFY(here.told.isEmpty());
    }

    // Switched on, it syncs at once; then a change once the window was left
    // alone a little, and the timer whatever happened; switched off, no more.
    void onSyncsAChangeOnceTheWindowIsLeftAlone() {
        Here here;
        QVERIFY2(here.start().isEmpty(), "no hub");
        here.desk->setAutomatic(u"hub"_s, true);
        QVERIFY(here.idle());
        QCOMPARE(here.hub->begun.load(), 1);
        QCOMPARE(here.hub->onHub(), 2);
        QCOMPARE(here.first().value("auto").toBool(), true);
        QCOMPARE(here.first().value("autoLast").toDateTime(), here.now);
        QVERIFY(HubAutoSync(here.folder()).hub(u"hub"_s).on);

        QVERIFY(here.add(u"Fern"_s));
        QVERIFY(here.pass(60s));
        QCOMPARE(here.hub->begun.load(), 1);
        here.desk->touched(); // a minute on, the person still at it
        QVERIFY(here.pass(90s));
        QCOMPARE(here.hub->begun.load(), 1);
        QVERIFY(here.pass(40s)); // two minutes since they were
        QCOMPARE(here.hub->begun.load(), 2);
        QCOMPARE(here.hub->onHub(), 3);
        // What the sync brought is no change of the person's to sync again.
        QVERIFY(here.pass(5min));
        QCOMPARE(here.hub->begun.load(), 2);

        // A quarter of an hour after the last, whatever happened.
        QVERIFY(here.pass(10min));
        QCOMPARE(here.hub->begun.load(), 3);

        here.desk->setAutomatic(u"hub"_s, false);
        QVERIFY(here.idle());
        QVERIFY(here.add(u"Bracken"_s));
        for (int hour = 0; hour < 3; ++hour)
            QVERIFY(here.pass(1h));
        QCOMPARE(here.hub->begun.load(), 3);
        QCOMPARE(here.first().value("auto").toBool(), false);
        QVERIFY(here.told.isEmpty());
    }

    // On battery it waits, and says so; plugged in again, it syncs.
    void onBatteryItWaits() {
        Here here;
        QVERIFY2(here.start().isEmpty(), "no hub");
        here.wait = SyncWait::Reason::Battery;
        here.desk->setAutomatic(u"hub"_s, true);
        QVERIFY(here.pass(1h));
        QCOMPARE(here.hub->begun.load(), 0);
        QCOMPARE(here.first().value("autoWaiting").toString(), u"battery"_s);
        here.wait = SyncWait::Reason::None;
        QVERIFY(here.pass(30s));
        QCOMPARE(here.hub->begun.load(), 1);
        QCOMPARE(here.first().value("autoWaiting").toString(), QString());
    }

    // Never while another sync of this computer's runs, whichever Omabook runs it.
    void aSyncRunningIsWaitedFor() {
        Here here;
        QVERIFY2(here.start().isEmpty(), "no hub");
        auto held = HubAutoSync(here.folder()).hold();
        QVERIFY(held);
        here.desk->setAutomatic(u"hub"_s, true);
        QVERIFY(here.idle());
        QVERIFY(here.pass(1min));
        QCOMPARE(here.hub->begun.load(), 0);
        held.reset();
        QVERIFY(here.pass(30s));
        QCOMPARE(here.hub->begun.load(), 1);
    }

    // A refused sync is shown with the hub and told once, here and by the
    // background service alike, and tried no more until the person looks:
    // switched off and on again, it is tried once more.
    void aRefusedSyncIsShownOnceAndNotTriedAgain() {
        Here here;
        QVERIFY2(here.start().isEmpty(), "no hub");
        QVERIFY(here.hub->pairedElsewhere());
        const QString said = here.desk->status(); // of the linking
        here.desk->setAutomatic(u"hub"_s, true);
        QVERIFY(here.idle());
        QCOMPARE(here.hub->begun.load(), 1);
        const QString refused = here.first().value("autoRefused").toString();
        QVERIFY2(refused.contains(u"isn't paired with this computer's library"_s) && refused.contains(u"--pair"_s), qPrintable(refused));
        QCOMPARE(here.told.size(), 1);
        QVERIFY2(here.told.first().contains(u"hub"_s) && here.told.first().contains(u"--pair"_s), qPrintable(here.told.first()));
        // The panel's line for what the person did stays as it was: the hub's own says it.
        QCOMPARE(here.desk->status(), said);
        QVERIFY(!here.desk->failed());
        QCOMPARE(here.hub->onHub(), 1);

        QVERIFY(here.add(u"Fern"_s));
        for (int hour = 0; hour < 3; ++hour)
            QVERIFY(here.pass(1h));
        QCOMPARE(here.hub->begun.load(), 1);
        QCOMPARE(here.told.size(), 1);

        // The background service, with the window open or closed, reads the same.
        QStringList toldThere;
        HubAutoRun service({.folder = here.folder(),
                            .hosts = [] { return QStringList{u"hub"_s}; },
                            .sync = [&here](const QString &host) { return here.hub->sync(host, {}); },
                            .now = [&here] { return here.now; },
                            .wait = [] { return SyncWait::Reason::None; },
                            .notify = [&toldThere](const QString &title, const QString &) { toldThere << title; },
                            .every = std::chrono::hours(1)});
        service.tick();
        QCOMPARE(here.hub->begun.load(), 1);
        QVERIFY(toldThere.isEmpty());

        // The person looked: off, and on again.
        here.desk->setAutomatic(u"hub"_s, false);
        QVERIFY(here.idle());
        here.desk->setAutomatic(u"hub"_s, true);
        QVERIFY(here.idle());
        QCOMPARE(here.hub->begun.load(), 2);
    }

    // A hub out of reach is no refusal: nothing is told, it is tried again in a
    // while, and at once when the network is back; what changed meanwhile goes.
    void aHubBackInReachIsCaughtUpWith() {
        Here here;
        QVERIFY2(here.start().isEmpty(), "no hub");
        here.desk->setAutomatic(u"hub"_s, true);
        QVERIFY(here.idle());
        QCOMPARE(here.hub->begun.load(), 1);

        here.hub->reachable = false;
        QVERIFY(here.add(u"Fern"_s));
        QVERIFY(here.pass(3min));
        QCOMPARE(here.hub->begun.load(), 2);
        QCOMPARE(here.first().value("autoUnreached").toBool(), true);
        QCOMPARE(here.first().value("autoRefused").toString(), QString());
        QVERIFY(here.told.isEmpty());
        QVERIFY(here.pass(1min));
        QCOMPARE(here.hub->begun.load(), 2);
        QVERIFY(here.pass(70s)); // tried again a while on, still out of reach
        QCOMPARE(here.hub->begun.load(), 3);

        here.hub->reachable = true;
        here.desk->cameIntoReach();
        QVERIFY(here.idle());
        QCOMPARE(here.hub->begun.load(), 4);
        QCOMPARE(here.hub->onHub(), 3);
        QCOMPARE(here.first().value("autoUnreached").toBool(), false);
        QVERIFY(here.told.isEmpty());
    }

    // The background service, allowed from the panel: installed through systemctl, and taken away.
    void theBackgroundServiceIsAllowed() {
        Here here;
        QVERIFY2(here.start().isEmpty(), "no hub");
        const QString log = here.dir.filePath(u"systemctl.log"_s);
        QVERIFY(program(here.dir.filePath(u"bin/systemctl"_s), "#!/bin/sh\necho \"$@\" >> '" + log.toUtf8() + "'\n"));
        QVERIFY(!here.desk->background());
        here.desk->setBackground(true);
        QVERIFY(here.idle());
        QVERIFY2(!here.desk->failed(), qPrintable(here.desk->status()));
        QVERIFY(here.desk->background());
        QFile unit(here.dir.filePath(u"systemd/user/omabook-hubsync.service"_s));
        QVERIFY(unit.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(unit.readAll());
        QVERIFY2(text.contains(u"ExecStart=\"/usr/bin/omabook\" --translated hub sync --service"_s), qPrintable(text));
        QVERIFY(contents(log).contains("enable --now omabook-hubsync.service"));
        here.desk->setBackground(false);
        QVERIFY(here.idle());
        QVERIFY(!here.desk->background());
        QVERIFY(contents(log).contains("disable --now omabook-hubsync.service"));
    }

    // `omabook hub sync <hub> --auto on|off`, and `--background on|off`, of a hub this computer is linked to.
    void theCommandLineSwitchesIt() {
        QTemporaryDir dir;
        const QString hubs = dir.filePath(u"data/omabook/hubs"_s);
        QVERIFY(QDir().mkpath(hubs));
        QVERIFY(Fixtures::write(
            hubs + u"/hubs.json"_s,
            QJsonDocument(QJsonArray{QJsonObject{{"host", "hub"}, {"computer", "laptop"}, {"fingerprint", "SHA256:x"}}}).toJson()));
        const QString log = dir.filePath(u"systemctl.log"_s);
        QVERIFY(program(dir.filePath(u"bin/systemctl"_s), "#!/bin/sh\necho \"$@\" >> '" + log.toUtf8() + "'\n"));
        const auto omabook = [&dir](const QStringList &arguments, QString *out) {
            QProcess process;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert(u"XDG_DATA_HOME"_s, dir.filePath(u"data"_s));
            environment.insert(u"XDG_CONFIG_HOME"_s, dir.filePath(u"config"_s));
            environment.insert(u"PATH"_s, dir.filePath(u"bin"_s) + u":"_s + environment.value(u"PATH"_s));
            process.setProcessEnvironment(environment);
            process.start(QStringLiteral(OMABOOK_BINARY), arguments);
            if (!process.waitForFinished(slow))
                return -1;
            *out = QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError());
            return process.exitCode();
        };
        QString said;
        QCOMPARE(omabook({u"hub"_s, u"sync"_s, u"hub"_s, u"--auto"_s, u"on"_s}, &said), 0);
        QVERIFY2(said.contains(u"Automatic sync with hub is on"_s), qPrintable(said));
        QVERIFY(HubAutoSync(hubs).hub(u"hub"_s).on);
        // The one hub, when none is named.
        QCOMPARE(omabook({u"hub"_s, u"sync"_s, u"--auto"_s, u"off"_s}, &said), 0);
        QVERIFY2(said.contains(u"Automatic sync with hub is off"_s), qPrintable(said));
        QVERIFY(!HubAutoSync(hubs).hub(u"hub"_s).on);
        QVERIFY(omabook({u"hub"_s, u"sync"_s, u"nowhere"_s, u"--auto"_s, u"on"_s}, &said) != 0);
        QVERIFY2(said.contains(u"nowhere"_s), qPrintable(said));
        QVERIFY(omabook({u"hub"_s, u"sync"_s, u"hub"_s, u"--auto"_s, u"sometimes"_s}, &said) != 0);
        QVERIFY(!HubAutoSync(hubs).hub(u"nowhere"_s).on);

        QCOMPARE(omabook({u"hub"_s, u"sync"_s, u"--background"_s, u"on"_s}, &said), 0);
        QVERIFY2(said.contains(u"with the window closed"_s), qPrintable(said));
        QVERIFY(QFile::exists(dir.filePath(u"config/systemd/user/omabook-hubsync.service"_s)));
        QVERIFY(contents(log).contains("enable --now omabook-hubsync.service"));
        QCOMPARE(omabook({u"hub"_s, u"sync"_s, u"--background"_s, u"off"_s}, &said), 0);
        QVERIFY(!QFile::exists(dir.filePath(u"config/systemd/user/omabook-hubsync.service"_s)));
    }
};

QTEST_MAIN(HubAutoSyncTests)
#include "hubautosync_tests.moc"
