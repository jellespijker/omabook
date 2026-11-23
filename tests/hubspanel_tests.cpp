// The Hubs panel (#189), from the settings, the library's menu and its key:
// a hub linked after saying where it is and yes to what linking does there,
// then shown as it stands (reachable, its Omabook, its libraries, the
// computers linked to it, its snapshots' warnings in red); another computer
// taken off it after asking; the hub unlinked, by a key and by the mouse;
// and a hub that took the key away, or can't be reached, said so. Surveyed in
// German, under fingers and right to left.
//
// The hub is in this process, in temporary folders: its keeper of one
// library, its requests with the snapshots' among them, its computers' keys,
// and the session's pipe (HubSession), as hubsync_tests has it. Linking does
// there what `omabook hub allow` does over the person's ssh; the same over
// ssh itself is in hublink_tests.

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QMutex>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QStyleHints>
#include <QThread>
#include <QUuid>
#include <QtTest>
#include <memory>
#include <span>
#include <utility>

#include "bookmeta.h"
#include "crypto.h"
#include "desk.h"
#include "hubcomputers.h"
#include "hubdesk.h"
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

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// restic, and the worker, can take a while on a busy machine.
constexpr int slow = 30000;

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

[[nodiscard]] QString signedBy(const Person &person, const QString &name, const QString &line) {
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
    std::optional<Person> person;
    mutable QObject home; // on the window's thread
    bool reachable = true;

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
        desk.hubsDesk.emplace(std::move(hub));
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

    // A library the hub's isn't paired with, as another computer's library is
    // (#214), is refused at Sync Now and at its preview, the panel saying how
    // to pair; neither side changes.
    void aLibraryNotPairedWithTheHubsSaysHowToPair() {
        Desk desk("en", {}, false);
        HubHere *hub = served(desk);
        QVERIFY(hub);
        const QString other = QUuid::createUuid().toString(QUuid::Id128);
        QVERIFY(hub->keeper
                    .whole([&other](std::span<Library *const> libraries) -> std::expected<void, Failure> {
                        Spaces::pair(*libraries.front(), u"personal:"_s + other, other, u"desktop"_s);
                        return {};
                    })
                    .has_value());
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

    // The panel with a hub and its warnings, its computers, and linking's yes: at every size, in a
    // long language, under fingers and right to left.
    void thePanelAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            QVERIFY(served(desk));
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QVERIFY(linked(desk));
            QVERIFY(open(desk));
            QTRY_VERIFY(shown(desk, "hubWarning"));
            desk.quicken();
            for (const QString &page : {u"list"_s, u"computers"_s, u"consent"_s}) {
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
