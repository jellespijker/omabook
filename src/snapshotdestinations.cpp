#include "snapshotdestinations.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <chrono>
#include <ranges>
#include <set>
#include <tuple>
#include <utility>

#include "log.h"

namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QString configOf(const QString &state) { return state + u"/destinations.json"_s; }
[[nodiscard]] QString statesOf(const QString &state) { return state + u"/destinations-state.json"_s; }

[[nodiscard]] QJsonObject readJson(const QString &file) {
    QFile in(file);
    return in.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(in.readAll()).object() : QJsonObject();
}

[[nodiscard]] bool writeJson(const QString &file, const QJsonObject &object) {
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return false;
    QSaveFile out(file);
    return out.open(QIODevice::WriteOnly) && out.write(QJsonDocument(object).toJson()) > 0 && out.commit();
}

// A name that can name the hub's secrets (HubPlace::secret).
[[nodiscard]] bool nameAllowed(const QString &name) {
    static const QRegularExpression allowed(u"^[a-z0-9][a-z0-9-]*$"_s);
    return allowed.match(name).hasMatch();
}

// A password between :// and @, as in rest:https://user:password@host/.
[[nodiscard]] bool holdsPassword(const QString &location) {
    static const QRegularExpression password(u"://[^/@:]+:[^/@]+@"_s);
    return password.match(location).hasMatch();
}

[[nodiscard]] bool within(const QString &inner, const QString &outer) { return inner == outer || inner.startsWith(outer + u'/'); }

// A second disk's repository is there when restic's config file is: when its
// disk is unplugged, its mount point is empty or gone.
[[nodiscard]] bool absent(const SnapshotDestination &destination) {
    return destination.kind() == SnapshotDestination::Kind::Disk && !QFileInfo::exists(destination.location + u"/config"_s);
}

[[nodiscard]] QString timeText(const QDateTime &time) { return time.isValid() ? time.toString(Qt::ISODateWithMs) : QString(); }

[[nodiscard]] QJsonObject jsonOf(const DestinationState &state) {
    return {{u"tried"_s, timeText(state.tried)},
            {u"copied"_s, timeText(state.copied)},
            {u"done"_s, state.done},
            {u"absent"_s, state.absent},
            {u"said"_s, state.said},
            {u"latest_id"_s, state.latestId},
            {u"latest"_s, timeText(state.latest)},
            {u"size"_s, state.size}};
}

[[nodiscard]] DestinationState stateFrom(const QJsonObject &json) {
    return {.tried = QDateTime::fromString(json.value("tried"_L1).toString(), Qt::ISODateWithMs),
            .copied = QDateTime::fromString(json.value("copied"_L1).toString(), Qt::ISODateWithMs),
            .done = json.value("done"_L1).toBool(),
            .absent = json.value("absent"_L1).toBool(),
            .said = json.value("said"_L1).toString(),
            .latestId = json.value("latest_id"_L1).toString(),
            .latest = QDateTime::fromString(json.value("latest"_L1).toString(), Qt::ISODateWithMs),
            .size = json.value("size"_L1).toInteger(-1)};
}

// How long a place away from home has to answer before a copy or a test, rather than restic's minutes of trying.
constexpr std::chrono::seconds answerWithin{30};

// The way to the destinations on hubs, as the program set it.
[[nodiscard]] SnapshotDestinations::HubReach &hubReach() {
    static SnapshotDestinations::HubReach reach;
    return reach;
}

constexpr auto hubPrefix = "hub:"_L1;

// A host as ssh is given it: no option, no space, nothing that isn't a name.
[[nodiscard]] bool hostAllowed(const QString &host) {
    static const QRegularExpression allowed(u"^[A-Za-z0-9_.][A-Za-z0-9_.@-]*$"_s);
    return allowed.match(host).hasMatch();
}

// A place away reached first; a disk is there or not.
[[nodiscard]] std::expected<void, Failure> reached(const SnapshotDestination &destination, const ResticRepository &repository) {
    if (destination.kind() == SnapshotDestination::Kind::Disk)
        return {};
    return Restic::answers(repository, answerWithin);
}

[[nodiscard]] Failure unkept(const Failure &why) { return {Failure::Code::SnapshotPasswordUnkept, {why.text()}}; }

} // namespace

SnapshotDestination::Kind SnapshotDestination::kind() const {
    if (location.startsWith(hubPrefix))
        return Kind::Hub;
    return QDir::isAbsolutePath(location) || !location.contains(u':') ? Kind::Disk : Kind::Away;
}

QString SnapshotDestination::hub() const { return kind() == Kind::Hub ? location.mid(hubPrefix.size()) : QString(); }

