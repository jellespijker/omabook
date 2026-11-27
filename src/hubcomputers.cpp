#include "hubcomputers.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <cerrno>
#include <utility>

#include "hubowner.h"
#include "hubplace.h"
#include "sshkey.h"

using namespace Qt::StringLiterals;

namespace {

// What the hub writes in each of its lines, by which it knows them again.
[[nodiscard]] QString ownMark(const QString &fingerprint) { return u"hub session --key '"_s + fingerprint + u"'\""_s; }

// A word that can stand in single quotes inside command="...": nothing that
// would end either, and nothing sshd or the shell would read as more.
[[nodiscard]] bool fitsTheLine(QStringView word) {
    return !word.isEmpty() && std::ranges::none_of(word, [](QChar c) {
        return c == u'"' || c == u'\'' || c == u'\\' || c.category() == QChar::Other_Control;
    });
}

[[nodiscard]] QJsonObject toJson(const HubComputers::Computer &computer) {
    return {{"name", computer.name},
            {"fingerprint", computer.fingerprint},
            {"key", computer.key},
            {"identity", computer.identity},
            {"linked", computer.linked.toString(Qt::ISODate)}};
}

[[nodiscard]] HubComputers::Computer fromJson(const QJsonObject &object) {
    return {.name = object.value("name"_L1).toString(),
            .fingerprint = object.value("fingerprint"_L1).toString(),
            .key = object.value("key"_L1).toString(),
            .identity = object.value("identity"_L1).toString(),
            .linked = QDateTime::fromString(object.value("linked"_L1).toString(), Qt::ISODate)};
}

} // namespace

HubComputers HubComputers::usual(const HubPlace &place) {
    return {place.state(), QDir::home().filePath(u".ssh/authorized_keys"_s), {QCoreApplication::applicationFilePath()}};
}

QByteArray HubComputers::linkMessage(QStringView name, QStringView key) {
    return "omabook hub link 1\n" + name.toUtf8() + '\n' + key.toUtf8() + '\n';
}

bool HubComputers::isName(QStringView name) {
    static const QRegularExpression word(u"^[A-Za-z0-9][A-Za-z0-9._-]{0,62}$"_s);
    return word.matchView(name).hasMatch();
}

std::expected<HubComputers::Computer, Failure> HubComputers::link(const Asked &asked) {
    if (!isName(asked.name))
        return std::unexpected(Failure(Failure::Code::HubBadComputerName, {asked.name}));
    const auto key = SshKey::parse(asked.key);
    if (!key)
        return std::unexpected(key.error());
    Computer computer{.name = asked.name,
                      .fingerprint = SshKey::fingerprint(*key),
                      .key = SshKey::line(*key),
                      .identity = asked.identity,
                      .linked = QDateTime::currentDateTimeUtc()};
    if (!HubOwner::signedBy(asked.identity, linkMessage(computer.name, computer.key), asked.signature))
        return std::unexpected(Failure(Failure::Code::HubBadSignature));
    const auto held = hold();
    Kept now = kept();
    if (!now.owner.isEmpty() && !now.isOwner(asked.identity)) {
        // Not an id the hub knows: the owner's only when one it knows says so.
        QStringList owners = now.since;
        owners << now.owner;
        if (!HubOwner::vouched(owners, asked.identity, computer.key, asked.former))
            return std::unexpected(Failure(Failure::Code::HubNotOwner));
        now.since << asked.identity;
    }
    // Linked again, it replaces what was there under its name or with its key.
    const auto replaced = [&computer](const Computer &other) {
        return other.name == computer.name || other.fingerprint == computer.fingerprint;
    };
    QStringList drop{computer.fingerprint};
    for (const Computer &other : std::as_const(now.computers))
        if (replaced(other))
            drop << other.fingerprint;
    const auto line = keyLine(computer);
    if (!line)
        return std::unexpected(line.error());
    if (const auto written = rewriteKeys(drop, *line); !written)
        return std::unexpected(written.error());
    now.computers.removeIf(replaced);
    now.computers << computer;
    if (now.owner.isEmpty())
        now.owner = asked.identity;
    if (const auto stored = keep(now); !stored)
        return std::unexpected(stored.error());
    return computer;
}

std::expected<void, Failure> HubComputers::recognize(const Claimed &claimed) {
    const auto held = hold();
    Kept now = kept();
    const auto asking = std::ranges::find(now.computers, claimed.fingerprint, &Computer::fingerprint);
    if (claimed.fingerprint.isEmpty() || asking == now.computers.end() || !now.isOwner(asking->identity))
        return std::unexpected(Failure(Failure::Code::HubNotOwner));
    if (now.isOwner(claimed.identity))
        return {};
    if (!HubOwner::signedBy(claimed.identity, HubOwner::message(claimed.identity, asking->key), claimed.signature))
        return std::unexpected(Failure(Failure::Code::HubBadSignature));
    QStringList owners = now.since;
    owners << now.owner;
    if (!HubOwner::vouched(owners, claimed.identity, asking->key, claimed.former))
        return std::unexpected(Failure(Failure::Code::HubNotOwner));
    now.since << claimed.identity;
    return keep(now);
}

