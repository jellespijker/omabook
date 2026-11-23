#include "hubdesk.h"

#include <QJsonArray>
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

} // namespace

std::unique_ptr<HubDesk::Hubs> HubDesk::linkedHere(std::function<QString()> root, QString folder) {
    return std::make_unique<LinkedHere>(std::move(root), std::move(folder));
}

HubDesk::HubDesk(std::unique_ptr<Hubs> hubs, QObject *parent) : QObject(parent), m_hubs(std::move(hubs)), m_linked(m_hubs->linked()) {}

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
    m_status.clear();
    m_failed = false;
    m_unpaired.clear();
    emit changed();
    m_job = QtConcurrent::run([this, job = std::move(job)] {
        const Outcome outcome = job();
        QMetaObject::invokeMethod(this, [this, outcome] {
            m_doing.clear();
            m_doingHost.clear();
            m_linked = m_hubs->linked();
            for (const auto &[host, state] : outcome.states.asKeyValueRange())
                m_states.insert(host, state);
            // A hub no longer linked has no state left to show.
            for (const QString &host : m_states.keys())
                if (std::ranges::none_of(m_linked, [&host](const HubLinks::Hub &hub) { return hub.host == host; }))
                    m_states.remove(host);
            m_status = outcome.said;
            m_failed = outcome.failed;
            m_unpaired = outcome.unpaired;
            const bool wrote = !outcome.synced.isEmpty() && !outcome.synced.value(u"dryRun"_s).toBool();
            if (!outcome.synced.isEmpty())
                m_synced = outcome.synced;
            emit changed();
            if (wrote)
                emit booksChanged();
        });
    });
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
            return Outcome{.said = linked.error().text(), .failed = true, .states = {}, .synced = {}, .unpaired = {}};
        return Outcome{.said =
                           tr("This computer is linked to the hub at %1 as %2. Its key there opens only the hub's session, never a shell.")
                               .arg(reached, linked->computer),
                       .failed = false,
                       .states = {{reached, standing(*hubs, reached)}},
                       .synced = {},
                       .unpaired = {}};
    });
}

void HubDesk::unlink(const QString &host) {
    work(u"unlink"_s, host, [hubs = m_hubs.get(), host] {
        const auto removed = hubs->remove(host);
        if (!removed)
            return Outcome{.said = removed.error().text(), .failed = true, .states = {}, .synced = {}, .unpaired = {}};
        return Outcome{.said = tr("This computer is no longer linked to the hub at %1: its key there is gone.").arg(host),
                       .failed = false,
                       .states = {},
                       .synced = {},
                       .unpaired = {}};
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
                        .unpaired = {}};
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
    work(dryRun ? u"preview"_s : u"sync"_s, host, [hubs = m_hubs.get(), host, dryRun, pair] {
        SyncOptions options;
        options.dryRun = dryRun;
        options.pair = pair;
        const SyncSummary summary = hubs->sync(host, options);
        const bool failed = !summary.error.isEmpty();
        Outcome outcome{.said = failed ? summary.error : QString(), .failed = failed, .states = {}, .synced = {}, .unpaired = {}};
        if (!failed)
            outcome.synced = {{u"host"_s, host}, {u"dryRun"_s, dryRun}, {u"lines"_s, Backend::syncLines(summary)}};
        if (!summary.unpairedThere.isEmpty())
            outcome.unpaired = {
                {u"host"_s, host}, {u"there"_s, summary.unpairedThere}, {u"here"_s, summary.unpairedHere}, {u"dryRun"_s, dryRun}};
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