QString SnapshotDestination::kindName(Kind kind) {
    switch (kind) {
    case Kind::Disk:
        return u"disk"_s;
    case Kind::Hub:
        return u"hub"_s;
    case Kind::Away:
        break;
    }
    return u"away"_s;
}

SnapshotDestinations::SnapshotDestinations(SnapshotPlace place) : m_place(std::move(place)) {}

void SnapshotDestinations::reachHubsBy(HubReach reach) { hubReach() = std::move(reach); }

std::expected<ResticRepository, Failure> SnapshotDestinations::placeOf(const SnapshotDestination &destination) {
    if (destination.kind() != SnapshotDestination::Kind::Hub)
        return ResticRepository{.location = destination.location};
    if (!hubReach())
        return std::unexpected(Failure(Failure::Code::HubNotLinked, {destination.hub()}));
    return hubReach()(destination.hub(), destination.name);
}

QString SnapshotDestinations::passwordSecret(const QString &name) { return u"destination-"_s + name; }
QString SnapshotDestinations::credentialsSecret(const QString &name) { return u"credentials-"_s + name; }

// The hub's own secrets on a hub; elsewhere the same kind of 0600 files, in the snapshots' state.
HubPlace SnapshotDestinations::secrets() const {
    return m_place.hub ? *m_place.hub : HubPlace({.state = m_place.state, .runtime = {}, .credentials = {}});
}

QList<SnapshotDestination> SnapshotDestinations::list() const {
    QList<SnapshotDestination> destinations;
    for (const QJsonValue value : readJson(configOf(m_place.state)).value("destinations"_L1).toArray()) {
        const QJsonObject json = value.toObject();
        QStringList credentials;
        for (const QJsonValue credential : json.value("credentials"_L1).toArray())
            credentials << credential.toString();
        destinations << SnapshotDestination{
            .name = json.value("name"_L1).toString(), .location = json.value("location"_L1).toString(), .credentials = credentials};
    }
    return destinations;
}

std::optional<SnapshotDestination> SnapshotDestinations::find(const QString &name) const {
    const QList<SnapshotDestination> destinations = list();
    const auto found = std::ranges::find(destinations, name, &SnapshotDestination::name);
    return found == destinations.end() ? std::nullopt : std::optional(*found);
}

std::optional<DestinationState> SnapshotDestinations::stateOf(const QString &name) const {
    const QJsonObject states = readJson(statesOf(m_place.state));
    if (!states.contains(name))
        return std::nullopt;
    return stateFrom(states.value(name).toObject());
}

bool SnapshotDestinations::write(const QList<SnapshotDestination> &destinations) const {
    QJsonArray array;
    for (const SnapshotDestination &destination : destinations)
        array << QJsonObject{{u"name"_s, destination.name},
                             {u"location"_s, destination.location},
                             {u"kind"_s, SnapshotDestination::kindName(destination.kind())},
                             {u"credentials"_s, QJsonArray::fromStringList(destination.credentials)}};
    return writeJson(configOf(m_place.state), {{u"destinations"_s, array}});
}

QJsonObject SnapshotDestinations::json(const SnapshotDestination &destination) const {
    const std::optional<DestinationState> state = stateOf(destination.name);
    QJsonObject json{{u"name"_s, destination.name},
                     {u"location"_s, destination.location},
                     {u"kind"_s, SnapshotDestination::kindName(destination.kind())},
                     {u"credentials"_s, QJsonArray::fromStringList(destination.credentials)},
                     {u"copied"_s, state && state->done}};
    if (state)
        json.insert(u"last"_s, QJsonObject{{u"tried"_s, state->tried.toString(Qt::ISODate)},
                                           {u"copied"_s, state->copied.toString(Qt::ISODate)},
                                           {u"absent"_s, state->absent},
                                           {u"said"_s, state->said},
                                           {u"latest_id"_s, state->latestId},
                                           {u"latest"_s, state->latest.toString(Qt::ISODate)},
                                           {u"size"_s, state->size}});
    return json;
}

void SnapshotDestinations::keepState(const QString &name, const DestinationState &state) const {
    QJsonObject states = readJson(statesOf(m_place.state));
    states.insert(name, jsonOf(state));
    if (!writeJson(statesOf(m_place.state), states))
        qCWarning(lcLibrary).noquote() << "How the copy to" << name << "went couldn't be kept in" << m_place.state;
}

