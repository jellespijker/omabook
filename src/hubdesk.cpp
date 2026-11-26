#include "hubdesk.h"

#include <QEvent>
#include <QJsonArray>
#include <QLockFile>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

#include "backend.h"
#include "hubcomputers.h"
#include "hubremote.h"
#include "library.h"
#include "person.h"

using namespace Qt::StringLiterals;

namespace {

// This computer's links, signed by the person kept for the library in use.
class LinkedHere : public HubDesk::Hubs {
public:
    LinkedHere(std::function<QString()> root, QString folder) : m_root(std::move(root)), m_folder(std::move(folder)) {}

    [[nodiscard]] QList<HubLinks::Hub> linked() const override { return links().hubs(); }

    [[nodiscard]] std::expected<HubLinks::Hub, Failure> add(const QString &host, const QString &computer) override {
        PersonFile kept;
        const Person *person = kept.get(m_root());
        if (person == nullptr)
            return std::unexpected(Failure::said(kept.problem()));
        return links().add(host, computer, *person);
    }

    [[nodiscard]] std::expected<HubLinks::Hub, Failure> remove(const QString &host) override { return links().remove(host); }

    [[nodiscard]] std::expected<QJsonObject, Failure> ask(const QString &host, const QJsonObject &request) const override {
        return links().ask(host, request);
    }

    [[nodiscard]] SyncSummary sync(const QString &host, const SyncOptions &options) override {
        SyncSummary summary;
        summary.dryRun = options.dryRun;
        // This computer's library in use, opened by the window as Backend::sync does: no hub's keeper is here.
        // ast-grep-ignore: hub-writes-through-the-keeper
        Library library(m_root());
        if (!library.open(&summary.error))
            return summary;
        const auto session = links().open(host);
        if (!session) {
            summary.error = session.error().text();
            // A hub that took the key away is for the person to see to; one out of reach is tried again.
            summary.unreached = session.error().code() != Failure::Code::HubNotLinked;
            return summary;
        }
        return HubRemote::sync(library, host, [&session](const QByteArray &line) { return (*session)->ask(line); }, {}, options);
    }

private:
    [[nodiscard]] HubLinks links() const { return HubLinks(m_folder); }

    std::function<QString()> m_root;
    QString m_folder;
};

// The texts of the snapshots' warnings, as the hub says them (#184).
[[nodiscard]] QStringList warningsOf(const QJsonObject &snapshots) {
    QStringList said;
    for (const auto &warning : snapshots.value("warnings"_L1).toArray())
        said << warning["text"_L1].toString();
    return said;
}

// The hub's phones and e-readers as the panel shows them, why it can't serve them said here, in the window's language.
[[nodiscard]] QVariantMap devicesShown(const QJsonObject &devices) {
    QVariantMap shown = devices.toVariantMap();
    const QString problem = devices.value("problem"_L1).toString();
    const QString port = QString::number(devices.value("options"_L1).toObject().value("port"_L1).toInt());
    if (problem == u"no-tailnet"_s)
        shown.insert(u"said"_s, HubDesk::tr("The hub isn't on a tailnet yet. Start Tailscale there, or choose another reach."));
    else if (problem == u"port-taken"_s)
        shown.insert(u"said"_s, HubDesk::tr("Another program on the hub is using port %1. Choose another port.").arg(port));
    return shown;
}

} // namespace

std::unique_ptr<HubDesk::Hubs> HubDesk::linkedHere(std::function<QString()> root, QString folder) {
    return std::make_unique<LinkedHere>(std::move(root), std::move(folder));
}

HubDesk::HubDesk(std::unique_ptr<Hubs> hubs, QObject *parent) : QObject(parent), m_hubs(std::move(hubs)), m_linked(m_hubs->linked()) {}

HubDesk::HubDesk(std::unique_ptr<Hubs> hubs, Automatic automatic, QObject *parent)
    : QObject(parent), m_hubs(std::move(hubs)), m_linked(m_hubs->linked()), m_automatic(std::move(automatic)),
      m_auto(std::make_unique<HubAutoSync>(m_automatic.folder, m_automatic.times)) {
    m_ticking.setInterval(m_automatic.every);
    connect(&m_ticking, &QTimer::timeout, this, &HubDesk::tick);
    m_ticking.start();
}

HubDesk::~HubDesk() {
    // A job still asking a hub uses the hubs this owns: let it finish first.
    m_job.waitForFinished();
}

QString HubDesk::thisComputer() { return HubLinks::thisComputer(); }

