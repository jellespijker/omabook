// The Hubs panel (#189), from the settings, the library's menu and its key:
// a hub linked after saying where it is and yes to what linking does there,
// then shown as it stands (reachable, its Omabook, its libraries, the
// computers linked to it, its snapshots' warnings in red); another computer
// taken off it after asking; the hub unlinked, by a key and by the mouse;
// and a hub that took the key away, or can't be reached, said so; a library
// the hub's isn't paired with paired after asking (#217); automatic sync
// switched on, and a sync it was refused said in red (#191). Surveyed in
// German, under fingers and right to left.
//
// The hub is in this process, in temporary folders (hubhere.h). Linking does
// there what `omabook hub allow` does over the person's ssh; the same over
// ssh itself is in hublink_tests.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QStyleHints>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <chrono>
#include <memory>
#include <span>
#include <utility>

#include "bookmeta.h"
#include "crypto.h"
#include "desk.h"
#include "hubcomputers.h"
#include "hubdesk.h"
#include "hubhere.h"
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
#include "syncwait.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// restic, and the worker, can take a while on a busy machine.
constexpr int slow = 30000;

using HubTests::HubHere;

} // namespace

class HubsPanelTests : public QObject {
    Q_OBJECT

    static QVariant value(const Desk &desk, const QString &name, const char *property) {
        QObject *found = desk.find(name);
        return found ? found->property(property) : QVariant();
    }
    // The item of that name on screen; a list made again leaves its old items for a moment, out of sight.
    static QQuickItem *onScreen(const Desk &desk, const QString &name, bool pressable = false) {
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (!item->isVisible())
                continue;
            if (item->objectName() == name && item->width() > 0 && item->height() > 0 && (!pressable || item->isEnabled()))
                return item;
            items << item->childItems();
        }
        return nullptr;
    }
    static QString text(const Desk &desk, const QString &name) {
        QObject *item = onScreen(desk, name);
        return item ? item->property("text").toString() : QString();
    }
    static bool shown(const Desk &desk, const QString &name) { return onScreen(desk, name) != nullptr; }
    // Pressed as a mouse does, where it is on screen: as a person would, not
    // so soon after the last that the two make a double click.
    static void click(const Desk &desk, const QString &name) {
        QVERIFY2(QTest::qWaitFor([&] { return onScreen(desk, name, true) != nullptr; }, 5000), qPrintable(name));
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QQuickItem *item = onScreen(desk, name, true);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    // Tapped with a finger, where it is on screen.
    static void tap(const Desk &desk, const QString &name) {
        static QPointingDevice *const finger = QTest::createTouchDevice();
        QVERIFY2(QTest::qWaitFor([&] { return onScreen(desk, name, true) != nullptr; }, 5000), qPrintable(name));
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QQuickItem *item = onScreen(desk, name, true);
        const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
        QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
    }
    // A button out of sight in a dialog that scrolls, pressed.
    static void press(const Desk &desk, const QString &name) {
        QObject *button = desk.find(name);
        QVERIFY2(button, qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
    }
    static void type(const Desk &desk, const QString &name, const QString &words) {
        QObject *field = desk.find(name);
        QVERIFY2(field, qPrintable(name));
        QVERIFY(field->setProperty("text", words));
    }
    static void key(const Desk &desk, Qt::Key pressed, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QTest::keyClick(desk.window, pressed, modifiers);
        QCoreApplication::processEvents();
    }
    static bool idle(const Desk &desk) {
        return QTest::qWaitFor([&] { return desk.hubsDesk->doing().isEmpty(); }, slow);
    }
    // The desk's hubs are the hub here's.
    static HubHere *served(Desk &desk) {
        auto hub = std::make_unique<HubHere>();
        HubHere *kept = hub.get();
        const QString why = kept->start();
        if (!why.isEmpty()) {
            qWarning().noquote() << why;
            return nullptr;
        }
        // Automatic sync's state beside the desk's links, its service never this computer's (#191).
        desk.hubsDesk.emplace(std::move(hub), HubDesk::Automatic{.folder = desk.dir.filePath("hubs"),
                                                                 .now = [] { return QDateTime::currentDateTimeUtc(); },
                                                                 .wait = [] { return SyncWait::Reason::None; },
                                                                 .notify = {},
                                                                 .service = {.folder = desk.dir.filePath("systemd/user"),
                                                                             .systemctl = desk.dir.filePath("none"),
                                                                             .omabook = "/usr/bin/omabook"},
                                                                 .every = std::chrono::hours(1),
                                                                 .times = {}});
        return kept;
    }
    static bool open(Desk &desk) {
        desk.call("showHubs");
        return desk.waitForPopup() && value(desk, "hubs", "opened").toBool() && idle(desk);
    }
    // This computer linked as laptop, through the desk, and the hub asked how it stands.
    static bool linked(Desk &desk) {
        desk.hubsDesk->link("hub", "laptop");
        return idle(desk) && !desk.hubsDesk->failed() && desk.hubsDesk->hubs().size() == 1;
    }
    static QVariantMap first(const Desk &desk) { return desk.hubsDesk->hubs().value(0).toMap(); }

    QStringList found;
    void look(Desk &desk, const QString &state) {
        QVERIFY2(desk.pictured(), qPrintable(state));
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("hubs-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QVERIFY2(!QStandardPaths::findExecutable("restic").isEmpty(), "restic is one of Omabook's dependencies");
    }

    // From the settings: where the hub is, then a yes to what linking does, then the hub as it stands.
    void aHubIsLinkedAfterAYes() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.call("showSettings");
        QVERIFY(desk.waitForPopup());
        QCOMPARE(text(desk, "hubsSummary"), QString("Linked to no hub."));
        press(desk, "openHubs");
        QTRY_VERIFY(value(desk, "hubs", "opened").toBool());
        QVERIFY(idle(desk));
        QVERIFY(shown(desk, "hubsNone"));

        // A hub that isn't there: said, and still on the page to try another.
        click(desk, "hubsLink");
        QTRY_VERIFY(shown(desk, "hubHost"));
        QVERIFY(!value(desk, "hubNext", "enabled").toBool());
        type(desk, "hubHost", "nowhere");
        click(desk, "hubNext");
        QTRY_VERIFY(shown(desk, "hubConsent"));
        click(desk, "hubLinkYes");
        QVERIFY(idle(desk));
        QVERIFY(desk.hubsDesk->failed());
        QVERIFY2(text(desk, "hubsStatus").contains("nowhere can't be reached"), qPrintable(text(desk, "hubsStatus")));
        QVERIFY(shown(desk, "hubConsent"));
        QVERIFY(desk.hubsDesk->hubs().isEmpty());

        // The hub, as laptop: nothing is done there before the yes.
        click(desk, "hubLinkBack");
        QTRY_VERIFY(shown(desk, "hubHost"));
        type(desk, "hubHost", "hub");
        type(desk, "hubComputerName", "laptop");
        click(desk, "hubNext");
        QTRY_VERIFY(shown(desk, "hubConsent"));
        const QString consent = text(desk, "hubConsent");
        QVERIFY2(consent.contains("hub once, over ssh") && consent.contains("never asks for a password"), qPrintable(consent));
        QCOMPARE(hub->names(), QStringList{"desktop"});
        click(desk, "hubLinkYes");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QCOMPARE(hub->names(), (QStringList{"desktop", "laptop"}));

        // Back in the list, the hub as it stands.
        QTRY_VERIFY(shown(desk, "hubsList"));
        QVERIFY2(text(desk, "hubsStatus").startsWith("This computer is linked to the hub at hub as laptop"),
                 qPrintable(text(desk, "hubsStatus")));
        QCOMPARE(text(desk, "hubName"), QString("hub"));
        QCOMPARE(text(desk, "hubState"), QString("Reachable, Omabook %1. This computer is laptop there.").arg(OMABOOK_VERSION));
        QCOMPARE(text(desk, "hubLibraries"), QString("home: 1 book"));
        QCOMPARE(text(desk, "hubComputers"), QString("2 computers linked: desktop, laptop"));
        // Its snapshots' password is gone: said in red, with what to do, as the hub says it.
        QTRY_VERIFY(shown(desk, "hubWarning"));
        const QStringList warnings = first(desk).value("warnings").toStringList();
        QVERIFY2(std::ranges::any_of(warnings, [](const QString &warning) { return warning.contains("password"); }),
                 qPrintable(warnings.join('\n')));
        QCOMPARE(onScreen(desk, "hubWarning")->property("color").value<QColor>(), desk.look->colors().value("red").value<QColor>());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // By the keyboard: Enter shows the hub's computers, Down to the desktop, Delete asks, Enter takes it off.
    void anotherComputerIsForgottenAfterAsking() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubsList"));
        key(desk, Qt::Key_Return);
        QTRY_VERIFY(shown(desk, "hubComputer"));
        QCOMPARE(text(desk, "hubComputer"), QString("desktop"));
        // This computer is marked, and only unlinking takes it off.
        QVERIFY(std::ranges::any_of(first(desk).value("computers").toList(), [](const QVariant &computer) {
            return computer.toMap().value("name") == "laptop" && computer.toMap().value("this").toBool();
        }));
        key(desk, Qt::Key_Down);
        key(desk, Qt::Key_Delete);
        QVERIFY(!shown(desk, "hubForgetQuestion")); // laptop is this computer: nothing to ask
        key(desk, Qt::Key_Up);
        key(desk, Qt::Key_Delete);
        QTRY_VERIFY(shown(desk, "hubForgetQuestion"));
        QVERIFY(text(desk, "hubForgetQuestion").startsWith("Take desktop off the hub?"));
        // Escape leaves it, asked again it goes.
        key(desk, Qt::Key_Escape);
        QTRY_VERIFY(!shown(desk, "hubForgetQuestion"));
        QVERIFY(value(desk, "hubs", "opened").toBool());
        QCOMPARE(hub->names(), (QStringList{"desktop", "laptop"}));
        key(desk, Qt::Key_D);
        QTRY_VERIFY(shown(desk, "hubForgetQuestion"));
        key(desk, Qt::Key_Return);
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QCOMPARE(hub->names(), QStringList{"laptop"});
        QCOMPARE(text(desk, "hubsStatus"), QString("The computer desktop is no longer linked to the hub at hub: its key is gone."));
        QTRY_COMPARE(text(desk, "hubComputer"), QString("laptop (this computer)"));
        // Escape goes back a step, then closes.
        key(desk, Qt::Key_Escape);
        QTRY_VERIFY(shown(desk, "hubsList"));
        QVERIFY(text(desk, "hubComputers").startsWith("1 computer linked: laptop"));
        key(desk, Qt::Key_Escape);
        QTRY_VERIFY(!value(desk, "hubs", "opened").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Unlinking asks first: D and Escape leave it, the mouse says yes.
    void theHubIsUnlinked() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubsList"));
        key(desk, Qt::Key_D);
        QTRY_VERIFY(shown(desk, "hubUnlinkQuestion"));
        key(desk, Qt::Key_Escape);
        QTRY_VERIFY(!shown(desk, "hubUnlinkQuestion"));
        click(desk, "hubUnlink");
        QTRY_VERIFY(shown(desk, "hubUnlinkQuestion"));
        click(desk, "hubUnlinkNo");
        QTRY_VERIFY(!shown(desk, "hubUnlinkQuestion"));
        QCOMPARE(hub->names(), (QStringList{"desktop", "laptop"}));
        click(desk, "hubUnlink");
        click(desk, "hubUnlinkYes");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QCOMPARE(hub->names(), QStringList{"desktop"});
        QVERIFY(desk.hubsDesk->hubs().isEmpty());
        QTRY_VERIFY(shown(desk, "hubsNone"));
        QCOMPARE(text(desk, "hubsStatus"), QString("This computer is no longer linked to the hub at hub: its key there is gone."));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A hub that took the key away, and one that can't be reached, say so; R asks again.
    void aHubThatCantBeAskedSaysWhy() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        hub->reachable = false;
        QVERIFY(open(desk));
        QCOMPARE(first(desk).value("state").toString(), QString("unreachable"));
        QVERIFY2(text(desk, "hubState").contains("Connection timed out"), qPrintable(text(desk, "hubState")));
        QVERIFY(!shown(desk, "hubShowComputers"));
        QVERIFY(!shown(desk, "hubWarning"));
        hub->reachable = true;
        hub->takeAway("laptop");
        key(desk, Qt::Key_R);
        QVERIFY(idle(desk));
        QCOMPARE(first(desk).value("state").toString(), QString("unlinked"));
        QTRY_VERIFY(text(desk, "hubState").startsWith("The hub has taken this computer's key away"));
        // Unlinked here too: the hub had already let it go.
        click(desk, "hubUnlink");
        click(desk, "hubUnlinkYes");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QVERIFY(desk.hubsDesk->hubs().isEmpty());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A sync seen first, with P, changes nothing on either side; Sync Now then does it.
    void theHubIsSyncedAfterAPreview() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubSyncNow"));
        key(desk, Qt::Key_P);
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QTRY_VERIFY(shown(desk, "hubSynced"));
        const QString preview = text(desk, "hubSynced");
        QVERIFY2(preview.startsWith("A sync now would do this") && preview.contains("1 book received") && preview.contains("1 book sent"),
                 qPrintable(preview));
        QCOMPARE(hub->onHub(), 1);
        QCOMPARE(hub->here(), 1);
        click(desk, "hubSyncNow");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QTRY_VERIFY(text(desk, "hubSynced").startsWith("Synced:"));
        QCOMPARE(hub->onHub(), 2);
        QCOMPARE(hub->here(), 2);
        // The hub asked again: its library as the sync left it.
        QTRY_COMPARE(text(desk, "hubLibraries"), QString("home: 2 books"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The hub's background work (#192): the book whose graph it builds and how
    // far it got, then stopped with Cancel Work.
    void theHubsWorkShowsAndIsCancelled() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_COMPARE(text(desk, "hubWork"), QString("Building the graph of Moss: 1 of 3 requests."));
        click(desk, "hubCancelWork");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QVERIFY(!hub->working);
        QTRY_COMPARE(text(desk, "hubWork"), QString("No background work under way."));
        QVERIFY(!shown(desk, "hubCancelWork"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // C stops the selected hub's work too, from the keyboard (#190).
    void theHubsWorkIsCancelledByItsKey() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_COMPARE(text(desk, "hubWork"), QString("Building the graph of Moss: 1 of 3 requests."));
        key(desk, Qt::Key_C);
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QVERIFY(!hub->working);
        QTRY_COMPARE(text(desk, "hubWork"), QString("No background work under way."));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A library the hub's isn't paired with, as another computer's library is
    // (#214), is refused at Sync Now and at its preview, the panel saying how
    // to pair; neither side changes.
    void aLibraryNotPairedWithTheHubsSaysHowToPair() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(hub->pairedElsewhere());
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubSyncNow"));
        for (const bool preview : {true, false}) {
            if (preview)
                key(desk, Qt::Key_P);
            else
                click(desk, "hubSyncNow");
            QVERIFY(idle(desk));
            QVERIFY(desk.hubsDesk->failed());
            QTRY_VERIFY(text(desk, "hubsStatus").contains("isn't paired with this computer's library"));
            QVERIFY2(text(desk, "hubsStatus").contains("omabook hub sync <hub>:home --pair"), qPrintable(text(desk, "hubsStatus")));
            QCOMPARE(hub->onHub(), 1);
            QCOMPARE(hub->here(), 1);
        }
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Where the sync was refused for that, Pair asks first, naming both libraries (#217): A, Escape and
    // Keep Apart leave them unpaired; a yes to the preview's pairs nothing and shows what the sync would
    // do; a yes to the sync's, tapped, pairs them through --pair and syncs, and the next sync goes through.
    void aLibraryNotPairedIsPairedAfterAYes() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(hub->pairedElsewhere());
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubSyncNow"));
        QVERIFY(!shown(desk, "hubPair"));
        key(desk, Qt::Key_A); // nothing to pair yet
        QVERIFY(!shown(desk, "hubPairQuestion"));

        key(desk, Qt::Key_P);
        QVERIFY(idle(desk));
        QVERIFY(desk.hubsDesk->failed());
        QTRY_VERIFY(shown(desk, "hubPair"));
        QVERIFY(!shown(desk, "hubPairQuestion"));
        key(desk, Qt::Key_A);
        QTRY_VERIFY(shown(desk, "hubPairQuestion"));
        QVERIFY2(text(desk, "hubPairQuestion")
                     .startsWith("See what a sync would do with the hub's library home paired with this computer's library laptop?"),
                 qPrintable(text(desk, "hubPairQuestion")));
        key(desk, Qt::Key_Escape);
        QTRY_VERIFY(!shown(desk, "hubPairQuestion"));
        QVERIFY(value(desk, "hubs", "opened").toBool());
        click(desk, "hubPair");
        QTRY_VERIFY(shown(desk, "hubPairQuestion"));
        click(desk, "hubPairNo");
        QTRY_VERIFY(!shown(desk, "hubPairQuestion"));
        QVERIFY(shown(desk, "hubPair"));
        QCOMPARE(hub->onHub(), 1);
        QCOMPARE(hub->here(), 1);

        // The preview's yes, by Enter: what the sync would do, and nothing changed or paired.
        key(desk, Qt::Key_A);
        QTRY_VERIFY(shown(desk, "hubPairQuestion"));
        key(desk, Qt::Key_Return);
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QTRY_VERIFY(shown(desk, "hubSynced"));
        QVERIFY2(text(desk, "hubSynced").startsWith("A sync now would do this") && text(desk, "hubSynced").contains("1 book received"),
                 qPrintable(text(desk, "hubSynced")));
        QVERIFY(!shown(desk, "hubPair"));
        QCOMPARE(hub->onHub(), 1);
        QCOMPARE(hub->here(), 1);

        // Sync Now is still refused, and its Pair, tapped, pairs and syncs.
        click(desk, "hubSyncNow");
        QVERIFY(idle(desk));
        QVERIFY(desk.hubsDesk->failed());
        QTRY_VERIFY(text(desk, "hubsStatus").contains("isn't paired with this computer's library"));
        tap(desk, "hubPair");
        QTRY_VERIFY(shown(desk, "hubPairQuestion"));
        QVERIFY2(
            text(desk, "hubPairQuestion").startsWith("Pair the hub's library home with this computer's library laptop, and sync them?"),
            qPrintable(text(desk, "hubPairQuestion")));
        QCOMPARE(text(desk, "hubPairYes"), QString("Pair and Sync"));
        tap(desk, "hubPairYes");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QTRY_VERIFY(text(desk, "hubSynced").startsWith("Synced:"));
        QCOMPARE(hub->onHub(), 2);
        QCOMPARE(hub->here(), 2);
        QVERIFY(!shown(desk, "hubPair"));

        // Paired: the next sync goes through, asking nothing.
        key(desk, Qt::Key_S);
        QVERIFY(idle(desk));
        QVERIFY2(!desk.hubsDesk->failed(), qPrintable(desk.hubsDesk->status()));
        QVERIFY(desk.hubsDesk->unpaired().isEmpty());
        QTRY_VERIFY(text(desk, "hubSynced").startsWith("Synced:"));
        QVERIFY(!shown(desk, "hubPair"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Automatic sync switched on by its box (#191): it syncs at once, and says
    // so. A sync the hub refuses, of a library it isn't paired with, is shown
    // in red with the hub, saying what to do; O switches it off and on again,
    // and it is tried once more.
    void automaticSyncIsSwitchedOnAndARefusalShown() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubAuto"));
        QVERIFY(!shown(desk, "hubAutoState"));
        QVERIFY(!shown(desk, "hubsBackground"));
        click(desk, "hubAuto");
        QVERIFY(idle(desk));
        QCOMPARE(hub->begun.load(), 1);
        QCOMPARE(hub->onHub(), 2);
        QTRY_VERIFY(text(desk, "hubAutoState").startsWith("It syncs by itself a little after a change"));
        QVERIFY(shown(desk, "hubsBackground"));
        key(desk, Qt::Key_O);
        QTRY_VERIFY(!shown(desk, "hubAutoState"));
        QCOMPARE(first(desk).value("auto").toBool(), false);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void anAutomaticSyncRefusedIsShownInRed() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        QVERIFY(hub->pairedElsewhere());
        QVERIFY(desk.show());
        QVERIFY(linked(desk));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "hubAuto"));
        click(desk, "hubAuto");
        QVERIFY(idle(desk));
        QCOMPARE(hub->begun.load(), 1);
        QTRY_VERIFY(text(desk, "hubAutoState").startsWith("Automatic sync has stopped"));
        QVERIFY2(text(desk, "hubAutoState").contains("isn't paired with this computer's library"), qPrintable(text(desk, "hubAutoState")));
        QCOMPARE(onScreen(desk, "hubAutoState")->property("color").value<QColor>(), desk.look->colors().value("red").value<QColor>());
        QCOMPARE(hub->onHub(), 1);
        // Off and on again: the person looked, and it is tried once more.
        key(desk, Qt::Key_O);
        QTRY_VERIFY(!shown(desk, "hubAutoState"));
        key(desk, Qt::Key_O);
        QVERIFY(idle(desk));
        QCOMPARE(hub->begun.load(), 2);
        QTRY_VERIFY(text(desk, "hubAutoState").startsWith("Automatic sync has stopped"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The library's menu has it, and its key; N there begins a link.
    void theMenuAndTheKeyOpenIt() {
        Desk desk("en", {}, false);
        QVERIFY(desk.show());
        desk.menu("library");
        QVERIFY(desk.waitForPopup());
        QObject *menu = desk.find("commandMenu");
        const QVariantList entries = menu->property("entries").toList();
        const auto entry = std::ranges::find_if(entries, [](const QVariant &each) { return each.toMap().value("id") == "library.hubs"; });
        QVERIFY2(entry != entries.end(), "the library's menu has no Hubs");
        QVERIFY(QMetaObject::invokeMethod(menu, "take", Q_ARG(QVariant, *entry)));
        QTRY_VERIFY(value(desk, "hubs", "opened").toBool());
        QVERIFY(desk.closePopups());
        key(desk, Qt::Key_Y, Qt::ShiftModifier);
        QTRY_VERIFY(value(desk, "hubs", "opened").toBool());
        QVERIFY(idle(desk));
        key(desk, Qt::Key_N);
        QTRY_VERIFY(shown(desk, "hubHost"));
        QTRY_VERIFY(value(desk, "hubHost", "activeFocus").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The panel with a hub and its warnings, the question before pairing, its computers, and linking's yes: at every size, in a
    // long language, under fingers and right to left.
    void thePanelAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            HubHere *hub = served(desk);
            QVERIFY(hub);
            QVERIFY(hub->pairedElsewhere());
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QVERIFY(linked(desk));
            // Synced by itself, and said (#191).
            desk.hubsDesk->setAutomatic("hub", true);
            QVERIFY(idle(desk));
            QVERIFY(open(desk));
            QTRY_VERIFY(shown(desk, "hubWarning"));
            QTRY_VERIFY(shown(desk, "hubAutoState"));
            desk.quicken();
            for (const QString &page : {u"list"_s, u"pair"_s, u"computers"_s, u"consent"_s}) {
                if (page == u"pair"_s) {
                    press(desk, "hubSyncNow");
                    QVERIFY(idle(desk));
                    QTRY_VERIFY(desk.find("hubPair") && value(desk, "hubPair", "visible").toBool());
                    press(desk, "hubPair");
                    QTRY_VERIFY(shown(desk, "hubPairQuestion"));
                }
                if (page == u"computers"_s)
                    click(desk, "hubShowComputers");
                if (page == u"consent"_s) {
                    QVERIFY(desk.closePopups());
                    QVERIFY(open(desk));
                    click(desk, "hubsLink");
                    type(desk, "hubHost", "omabook@hub.example.ts.net");
                    click(desk, "hubNext");
                }
                for (const QSize &size : dialogSizes()) {
                    QVERIFY(desk.resize(size));
                    desk.settle();
                    look(desk, page);
                }
            }
            QVERIFY(desk.closePopups());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(HubsPanelTests)
#include "hubspanel_tests.moc"