// The destination as it is kept, its folder made whole, or why it can't be one.
std::expected<SnapshotDestination, Failure> SnapshotDestinations::checked(SnapshotDestination destination) const {
    if (!nameAllowed(destination.name))
        return std::unexpected(Failure(Failure::Code::DestinationNameInvalid, {destination.name}));
    if (find(destination.name))
        return std::unexpected(Failure(Failure::Code::DestinationNameTaken, {destination.name}));
    if (!Snapshots(m_place, {}).ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    if (holdsPassword(destination.location))
        return std::unexpected(Failure(Failure::Code::DestinationPasswordInPlace, {destination.location}));
    const QString repository = m_place.repository.location;
    if (destination.kind() == SnapshotDestination::Kind::Disk) {
        destination.location = QDir::cleanPath(QDir(destination.location).absolutePath());
        if (within(destination.location, repository) || within(repository, destination.location))
            return std::unexpected(Failure(Failure::Code::DestinationIsTheRepository, {repository}));
        if (!QFileInfo(QFileInfo(destination.location).absolutePath()).isDir())
            return std::unexpected(Failure(Failure::Code::DestinationAbsent, {destination.name, destination.location}));
    } else if (destination.location == repository) {
        return std::unexpected(Failure(Failure::Code::DestinationIsTheRepository, {repository}));
    } else if (destination.kind() == SnapshotDestination::Kind::Hub && !hostAllowed(destination.hub())) {
        return std::unexpected(Failure(Failure::Code::HubNotLinked, {destination.hub()}));
    }
    return destination;
}

std::expected<QString, Failure> SnapshotDestinations::add(const SnapshotDestination &asked,
                                                          const QList<std::pair<QString, QByteArray>> &credentials) {
    const auto checkedOne = checked(asked);
    if (!checkedOne)
        return std::unexpected(checkedOne.error());
    const SnapshotDestination &destination = *checkedOne;
    auto place = placeOf(destination);
    if (!place)
        return std::unexpected(place.error());
    ResticRepository made = std::move(*place);
    made.password = Snapshots::newRecoveryKey().toLatin1();
    QJsonObject kept;
    for (const QString &name : destination.credentials) {
        const auto given = std::ranges::find(credentials, name, &std::pair<QString, QByteArray>::first);
        if (given == credentials.end() || given->second.isEmpty())
            return std::unexpected(Failure(Failure::Code::DestinationCredentialMissing, {name}));
        made.environment.append(*given);
        kept.insert(name, QString::fromUtf8(given->second));
    }
    if (const auto initialised = Restic::init(made, m_place.repository); !initialised)
        return std::unexpected(initialised.error());
    const HubPlace secret = secrets();
    if (const auto password = secret.keepSecret(passwordSecret(destination.name), made.password); !password)
        return std::unexpected(unkept(password.error()));
    if (!kept.isEmpty())
        if (const auto given = secret.keepSecret(credentialsSecret(destination.name), QJsonDocument(kept).toJson(QJsonDocument::Compact));
            !given)
            return std::unexpected(unkept(given.error()));
    QList<SnapshotDestination> destinations = list();
    destinations << destination;
    if (!write(destinations))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {m_place.state}));
    qCInfo(lcLibrary).noquote() << "snapshots go to" << destination.name << "too," << destination.location;
    return QString::fromLatin1(made.password);
}

std::expected<void, Failure> SnapshotDestinations::remove(const QString &name) {
    QList<SnapshotDestination> destinations = list();
    if (std::ranges::find(destinations, name, &SnapshotDestination::name) == destinations.end())
        return std::unexpected(Failure(Failure::Code::DestinationUnknown, {name}));
    destinations.removeIf([&name](const SnapshotDestination &destination) { return destination.name == name; });
    if (!write(destinations))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {m_place.state}));
    secrets().forgetSecret(passwordSecret(name));
    secrets().forgetSecret(credentialsSecret(name));
    QJsonObject states = readJson(statesOf(m_place.state));
    states.remove(name);
    std::ignore = writeJson(statesOf(m_place.state), states); // a state left over is shown for no destination
    return {};
}

std::expected<ResticRepository, Failure> SnapshotDestinations::repositoryOf(const SnapshotDestination &destination) const {
    const HubPlace place = secrets();
    const auto password = place.secret(passwordSecret(destination.name));
    if (!password)
        return std::unexpected(password.error());
    auto reached = placeOf(destination);
    if (!reached)
        return std::unexpected(reached.error());
    ResticRepository repository = std::move(*reached);
    repository.password = password->trimmed();
    if (destination.credentials.isEmpty())
        return repository;
    const auto given = place.secret(credentialsSecret(destination.name));
    if (!given)
        return std::unexpected(given.error());
    const QJsonObject values = QJsonDocument::fromJson(*given).object();
    for (const QString &name : destination.credentials) {
        if (!values.contains(name))
            return std::unexpected(Failure(Failure::Code::DestinationCredentialMissing, {name}));
        repository.environment.append({name, values.value(name).toString().toUtf8()});
    }
    return repository;
}

