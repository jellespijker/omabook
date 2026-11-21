#include "devicedesk.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QtConcurrentRun>
#include <expected>

#include "deviceserver.h"
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
    : QObject(parent), m_settings(settings), m_secrets(secrets), m_root(std::move(root)) {
    m_thread.setObjectName(u"devices"_s);
    m_thread.start();
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
    const DeviceServer::Setup setup{.options = options, .opdsToken = m_token, .koreaderKey = m_secrets.get(secretName(u"serve"_s))};
    if (m_server) {
        QMetaObject::invokeMethod(m_server.get(), &DeviceServer::stop);
        m_server->deleteLater();
    }
    m_address.clear();
    m_problem.clear();
    emit changed();
    if (options.reach == DeviceOptions::Reach::Off || root.isEmpty())
        return;
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
            emit changed();
        });
    });
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