QVariantList HubDesk::hubs() const {
    QVariantList hubs;
    for (const HubLinks::Hub &hub : m_linked) {
        QVariantMap shown = m_states.value(hub.host);
        shown.insert(u"host"_s, hub.host);
        shown.insert(u"computer"_s, hub.computer);
        shown.insert(u"fingerprint"_s, hub.fingerprint);
        shown.insert(u"linked"_s, hub.linked);
        if (!shown.contains(u"state"_s))
            shown.insert(u"state"_s, QString());
        if (m_auto) {
            const HubAutoSync::Hub kept = m_auto->hub(hub.host);
            shown.insert(u"auto"_s, kept.on);
            shown.insert(u"autoLast"_s, kept.last);
            shown.insert(u"autoRefused"_s, kept.on ? kept.refused : QString());
            shown.insert(u"autoUnreached"_s, kept.on && kept.unreached);
            shown.insert(u"autoWaiting"_s, kept.on ? SyncWait::name(m_waiting.value(hub.host)) : QString());
        }
        hubs << shown;
    }
    return hubs;
}

QVariantMap HubDesk::standing(const Hubs &hubs, const QString &host) {
    const auto status = hubs.ask(host, {{"request", "status"}});
    if (!status) {
        const bool unlinked = status.error().code() == Failure::Code::HubNotLinked;
        return {{u"state"_s, unlinked ? u"unlinked"_s : u"unreachable"_s}, {u"why"_s, status.error().text()}};
    }
    QVariantMap shown{{u"state"_s, u"reachable"_s},
                      {u"version"_s, status->value("version"_L1).toString()},
                      {u"libraries"_s, status->value("libraries"_L1).toArray().toVariantList()}};
    if (const auto computers = hubs.ask(host, {{"request", "computers"}}); computers)
        shown.insert(u"computers"_s, computers->value("computers"_L1).toArray().toVariantList());
    // Its background work (#192), which a hub of before it doesn't do.
    if (const auto work = hubs.ask(host, {{"request", "work"}}); work)
        shown.insert(u"work"_s, work->toVariantMap());
    // Its phones and e-readers (#193), which a hub of before it doesn't serve.
    if (const auto devices = hubs.ask(host, {{"request", "devices"}}); devices)
        shown.insert(u"devices"_s, devicesShown(*devices));
    // A hub of before #184 has no word of its snapshots: nothing is shown of them.
    if (const auto snapshots = hubs.ask(host, {{"request", "snapshots"}}); snapshots) {
        shown.insert(u"snapshots"_s, snapshots->value("set_up"_L1).toBool());
        shown.insert(u"warnings"_s, warningsOf(*snapshots));
    }
    return shown;
}

void HubDesk::work(const QString &doing, const QString &host, std::function<Outcome()> job) {
    if (!m_doing.isEmpty())
        return;
    m_doing = doing;
    m_doingHost = host;
    // What the person did last stays said while a sync of its own runs.
    if (doing != u"auto"_s) {
        m_status.clear();
        m_failed = false;
        m_unpaired.clear();
    }
    emit changed();
    m_job = QtConcurrent::run([this, doing, host, job = std::move(job)] {
        const Outcome outcome = job();
        QMetaObject::invokeMethod(this, [this, doing, host, outcome] { finish(doing, host, outcome); });
    });
}

void HubDesk::finish(const QString &doing, const QString &host, const Outcome &outcome) {
    m_doing.clear();
    m_doingHost.clear();
    m_linked = m_hubs->linked();
    for (const auto &[each, state] : outcome.states.asKeyValueRange())
        m_states.insert(each, state);
    // A hub no longer linked has no state left to show.
    for (const QString &each : m_states.keys())
        if (std::ranges::none_of(m_linked, [&each](const HubLinks::Hub &hub) { return hub.host == each; }))
            m_states.remove(each);
    bool wrote = !outcome.synced.isEmpty() && !outcome.synced.value(u"dryRun"_s).toBool();
    if (doing != u"auto"_s) {
        m_status = outcome.said;
        m_failed = outcome.failed;
        m_unpaired = outcome.unpaired;
        if (!outcome.synced.isEmpty())
            m_synced = outcome.synced;
    }
    if (m_auto && outcome.byHand)
        m_auto->byHand(host, *outcome.byHand, outcome.synced.value(u"brought"_s).toBool()); // a change for the other hubs (#196)
    if (m_auto && outcome.automatic) {
        wrote = outcome.automatic->error.isEmpty();
        if (const auto tell = m_auto->done(host, *outcome.automatic, m_automatic.now()); tell && m_automatic.notify) {
            const auto [title, body] = HubAutoSync::notice(host, *tell);
            m_automatic.notify(title, body);
        }
    }
    emit changed();
    if (wrote) {
        // What the sync brought is no change of the person's to sync again.
        m_ownChange = true;
        emit booksChanged();
        m_ownChange = false;
    }
}