std::expected<SnapshotDestinations::Tested, Failure> SnapshotDestinations::test(const QString &name) const {
    const auto destination = find(name);
    if (!destination)
        return std::unexpected(Failure(Failure::Code::DestinationUnknown, {name}));
    if (absent(*destination))
        return std::unexpected(Failure(Failure::Code::DestinationAbsent, {name, destination->location}));
    const auto repository = repositoryOf(*destination);
    if (!repository)
        return std::unexpected(repository.error());
    if (const auto answered = reached(*destination, *repository); !answered)
        return std::unexpected(answered.error());
    const auto held = Restic::snapshots(*repository, {u"omabook"_s});
    if (!held)
        return std::unexpected(held.error());
    return Tested{.snapshots = held->size(), .latest = held->isEmpty() ? std::nullopt : std::optional(held->last())};
}

// Forgets there what the retention keeps no longer, library by library, as Snapshots::thin does here.
std::expected<void, Failure> SnapshotDestinations::thin(const ResticRepository &repository, const QDateTime &now) const {
    const auto held = Restic::snapshots(repository, {u"omabook"_s});
    if (!held)
        return std::unexpected(held.error());
    std::set<QString> libraries;
    for (const Restic::Snapshot &snapshot : *held)
        if (const QString library = snapshot.tagged(u"library:"_s); !library.isEmpty())
            libraries.insert(u"library:"_s + library);
    const Snapshots there(SnapshotPlace{.state = m_place.state, .repository = repository, .hub = std::nullopt}, {});
    for (const QString &library : libraries)
        if (const auto thinned = there.thin(library, now); !thinned)
            return std::unexpected(thinned.error());
    return {};
}

std::expected<DestinationState, Failure> SnapshotDestinations::copy(const QString &name, const QDateTime &now) const {
    const auto destination = find(name);
    if (!destination)
        return std::unexpected(Failure(Failure::Code::DestinationUnknown, {name}));
    if (!Snapshots(m_place, {}).ready())
        return std::unexpected(Failure(Failure::Code::SnapshotsNotSetUp));
    DestinationState state = stateOf(name).value_or(DestinationState{});
    state.tried = now;
    const auto failed = [&](const Failure &failure) -> std::expected<DestinationState, Failure> {
        state.done = false;
        state.absent = failure.code() == Failure::Code::DestinationAbsent;
        state.said = failure.english();
        keepState(name, state);
        qCWarning(lcLibrary).noquote() << "The snapshots couldn't be copied to" << name << "-" << state.said;
        return std::unexpected(failure);
    };
    if (absent(*destination))
        return failed(Failure(Failure::Code::DestinationAbsent, {name, destination->location}));
    const auto repository = repositoryOf(*destination);
    if (!repository)
        return failed(repository.error());
    if (const auto answered = reached(*destination, *repository); !answered)
        return failed(answered.error());
    if (const auto copied = Restic::copy(*repository, m_place.repository, {u"omabook"_s}); !copied)
        return failed(copied.error());
    if (const auto thinned = thin(*repository, now); !thinned)
        return failed(thinned.error());
    const auto held = Restic::snapshots(*repository, {u"omabook"_s});
    if (!held)
        return failed(held.error());
    state = DestinationState{.tried = now,
                             .copied = now,
                             .done = true,
                             .absent = false,
                             .said = {},
                             .latestId = held->isEmpty() ? QString() : held->last().id,
                             .latest = held->isEmpty() ? QDateTime() : held->last().time,
                             .size = Restic::size(*repository).value_or(-1)};
    keepState(name, state);
    qCInfo(lcLibrary).noquote() << "snapshots copied to" << name;
    return state;
}

SnapshotDestinations::Round SnapshotDestinations::copyAll(const QDateTime &now) const {
    Round round;
    for (const SnapshotDestination &destination : list()) {
        if (const auto copied = copy(destination.name, now); copied)
            round.copied << destination.name;
        else
            round.failed.append({destination.name, copied.error()});
    }
    return round;
}

bool SnapshotDestinations::backedUp() const {
    bool disk = false;
    bool away = false;
    for (const SnapshotDestination &destination : list()) {
        if (const auto state = stateOf(destination.name); !state || !state->done)
            continue;
        (destination.kind() == SnapshotDestination::Kind::Disk ? disk : away) = true;
    }
    return Snapshots(m_place, {}).ready() && disk && away;
}
