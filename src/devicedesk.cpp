#include "devicedesk.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QProcess>
#include <QTimer>
#include <QtConcurrentRun>
#include <chrono>
#include <expected>

#include "deviceserver.h"
#include "deviceserving.h"
#include "koreadersync.h"
#include "kosyncclient.h"
#include "library.h"
#include "secretstore.h"
#include "sendbook.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString secretName(const QString &which) {
    if (which == u"serve")
        return QString::fromLatin1(DeviceSecrets::koreaderServeKey);
    if (which == u"sync")
        return QString::fromLatin1(DeviceSecrets::koreaderKey);
    if (which == u"mail")
        return QString::fromLatin1(DeviceSecrets::mailPassword);
    return {};
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, Failure> opened(const QString &root) {
    auto library = std::make_unique<Library>(root);
    if (QString why; !library->open(&why))
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {why}));
    return library;
}

} // namespace

DeviceDesk::DeviceDesk(Settings *settings, SecretStore &secrets, Root root, QObject *parent)
    : DeviceDesk(settings, secrets, std::move(root), Programs{}, parent) {}

DeviceDesk::DeviceDesk(Settings *settings, SecretStore &secrets, Root root, Programs programs, QObject *parent)
    : QObject(parent), m_settings(settings), m_secrets(secrets), m_root(std::move(root)), m_programs(std::move(programs)) {
    m_thread.setObjectName(u"devices"_s);
    m_thread.start();
    auto *jobs = new QObject; // owned by its thread from here: deleted when it finishes
    jobs->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, jobs, &QObject::deleteLater);
    m_jobs = jobs;
    // config.json edited by hand, or another library chosen, is followed too.
    connect(m_settings, &Settings::changed, this, &DeviceDesk::follow);
}

DeviceDesk::~DeviceDesk() {
    m_thread.quit();
    m_thread.wait();
}

DeviceOptions DeviceDesk::current() const { return DeviceOptions::fromJson(m_settings->devices()); }

QVariantMap DeviceDesk::options() const { return current().toJson().toVariantMap(); }

QVariantMap DeviceDesk::secrets() const {
    return {{"serve", !m_secrets.get(secretName(u"serve"_s)).isEmpty()},
            {"sync", !m_secrets.get(secretName(u"sync"_s)).isEmpty()},
            {"mail", !m_secrets.get(secretName(u"mail"_s)).isEmpty()}};
}

QString DeviceDesk::deviceKind() const { return SendBook::deviceName(SendBook::deviceAt(current().deviceFolder)); }

bool DeviceDesk::deviceThere() const {
    const QString folder = current().deviceFolder;
    return !folder.isEmpty() && QFileInfo(folder).isDir();
}

bool DeviceDesk::canMail() const {
    const DeviceOptions options = current();
    return !options.kindleAddress.isEmpty() && !options.mailHost.isEmpty() && !options.mailFrom.isEmpty();
}

void DeviceDesk::set(const QString &key, const QVariant &value) {
    QJsonObject devices = current().toJson();
    devices.insert(key, QJsonValue::fromVariant(value));
    m_settings->setDevices(DeviceOptions::fromJson(devices).toJson());
    emit changed();
}

QString DeviceDesk::setPassword(const QString &which, const QString &password) {
    const QString name = secretName(which);
    if (name.isEmpty())
        return {};
    QString why;
    if (password.isEmpty())
        std::ignore = m_secrets.forget(name); // Gone or never there, either way it is not kept.
    else if (!m_secrets.put(name, which == u"mail" ? password : KosyncClient::keyOf(password), &why))
        m_status = why;
    emit changed();
    if (!why.isEmpty())
        return why;
    restart(true);
    return {};
}

void DeviceDesk::renewToken() {
    std::ignore = DeviceSecrets::newToken(m_secrets); // The new one is read back as the server restarts.
    restart(true);
}

void DeviceDesk::follow() { restart(false); }

