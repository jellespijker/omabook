#include "hubdevices.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>
#include <QUrl>
#include <optional>
#include <utility>

#include "deviceserver.h"
#include "hubkeeper.h"
#include "hubrequests.h"
#include "kosyncclient.h"
#include "library.h"
#include "log.h"

using namespace Qt::StringLiterals;

namespace {

// What a hub serves as when nothing was chosen: nothing, until a computer says where.
[[nodiscard]] QJsonObject unchosen() {
    return {{"serve", "off"},          {"port", DeviceOptions{}.port}, {"opds", true}, {"opds_collection", ""},
            {"koreader_serve", false}, {"koreader_serve_user", ""},    {"library", ""}};
}

[[nodiscard]] Failure badChoice(const QString &key, const QJsonValue &value) {
    return {Failure::Code::HubDevicesChoice, {key, value.toVariant().toString()}};
}

// A port, as a number; typed into the window it may come as text. None when it can't be one.
[[nodiscard]] std::optional<QJsonValue> portOf(const QJsonValue &value) {
    const int port = value.isString() ? value.toString().toInt() : value.toInt();
    if (port > 0 && port < 65536)
        return port;
    return std::nullopt;
}

// The choice as it is kept, by the kind of value its key takes; none when it can't be one.
[[nodiscard]] std::optional<QJsonValue> keptAs(const QString &key, const QJsonValue &value) {
    if (key == u"port"_s)
        return portOf(value);
    const bool words = (key == u"opds_collection"_s || key == u"koreader_serve_user"_s) && value.isString();
    if (words)
        return value.toString().trimmed();
    const bool reach = key == u"serve"_s && DeviceOptions::reachNamed(value.toString());
    const bool flag = (key == u"opds"_s || key == u"koreader_serve"_s) && value.isBool();
    const bool library = key == u"library"_s && value.isString();
    if (reach || flag || library)
        return value;
    return std::nullopt;
}

// The choice as it is kept, or why it can't be one.
[[nodiscard]] std::expected<QJsonValue, Failure> checked(const QString &key, const QJsonValue &value, const QStringList &libraries) {
    if (key == u"library"_s && !value.toString().isEmpty() && !libraries.contains(value.toString()))
        return std::unexpected(Failure(Failure::Code::HubNoSuchLibrary, {value.toString()}));
    const auto kept = keptAs(key, value);
    if (!kept)
        return std::unexpected(badChoice(key, value));
    return *kept;
}

[[nodiscard]] QJsonObject settingsIn(const QString &file) {
    QFile settings(file);
    if (!settings.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(settings.readAll()).object();
}

// Why it can't serve, as a computer words it in its own language.
[[nodiscard]] QString problemOf(const Failure &failure) {
    switch (failure.code()) {
    case Failure::Code::NoTailnet:
        return u"no-tailnet"_s;
    case Failure::Code::PortTaken:
    case Failure::Code::PortTakenBy:
    case Failure::Code::ServedByWindow:
    case Failure::Code::ServedByService:
    case Failure::Code::ServedByCommand:
    case Failure::Code::ServedByHub:
        return u"port-taken"_s;
    default:
        return u"other"_s;
    }
}

} // namespace

HubDevices::HubDevices(HubKeeper &keeper, HubPlace place) : HubDevices(keeper, std::move(place), Around{}) {}

HubDevices::HubDevices(HubKeeper &keeper, HubPlace place, Around around)
    : m_keeper(keeper), m_place(std::move(place)), m_around(std::move(around)) {
    // Each request in the keeper's turn, with the library chosen when it started.
    m_server = std::make_unique<DeviceServer>([this](const DeviceServer::Work &work) -> std::expected<HttpResponse, Failure> {
        std::optional<HttpResponse> answer;
        const auto done = m_keeper.write(m_library, HubKeeper::Moment::None, [&](Library &library) -> std::expected<QJsonObject, Failure> {
            answer = work(library);
            return QJsonObject{};
        });
        if (!done)
            return std::unexpected(done.error());
        return *std::move(answer);
    });
    m_again.setSingleShot(true);
    QObject::connect(&m_again, &QTimer::timeout, &m_again, [this] { follow(); });
}

HubDevices::~HubDevices() = default;

QJsonObject HubDevices::chosen() const {
    QJsonObject choices = unchosen();
    const QJsonObject kept = settingsIn(m_place.settings()).value("devices"_L1).toObject();
    for (auto it = kept.constBegin(); it != kept.constEnd(); ++it)
        if (choices.contains(it.key()))
            choices.insert(it.key(), it.value());
    return choices;
}

QString HubDevices::served() const {
    const QStringList names = m_keeper.names();
    const QString library = chosen().value("library"_L1).toString();
    return names.contains(library) ? library : names.value(0);
}

QString HubDevices::secret(const char *name) const {
    const auto kept = m_place.secret(QString::fromLatin1(name));
    return kept ? QString::fromUtf8(*kept).trimmed() : QString();
}

void HubDevices::follow() {
    m_again.stop();
    m_server->stop();
    m_address.clear();
    m_named.clear();
    m_problem.clear();
    m_said.clear();
    const DeviceOptions options = DeviceOptions::fromJson(chosen());
    m_library = served();
    if (options.reach == DeviceOptions::Reach::Off || (!options.opds && !options.koreaderServe))
        return;
    QString token = secret(DeviceSecrets::opdsToken);
    if (token.isEmpty() && options.opds) {
        token = DeviceSecrets::madeToken();
        if (const auto kept = m_place.keepSecret(QString::fromLatin1(DeviceSecrets::opdsToken), token.toUtf8()); !kept) {
            qCWarning(lcHub).noquote() << "The catalog's token can't be kept, so no app is let in:" << kept.error().english();
            token.clear();
        }
    }
    const auto address = m_server->start(
        {.options = options, .opdsToken = token, .koreaderKey = secret(DeviceSecrets::koreaderServeKey), .by = DeviceServing::By::Hub},
        m_around.interfaces());
    if (!address) {
        m_problem = problemOf(address.error());
        m_said = address.error().english();
        qCWarning(lcHub).noquote() << "Phones and e-readers can't be served as chosen, tried again in a minute:" << m_said;
        m_again.start(again);
        return;
    }
    m_address = *address;
    lookUpName(options.reach, m_address);
    qCInfo(lcHub).noquote() << "Serving phones and e-readers the library" << m_library << "at" << m_address;
}

void HubDevices::lookUpName(DeviceOptions::Reach reach, const QString &address) {
    if ((reach != DeviceOptions::Reach::Tailnet && reach != DeviceOptions::Reach::Network) || m_around.tailscale.isEmpty())
        return;
    QProcess tailscale;
    tailscale.start(m_around.tailscale, {u"status"_s, u"--json"_s});
    if (!tailscale.waitForFinished(3000) || tailscale.exitCode() != 0)
        return;
    if (const QString name = DeviceReach::tailnetName(tailscale.readAllStandardOutput()); !name.isEmpty())
        m_named = u"http://%1:%2"_s.arg(name).arg(QUrl(address).port());
}

QJsonObject HubDevices::status() const {
    QJsonObject options = chosen();
    options.insert("library"_L1, served());
    QJsonObject status{{"options", options},
                       {"libraries", QJsonArray::fromStringList(m_keeper.names())},
                       {"address", m_address},
                       {"named", m_named},
                       {"token", secret(DeviceSecrets::opdsToken)},
                       {"password", !secret(DeviceSecrets::koreaderServeKey).isEmpty()},
                       {"devices", m_server->running() ? m_server->status().devices : 0}};
    if (!m_problem.isEmpty()) {
        status.insert("problem"_L1, m_problem);
        status.insert("said"_L1, m_said);
    }
    return status;
}

std::expected<void, Failure> HubDevices::keep(const QJsonObject &chosen) const {
    QJsonObject settings = settingsIn(m_place.settings());
    settings.insert("devices"_L1, chosen);
    QSaveFile file(m_place.settings());
    if (!QDir().mkpath(m_place.state()) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(settings).toJson()) < 0
        || !file.commit())
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {m_place.settings()}));
    return {};
}