void HubDesk::refresh() {
    m_linked = m_hubs->linked();
    emit changed();
    if (m_linked.isEmpty())
        return;
    QStringList hosts;
    for (const HubLinks::Hub &hub : std::as_const(m_linked))
        hosts << hub.host;
    work(u"status"_s, {}, [hubs = m_hubs.get(), hosts] {
        Outcome outcome;
        for (const QString &host : hosts)
            outcome.states.insert(host, standing(*hubs, host));
        return outcome;
    });
}

void HubDesk::link(const QString &host, const QString &computer) {
    const QString reached = host.trimmed();
    const QString name = computer.trimmed().isEmpty() ? thisComputer() : computer.trimmed();
    if (reached.isEmpty() || !HubComputers::isName(name)) {
        m_status = reached.isEmpty() ? tr("Say where the hub is: its name on your network or tailnet, as ssh reaches it.")
                                     : Failure(Failure::Code::HubBadComputerName, {name}).text();
        m_failed = true;
        emit changed();
        return;
    }
    work(u"link"_s, reached, [hubs = m_hubs.get(), reached, name] {
        const auto linked = hubs->add(reached, name);
        if (!linked)
            return Outcome{
                .said = linked.error().text(), .failed = true, .states = {}, .synced = {}, .unpaired = {}, .automatic = {}, .byHand = {}};
        return Outcome{.said =
                           tr("This computer is linked to the hub at %1 as %2. Its key there opens only the hub's session, never a shell.")
                               .arg(reached, linked->computer),
                       .failed = false,
                       .states = {{reached, standing(*hubs, reached)}},
                       .synced = {},
                       .unpaired = {},
                       .automatic = {},
                       .byHand = {}};
    });
}

void HubDesk::unlink(const QString &host) {
    work(u"unlink"_s, host, [hubs = m_hubs.get(), host] {
        const auto removed = hubs->remove(host);
        if (!removed)
            return Outcome{
                .said = removed.error().text(), .failed = true, .states = {}, .synced = {}, .unpaired = {}, .automatic = {}, .byHand = {}};
        return Outcome{.said = tr("This computer is no longer linked to the hub at %1: its key there is gone.").arg(host),
                       .failed = false,
                       .states = {},
                       .synced = {},
                       .unpaired = {},
                       .automatic = {},
                       .byHand = {}};
    });
}

void HubDesk::forget(const QString &host, const QString &computer) {
    work(u"forget"_s, host, [hubs = m_hubs.get(), host, computer] {
        const auto forgotten = hubs->ask(host, {{"request", "forget"}, {"name", computer}});
        Outcome outcome{.said = forgotten ? tr("The computer %1 is no longer linked to the hub at %2: its key is gone.").arg(computer, host)
                                          : forgotten.error().text(),
                        .failed = !forgotten,
                        .states = {},
                        .synced = {},
                        .unpaired = {},
                        .automatic = {},
                        .byHand = {}};
        outcome.states.insert(host, standing(*hubs, host));
        return outcome;
    });
}

void HubDesk::cancelWork(const QString &host) {
    work(u"cancel"_s, host, [hubs = m_hubs.get(), host] {
        const auto cancelled = hubs->ask(host, {{"request", "work-cancel"}});
        Outcome outcome{.said = {}, .failed = !cancelled, .states = {}, .synced = {}, .unpaired = {}, .automatic = {}, .byHand = {}};
        if (!cancelled)
            outcome.said = cancelled.error().text();
        else if (cancelled->value("cancelled"_L1).toBool())
            outcome.said = tr("The work under way on the hub at %1 is stopped; nothing of it is kept.").arg(host);
        else
            outcome.said = tr("The hub at %1 has no work under way.").arg(host);
        outcome.states.insert(host, standing(*hubs, host));
        return outcome;
    });
}

void HubDesk::setDevices(const QString &host, const QVariantMap &options, const QString &password, bool newToken) {
    QJsonObject asked{{"request", "devices-set"}, {"options", QJsonObject::fromVariantMap(options)}};
    if (!password.isEmpty())
        asked.insert("password"_L1, password);
    if (newToken)
        asked.insert("new_token"_L1, true);
    work(u"devices"_s, host, [hubs = m_hubs.get(), host, asked] {
        const auto set = hubs->ask(host, asked);
        Outcome outcome{.said = set ? QString() : set.error().text(),
                        .failed = !set,
                        .states = {},
                        .synced = {},
                        .unpaired = {},
                        .automatic = {},
                        .byHand = {}};
        outcome.states.insert(host, standing(*hubs, host));
        return outcome;
    });
}

void HubDesk::syncNow(const QString &host, bool dryRun) { sync(host, dryRun, false); }

void HubDesk::pair() {
    if (!m_unpaired.isEmpty() && m_doing.isEmpty())
        sync(m_unpaired.value(u"host"_s).toString(), m_unpaired.value(u"dryRun"_s).toBool(), true);
}