void DeviceDesk::restart(bool secretsChanged) {
    const DeviceOptions options = current();
    // What the server is started with, so that a change to any other setting leaves it be.
    const QString root = m_root();
    const QJsonObject following{{"options", options.toJson()}, {"root", root}};
    if (following == m_following && !secretsChanged)
        return;
    m_following = following;
    m_token = options.opds ? DeviceSecrets::token(m_secrets) : QString();
    if (m_server) {
        QMetaObject::invokeMethod(m_server.get(), &DeviceServer::stop);
        m_server->deleteLater();
    }
    m_address.clear();
    m_problem.clear();
    m_servedBy.clear();
    m_devicesLately = 0;
    emit changed();
    const bool serving = options.reach != DeviceOptions::Reach::Off && !root.isEmpty() && (options.opds || options.koreaderServe);
    lookUpName();
    // On the thread before the server below, so that a service stopped has let go of the port first.
    followService(options, serving && options.background);
    if (!serving || options.background)
        return;
    serve({.options = options, .opdsToken = m_token, .koreaderKey = m_secrets.get(secretName(u"serve"_s))}, root);
}

void DeviceDesk::serve(const DeviceServer::Setup &setup, const QString &root) {
    auto *server = new DeviceServer(root); // owned by its thread from here: deleted when it finishes, or replaced
    server->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, server, &QObject::deleteLater);
    connect(server, &DeviceServer::readTo, this, &DeviceDesk::booksChanged);
    m_server = server;
    QMetaObject::invokeMethod(server, [this, server, setup] {
        const auto address = server->start(setup);
        QMetaObject::invokeMethod(this, [this, server, address] {
            if (server != m_server)
                return;
            m_address = address ? *address : QString();
            m_problem = address ? QString() : address.error().text();
            m_servedBy = address && !address->isEmpty() ? u"window"_s : QString();
            emit changed();
            if (address)
                return;
            // Not serving, so it goes; an Omabook serving there already is shown as the one serving.
            server->deleteLater();
            m_server = nullptr;
            refresh();
        });
    });
}

void DeviceDesk::followService(const DeviceOptions &options, bool wanted) {
    const ServeService service(m_programs.service);
    // Nothing of it here, and nothing wanted of it: systemd isn't asked.
    if (!wanted && !m_serviceFollowed && !service.installed())
        return;
    const bool first = !m_serviceFollowed;
    m_serviceFollowed = wanted;
    m_serviceUp = false;
    QString shown;
    if (wanted) {
        if (const auto address = DeviceReach::address(options.reach))
            shown = DeviceReach::shown(options.reach, *address, options.port);
        else
            m_problem = Failure(Failure::Code::NoTailnet).text();
    }
    QMetaObject::invokeMethod(m_jobs.get(), [this, service, wanted, first, shown] {
        // The first time this session it is written again, as this program may have moved since.
        const auto installOrRestart = [&service, first] { return first || !service.installed() ? service.install() : service.restart(); };
        const auto done = wanted ? installOrRestart() : service.remove();
        QMetaObject::invokeMethod(this, [this, done, wanted, shown] {
            if (!done) {
                m_problem = done.error().text();
                emit changed();
                return;
            }
            if (!wanted)
                return;
            m_serviceUp = true;
            m_address = shown;
            emit changed();
            // It takes a moment to start; the panel asks again while it is open.
            QTimer::singleShot(std::chrono::milliseconds(1500), this, &DeviceDesk::refresh);
        });
    });
}

void DeviceDesk::refresh() {
    if (m_server) {
        QMetaObject::invokeMethod(m_server.get(), [this, server = m_server.get()] {
            const bool running = server->running();
            const int devices = server->status().devices;
            QMetaObject::invokeMethod(this, [this, running, devices] { told(running ? u"window"_s : QString(), devices, running); });
        });
        return;
    }
    const DeviceOptions options = current();
    const auto address = DeviceReach::address(options.reach);
    if (!address) {
        told({}, 0, false);
        return;
    }
    // On the thread, which has no server of the window's own to wait for here.
    QMetaObject::invokeMethod(m_jobs.get(), [this, where = *address, port = options.port] {
        const auto status = DeviceServing::ask(where, port);
        QMetaObject::invokeMethod(this, [this, status] {
            told(status ? DeviceServing::byName(status->by) : QString(), status ? status->devices : 0, status.has_value());
        });
    });
}