std::expected<void, Failure> HubDevices::keepSecrets(const QJsonObject &asked) {
    if (asked.contains("password"_L1)) {
        const QString password = asked.value("password"_L1).toString();
        const QString name = QString::fromLatin1(DeviceSecrets::koreaderServeKey);
        if (password.isEmpty())
            m_place.forgetSecret(name);
        else if (const auto kept = m_place.keepSecret(name, KosyncClient::keyOf(password).toUtf8()); !kept)
            return kept;
    }
    if (asked.value("new_token"_L1).toBool())
        return m_place.keepSecret(QString::fromLatin1(DeviceSecrets::opdsToken), DeviceSecrets::madeToken().toUtf8());
    return {};
}

std::expected<QJsonObject, Failure> HubDevices::set(const QJsonObject &asked) {
    QJsonObject choices = chosen();
    const QJsonObject options = asked.value("options"_L1).toObject();
    // Every choice looked at before any is kept: one refused changes nothing.
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        const auto choice = checked(it.key(), it.value(), m_keeper.names());
        if (!choice)
            return std::unexpected(choice.error());
        choices.insert(it.key(), *choice);
    }
    if (!options.isEmpty())
        if (const auto kept = keep(choices); !kept)
            return std::unexpected(kept.error());
    if (const auto kept = keepSecrets(asked); !kept)
        return std::unexpected(kept.error());
    follow();
    return status();
}

void HubDevices::addTo(HubRequests &requests) {
    requests.add(u"devices"_s, [this](const QJsonObject &, HubKeeper &) -> std::expected<QJsonObject, Failure> { return status(); });
    requests.add(u"devices-set"_s, [this](const QJsonObject &asked, HubKeeper &) { return set(asked); });
}