std::expected<HubComputers::Computer, Failure> HubComputers::unlink(const QString &nameOrFingerprint) {
    const auto held = hold();
    Kept now = kept();
    const auto named = [&nameOrFingerprint](const Computer &computer) {
        return computer.name == nameOrFingerprint || computer.fingerprint == nameOrFingerprint;
    };
    const auto found = std::ranges::find_if(now.computers, named);
    if (found == now.computers.end())
        return std::unexpected(Failure(Failure::Code::HubNoSuchComputer, {nameOrFingerprint}));
    Computer gone = *found;
    // The key first: what fails after it leaves a record that opens nothing.
    if (const auto written = rewriteKeys({gone.fingerprint}, {}); !written)
        return std::unexpected(written.error());
    now.computers.removeIf(named);
    if (const auto stored = keep(now); !stored)
        return std::unexpected(stored.error());
    return gone;
}

QList<HubComputers::Computer> HubComputers::computers() const { return kept().computers; }

std::optional<HubComputers::Computer> HubComputers::byFingerprint(const QString &fingerprint) const {
    for (const Computer &computer : kept().computers)
        if (!fingerprint.isEmpty() && computer.fingerprint == fingerprint)
            return computer;
    return std::nullopt;
}

QString HubComputers::owner() const { return kept().owner; }

bool HubComputers::isOwner(const QString &identity) const { return !identity.isEmpty() && kept().isOwner(identity); }

std::unique_ptr<QLockFile> HubComputers::hold() const {
    QDir().mkpath(m_state);
    auto lock = std::make_unique<QLockFile>(QDir(m_state).filePath(u"computers.lock"_s));
    // Held for the moment of a write. Where it can't be had, what follows writes as before there was one.
    lock->tryLock(10000);
    return lock;
}

HubComputers::Kept HubComputers::kept() const {
    QFile file(QDir(m_state).filePath(u"computers.json"_s));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    Kept kept{.owner = object.value("owner"_L1).toString(), .since = {}, .computers = {}};
    for (const auto &id : object.value("owner_ids"_L1).toArray())
        kept.since << id.toString();
    for (const auto &computer : object.value("computers"_L1).toArray())
        kept.computers << fromJson(computer.toObject());
    return kept;
}

std::expected<void, Failure> HubComputers::keep(const Kept &kept) const {
    QJsonArray computers;
    for (const Computer &computer : kept.computers)
        computers.append(toJson(computer));
    QJsonObject written{{"owner", kept.owner}, {"computers", computers}};
    if (!kept.since.isEmpty())
        written.insert("owner_ids"_L1, QJsonArray::fromStringList(kept.since));
    QSaveFile file(QDir(m_state).filePath(u"computers.json"_s));
    if (!QDir().mkpath(m_state) || !file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(written).toJson()) < 0 || !file.commit())
        return std::unexpected(Failure(Failure::Code::HubKeysUnwritable, {file.fileName(), file.errorString()}));
    return {};
}

std::expected<QString, Failure> HubComputers::keyLine(const Computer &computer) const {
    QStringList command;
    for (const QString &part : m_session) {
        if (!fitsTheLine(part))
            return std::unexpected(Failure(Failure::Code::HubKeysUnwritable, {m_authorizedKeys, part}));
        command << u"'"_s + part + u"'"_s;
    }
    return u"restrict,command=\"%1 hub session --key '%2'\" %3 omabook-hub:%4"_s.arg(command.join(' '), computer.fingerprint, computer.key,
                                                                                     computer.name);
}

std::expected<void, Failure> HubComputers::rewriteKeys(const QStringList &drop, const QString &add) const {
    const auto unwritable = [this](const QString &why) {
        return std::unexpected(Failure(Failure::Code::HubKeysUnwritable, {m_authorizedKeys, why}));
    };
    QStringList lines;
    if (QFile file(m_authorizedKeys); file.exists()) {
        if (!file.open(QIODevice::ReadOnly))
            return unwritable(file.errorString());
        lines = QString::fromUtf8(file.readAll()).split('\n');
        while (!lines.isEmpty() && lines.last().isEmpty())
            lines.removeLast();
    }
    lines.removeIf([&drop](const QString &line) {
        return std::ranges::any_of(drop, [&line](const QString &fingerprint) { return line.contains(ownMark(fingerprint)); });
    });
    if (!add.isEmpty())
        lines << add;
    // ~/.ssh as sshd wants it, when it is made here: its user's alone.
    const QString folder = QFileInfo(m_authorizedKeys).absolutePath();
    if (!QFileInfo::exists(folder)
        && (!QDir().mkpath(folder) || !QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)))
        return unwritable(qt_error_string(errno));
    QSaveFile file(m_authorizedKeys);
    if (!file.open(QIODevice::WriteOnly) || file.write((lines.join('\n') + (lines.isEmpty() ? u""_s : u"\n"_s)).toUtf8()) < 0
        || !file.commit())
        return unwritable(file.errorString());
    if (!QFile::setPermissions(m_authorizedKeys, QFile::ReadOwner | QFile::WriteOwner))
        return unwritable(qt_error_string(errno));
    return {};
}