void HubDesk::sync(const QString &host, bool dryRun, bool pair) {
    const QString folder = m_auto ? m_automatic.folder : QString();
    const QDateTime began = m_auto ? m_automatic.now() : QDateTime();
    work(dryRun ? u"preview"_s : u"sync"_s, host, [hubs = m_hubs.get(), host, dryRun, pair, folder, began] {
        SyncOptions options;
        options.dryRun = dryRun;
        options.pair = pair;
        // After the background service's sync, when it is running one.
        const auto held = folder.isEmpty() ? nullptr : HubAutoSync(folder).hold(true);
        const SyncSummary summary = hubs->sync(host, options);
        const bool failed = !summary.error.isEmpty();
        Outcome outcome{.said = failed ? summary.error : QString(),
                        .failed = failed,
                        .states = {},
                        .synced = {},
                        .unpaired = {},
                        .automatic = {},
                        .byHand = {}};
        if (!failed)
            outcome.synced = {{u"host"_s, host},
                              {u"dryRun"_s, dryRun},
                              {u"lines"_s, Backend::syncLines(summary)},
                              {u"brought"_s, !dryRun && HubAutoSync::brought(summary)}};
        if (!summary.unpairedThere.isEmpty())
            outcome.unpaired = {
                {u"host"_s, host}, {u"there"_s, summary.unpairedThere}, {u"here"_s, summary.unpairedHere}, {u"dryRun"_s, dryRun}};
        // Through by hand: automatic sync goes on from it, a refusal seen to (#191).
        if (!failed && !dryRun && began.isValid())
            outcome.byHand = began;
        // The hub's libraries as the sync left them.
        if (!dryRun)
            outcome.states.insert(host, standing(*hubs, host));
        return outcome;
    });
}

void HubDesk::clear() {
    if (m_status.isEmpty() && m_synced.isEmpty() && m_unpaired.isEmpty())
        return;
    m_status.clear();
    m_synced.clear();
    m_unpaired.clear();
    m_failed = false;
    emit changed();
}

bool HubDesk::background() const { return m_auto && ServeService(m_automatic.service, ServeService::hubSync()).installed(); }

void HubDesk::setAutomatic(const QString &host, bool on) {
    if (!m_auto)
        return;
    if (const auto turned = m_auto->turn(host, on); !turned) {
        m_status = turned.error().text();
        m_failed = true;
    }
    m_waiting.remove(host);
    emit changed();
    if (on)
        tick();
}

void HubDesk::setBackground(bool on) {
    if (!m_auto)
        return;
    work(u"background"_s, {}, [service = ServeService(m_automatic.service, ServeService::hubSync()), on] {
        const auto done = on ? service.install() : service.remove();
        if (!done)
            return Outcome{
                .said = done.error().text(), .failed = true, .states = {}, .synced = {}, .unpaired = {}, .automatic = {}, .byHand = {}};
        return Outcome{.said = on ? tr("The hubs switched on sync by themselves with the window closed too, from login on.")
                                  : tr("The hubs sync by themselves only while the window is open."),
                       .failed = false,
                       .states = {},
                       .synced = {},
                       .unpaired = {},
                       .automatic = {},
                       .byHand = {}};
    });
}

void HubDesk::tick() {
    if (!m_auto || !m_doing.isEmpty())
        return;
    QStringList hosts;
    for (const HubLinks::Hub &hub : std::as_const(m_linked))
        hosts << hub.host;
    const QDateTime now = m_automatic.now();
    const auto host = m_auto->due(hosts, now);
    if (!host)
        return;
    // Asked only when a sync is due: the desktop may take a moment to say.
    if (const SyncWait::Reason wait = m_automatic.wait(); wait != SyncWait::Reason::None) {
        if (std::exchange(m_waiting[*host], wait) != wait)
            emit changed();
        return;
    }
    m_waiting.remove(*host);
    m_auto->began(now);
    work(u"auto"_s, *host, [hubs = m_hubs.get(), host = *host, folder = m_automatic.folder] {
        Outcome outcome{.said = {}, .failed = false, .states = {}, .synced = {}, .unpaired = {}, .automatic = {}, .byHand = {}};
        // Another Omabook of this computer's is syncing: the next look.
        if (const auto held = HubAutoSync(folder).hold(); held)
            outcome.automatic = hubs->sync(host, {});
        return outcome;
    });
}

void HubDesk::libraryChanged() {
    if (m_auto && !m_ownChange)
        m_auto->changed(m_automatic.now());
}

void HubDesk::touched() {
    if (m_auto)
        m_auto->touched(m_automatic.now());
}

void HubDesk::cameIntoReach() {
    if (!m_auto)
        return;
    m_auto->cameIntoReach();
    tick();
}

bool HubDesk::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseButtonPress:
    case QEvent::Wheel:
    case QEvent::TouchBegin:
        touched();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
