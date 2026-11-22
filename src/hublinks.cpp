#include "hublinks.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cerrno>
#include <utility>

#include "crypto.h"
#include "hubcomputers.h"
#include "hubsession.h"
#include "person.h"
#include "remote.h"
#include "sshkey.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QStringList usualSsh() {
    // Never a password prompt nobody can see, and no wait on a hub that is off.
    return {u"ssh"_s, u"-o"_s, u"BatchMode=yes"_s, u"-o"_s, u"ConnectTimeout=10"_s};
}

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// What ssh or the far side said last, for a failure's words.
[[nodiscard]] QString lastSaid(const Remote::Result &result) {
    const QString said = result.error.section('\n', -1).trimmed();
    return said.isEmpty() ? QString::number(result.code) : said;
}

[[nodiscard]] std::expected<SshKey::Public, Failure> keyOf(const QString &file, const QString &computer) {
    if (QFile::exists(file))
        return SshKey::publicOf(file);
    if (!QDir().mkpath(QFileInfo(file).absolutePath()))
        return std::unexpected(Failure(Failure::Code::HubKeyUnmade, {file, qt_error_string(errno)}));
    return SshKey::generate(file, u"omabook hub key of "_s + computer);
}

} // namespace

HubLinks::HubLinks(QString folder, QStringList ssh) : m_folder(std::move(folder)), m_ssh(ssh.isEmpty() ? usualSsh() : std::move(ssh)) {}

QString HubLinks::usualFolder() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/omabook/hubs"_s; }

QString HubLinks::keyFile() const { return QDir(m_folder).filePath(u"key"_s); }

QList<HubLinks::Hub> HubLinks::hubs() const {
    QFile file(QDir(m_folder).filePath(u"hubs.json"_s));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QList<Hub> hubs;
    for (const auto &value : QJsonDocument::fromJson(file.readAll()).array()) {
        const QJsonObject hub = value.toObject();
        hubs << Hub{.host = hub.value("host"_L1).toString(),
                    .computer = hub.value("computer"_L1).toString(),
                    .fingerprint = hub.value("fingerprint"_L1).toString(),
                    .linked = QDateTime::fromString(hub.value("linked"_L1).toString(), Qt::ISODate)};
    }
    return hubs;
}

std::expected<void, Failure> HubLinks::keep(const QList<Hub> &hubs) const {
    QJsonArray kept;
    for (const Hub &hub : hubs)
        kept.append(QJsonObject{{"host", hub.host},
                                {"computer", hub.computer},
                                {"fingerprint", hub.fingerprint},
                                {"linked", hub.linked.toString(Qt::ISODate)}});
    QSaveFile file(QDir(m_folder).filePath(u"hubs.json"_s));
    if (!QDir().mkpath(m_folder) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(kept).toJson()) < 0 || !file.commit())
        return std::unexpected(Failure(Failure::Code::HubKeyUnmade, {file.fileName(), file.errorString()}));
    return {};
}

std::expected<HubLinks::Hub, Failure> HubLinks::add(const QString &host, const QString &computer, const Person &person) {
    if (!HubComputers::isName(computer))
        return std::unexpected(Failure(Failure::Code::HubBadComputerName, {computer}));
    const auto key = keyOf(keyFile(), computer);
    if (!key)
        return std::unexpected(key.error());
    const QString line = SshKey::line(*key);
    const Person::Signature signature = person.sign(Crypto::bytes(HubComputers::linkMessage(computer, line)));
    const QTemporaryDir here;
    QFile given(here.filePath(u"key.pub"_s));
    if (!here.isValid() || !given.open(QIODevice::WriteOnly) || given.write(line.toUtf8() + '\n') < 0)
        return std::unexpected(Failure(Failure::Code::HubKeyUnmade, {given.fileName(), given.errorString()}));
    given.close();
    // Over the person's own ssh, once: the hub installs the key for its session.
    const Remote::Result allowed =
        Remote(host, m_ssh)
            .omabook({u"hub"_s, u"allow"_s, u"--name"_s, computer, u"--identity"_s, person.id(), u"--signature"_s,
                      QString::fromLatin1(
                          Crypto::toByteArray(signature).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))},
                     given.fileName());
    const QJsonObject answer = QJsonDocument::fromJson(allowed.output).object();
    if (!answer.value("ok"_L1).toBool())
        return std::unexpected(answer.contains("error"_L1) ? Failure::said(answer.value("error"_L1).toString())
                                                           : Failure(Failure::Code::HubUnreachable, {host, lastSaid(allowed)}));
    Hub hub{.host = host, .computer = computer, .fingerprint = SshKey::fingerprint(*key), .linked = QDateTime::currentDateTimeUtc()};
    QList<Hub> hubs = this->hubs();
    hubs.removeIf([&host](const Hub &other) { return other.host == host; });
    hubs << hub;
    if (const auto kept = keep(hubs); !kept)
        return std::unexpected(kept.error());
    // The key opens the session, and the session knows this computer by it.
    const auto greeted = exchange(host, {});
    if (!greeted)
        return std::unexpected(greeted.error());
    return hub;
}