void DeviceDesk::told(const QString &servedBy, int devices, bool answered) {
    m_servedBy = servedBy;
    m_devicesLately = devices;
    const DeviceOptions options = current();
    if (answered && servedBy != u"window") {
        // Another Omabook serves: that is no problem, and it is where devices go.
        m_problem.clear();
        if (const auto address = DeviceReach::address(options.reach); address && m_address.isEmpty())
            m_address = DeviceReach::shown(options.reach, *address, options.port);
    }
    if (!answered && options.background && m_serviceUp && !m_address.isEmpty())
        m_problem = Failure(Failure::Code::ServiceNotAnswering, {m_address}).text();
    emit changed();
}

void DeviceDesk::lookUpName() {
    const DeviceOptions::Reach reach = current().reach;
    if (reach != DeviceOptions::Reach::Tailnet && reach != DeviceOptions::Reach::Network)
        return;
    QMetaObject::invokeMethod(m_jobs.get(), [this, program = m_programs.tailscale] {
        QProcess tailscale;
        tailscale.start(program, {u"status"_s, u"--json"_s});
        const QString name = tailscale.waitForFinished(3000) && tailscale.exitCode() == 0
                                 ? DeviceReach::tailnetName(tailscale.readAllStandardOutput())
                                 : QString();
        QMetaObject::invokeMethod(this, [this, name] {
            m_name = name;
            emit changed();
        });
    });
}

QString DeviceDesk::namedAddress() const {
    const DeviceOptions options = current();
    if (m_name.isEmpty() || m_address.isEmpty() || options.reach == DeviceOptions::Reach::ThisComputer)
        return {};
    return u"http://%1:%2"_s.arg(m_name).arg(options.port);
}

void DeviceDesk::work(const std::function<QString()> &job) {
    if (m_busy)
        return;
    m_busy = true;
    m_status.clear();
    emit changed();
    std::ignore = QtConcurrent::run([this, job] { // Its end is told through the call below, never waited for.
        const QString said = job();
        QMetaObject::invokeMethod(this, [this, said] {
            m_busy = false;
            m_status = said;
            emit changed();
            emit booksChanged();
        });
    });
}

void DeviceDesk::syncNow() {
    const DeviceOptions options = current();
    const QString key = m_secrets.get(secretName(u"sync"_s));
    if (options.syncServer.isEmpty() || options.syncUser.isEmpty() || key.isEmpty()) {
        m_status = Failure(Failure::Code::SyncNotSetUp).text();
        emit changed();
        return;
    }
    work([root = m_root(), options, key] {
        const auto library = opened(root);
        if (!library)
            return library.error().text();
        QNetworkAccessManager network;
        KosyncClient client(network, QUrl(options.syncServer), options.syncUser, key);
        const auto summary = KoreaderSync::run(**library, client);
        if (!summary)
            return summary.error().text();
        return tr("%n books moved to where a device had read to.", "", summary->taken) + ' '
               + tr("%n places went to the sync server.", "", summary->sent);
    });
}

void DeviceDesk::sendBook(qint64 book, const QString &how) {
    const DeviceOptions options = current();
    const QString password = m_secrets.get(secretName(u"mail"_s));
    const QString device = deviceKind().isEmpty() ? tr("e-reader") : deviceKind();
    work([root = m_root(), options, password, book, how, device] {
        const auto library = opened(root);
        if (!library)
            return library.error().text();
        if (how == u"kindle") {
            const SendBook::Post post{
                .server = {.host = options.mailHost,
                           .port = options.mailPort,
                           .security = options.mailStartTls ? SmtpMail::Server::Security::StartTls : SmtpMail::Server::Security::Tls,
                           .user = options.mailUser,
                           .password = password},
                .from = options.mailFrom,
                .to = options.kindleAddress};
            const auto sent = SendBook::byMail(**library, book, post);
            return sent ? tr("Mailed to %1.").arg(options.kindleAddress) : sent.error().text();
        }
        const auto copied = SendBook::toFolder(**library, book, options.deviceFolder);
        return copied ? tr("On the %1 as %2.").arg(device, QFileInfo(*copied).fileName()) : copied.error().text();
    });
}
