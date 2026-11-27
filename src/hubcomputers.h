#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <expected>
#include <memory>
#include <optional>
#include <utility>

#include "failure.h"

class HubPlace;
class QLockFile;

// The computers linked to this hub (#180, ADR 0012), as the hub keeps them:
// each computer's key in the hub user's authorized_keys, on a line that lets
// it run nothing but `omabook hub session --key <its fingerprint>`, and what
// the hub knows of it (its name, its key, the person whose computer it is) in
// computers.json in the hub's state. The hub belongs to one person (0.5.x):
// the identity of the first computer linked, and only theirs may link more,
// under that id or one it has gone by since (HubOwner, #283).
//
// The line is the hub's own: `restrict` (no shell's terminal, no forwarding
// of ports, agents or X11, no ~/.ssh/rc) and command="...", so whatever the
// client asks to run, sshd runs the session, which knows the computer by the
// fingerprint the hub wrote there, never by what the client says. Lines that
// aren't the hub's are kept as they are.
class HubComputers {
public:
    struct Computer {
        QString name;        // a word: letters, digits, '.', '_' and '-'
        QString fingerprint; // its key's, "SHA256:..."
        QString key;         // its public line, "ssh-ed25519 AAAA..."
        QString identity;    // whose computer it is (Person::id)
        QDateTime linked;
    };

    // What a computer asks to be linked with: its key signed by its person.
    struct Asked {
        QString name;
        QString key;       // an ed25519 public line
        QString identity;  // Person::id
        QString signature; // of linkMessage(name, key), URL-safe Base64
        QString former;    // HubOwner::vouched, when the identity may be new to the hub
    };

    // `state` holds computers.json; `authorizedKeys` is the file sshd reads;
    // `session` is the program (and its first arguments) a key runs.
    HubComputers(QString state, QString authorizedKeys, QStringList session)
        : m_state(std::move(state)), m_authorizedKeys(std::move(authorizedKeys)), m_session(std::move(session)) {}

    // The hub's: its state, ~/.ssh/authorized_keys of its user, and this omabook.
    [[nodiscard]] static HubComputers usual(const HubPlace &place);

    // What a person signs to link a computer of theirs.
    [[nodiscard]] static QByteArray linkMessage(QStringView name, QStringView key);
    [[nodiscard]] static bool isName(QStringView name);

    // Links the computer: in place of one of the same name or key. Refused
    // when it isn't the owner's, isn't signed by them, or the key isn't one.
    [[nodiscard]] std::expected<Computer, Failure> link(const Asked &asked);
    // Takes the computer's key away, by its name or its key's fingerprint.
    [[nodiscard]] std::expected<Computer, Failure> unlink(const QString &nameOrFingerprint);

    [[nodiscard]] QList<Computer> computers() const;
    [[nodiscard]] std::optional<Computer> byFingerprint(const QString &fingerprint) const;
    // The person the hub belongs to, as the first computer linked named them;
    // empty before a computer is linked.
    [[nodiscard]] QString owner() const;
    // Whether `identity` is the owner's: the one above, or one it has gone by since.
    [[nodiscard]] bool isOwner(const QString &identity) const;
    // Takes `claimed.identity` as the owner's too, said by the linked computer
    // of `claimed.key`'s fingerprint, itself the owner's: signed by that person
    // (of HubOwner::message) and vouched for by one the hub knows as its owner.
    // Nothing happens when the hub knows the id already.
    struct Claimed {
        QString fingerprint, identity, signature, former;
    };
    [[nodiscard]] std::expected<void, Failure> recognize(const Claimed &claimed);

private:
    struct Kept {
        QString owner;
        QStringList since; // the ids the owner has gone by since ("owner_ids")
        QList<Computer> computers;

        [[nodiscard]] bool isOwner(const QString &identity) const { return identity == owner || since.contains(identity); }
    };
    // Held while computers.json is read and written again, by whoever does: `hub allow` and a session, each a
    // program of its own.
    [[nodiscard]] std::unique_ptr<QLockFile> hold() const;
    [[nodiscard]] Kept kept() const;
    [[nodiscard]] std::expected<void, Failure> keep(const Kept &kept) const;
    // Rewrites authorized_keys without the hub's lines for `drop`, and with `add` when given.
    [[nodiscard]] std::expected<void, Failure> rewriteKeys(const QStringList &drop, const QString &add) const;
    [[nodiscard]] std::expected<QString, Failure> keyLine(const Computer &computer) const;

    QString m_state;
    QString m_authorizedKeys;
    QStringList m_session;
};