std::expected<HubLinks::Hub, Failure> HubLinks::remove(const QString &host) {
    QList<Hub> hubs = this->hubs();
    const auto found = std::ranges::find_if(hubs, [&host](const Hub &hub) { return hub.host == host; });
    if (found == hubs.end())
        return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
    Hub hub = *found;
    if (const auto unlinked = ask(host, {{"request", "unlink"}}); !unlinked && unlinked.error().code() != Failure::Code::HubNotLinked)
        return std::unexpected(unlinked.error());
    hubs.removeIf([&host](const Hub &other) { return other.host == host; });
    if (const auto kept = keep(hubs); !kept)
        return std::unexpected(kept.error());
    return hub;
}

std::expected<QJsonObject, Failure> HubLinks::ask(const QString &host, const QJsonObject &request) const {
    const auto answers = exchange(host, {request});
    if (!answers)
        return std::unexpected(answers.error());
    QJsonObject answer = answers->value(0);
    if (!answer.value("ok"_L1).toBool())
        return std::unexpected(Failure::said(answer.value("error"_L1).toString()));
    return answer;
}

std::expected<QList<QJsonObject>, Failure> HubLinks::exchange(const QString &host, const QList<QJsonObject> &requests) const {
    if (std::ranges::none_of(hubs(), [&host](const Hub &hub) { return hub.host == host; }))
        return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
    const QTemporaryDir here;
    QFile input(here.filePath(u"requests"_s));
    QByteArray lines = compact(HubSession::hello()) + '\n';
    for (const QJsonObject &request : requests)
        lines += compact(request) + '\n';
    if (!here.isValid() || !input.open(QIODevice::WriteOnly) || input.write(lines) < 0)
        return std::unexpected(Failure(Failure::Code::HubUnreachable, {host, input.errorString()}));
    input.close();
    // This key alone: never another of the person's, which may open a shell there.
    const QStringList ssh = m_ssh + QStringList{u"-i"_s, keyFile(), u"-o"_s, u"IdentitiesOnly=yes"_s};
    const Remote::Result result = Remote(host, ssh).run(u"omabook hub session"_s, input.fileName());
    QList<QJsonObject> answers;
    for (const QByteArray &line : result.output.split('\n'))
        if (!line.trimmed().isEmpty())
            answers << QJsonDocument::fromJson(line).object();
    // sshd turned the key away: the hub has taken it off (omabook hub forget).
    if (answers.isEmpty() && result.error.contains(u"Permission denied (publickey"_s))
        return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
    if (answers.isEmpty())
        return std::unexpected(Failure(Failure::Code::HubUnreachable, {host, lastSaid(result)}));
    const QJsonObject greeting = answers.takeFirst();
    if (greeting.contains("linked"_L1) && !greeting.value("linked"_L1).toBool())
        return std::unexpected(Failure(Failure::Code::HubNotLinked, {host}));
    if (!greeting.value("ok"_L1).toBool())
        return std::unexpected(Failure::said(greeting.value("error"_L1).toString()));
    return answers;
}
