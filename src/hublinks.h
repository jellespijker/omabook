#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <expected>

#include "failure.h"

class Person;

// This computer's links to its person's hubs (#180, ADR 0012), what both the
// command line (`omabook hub add|remove|computers|status <host>`) and the
// window's Hubs panel (#189) call. A computer has one key for its hubs, made
// here the first time (SshKey) and kept in its Omabook data folder; linking
// installs it on the hub, over the person's own ssh, as a key that opens
// nothing but the hub's session (HubComputers), signed by the person
// (`omabook identity`) since a hub takes only its owner's computers. No
// password is ever asked or sent: ssh runs in batch mode.
//
// What the hubs are asked afterwards goes through that key and the session's
// protocol (HubSession): ask() says hello, then the request.
class HubLinks {
public:
    struct Hub {
        QString host;        // as ssh reaches it: "hub", "omabook@hub"
        QString computer;    // this computer's name there
        QString fingerprint; // this computer's key
        QDateTime linked;
    };

    // `folder` holds the key and the list of hubs; `ssh` is the program and
    // its first arguments (a test's stand-in, or options of its own).
    explicit HubLinks(QString folder = usualFolder(), QStringList ssh = {});

    // <data>/omabook/hubs: the person's own, beside their identity.
    [[nodiscard]] static QString usualFolder();
    [[nodiscard]] QString keyFile() const;

    [[nodiscard]] QList<Hub> hubs() const;

    // Links this computer, as `computer`, to the hub at `host`: checks that
    // Omabook runs there, installs the key, and opens a session with it.
    [[nodiscard]] std::expected<Hub, Failure> add(const QString &host, const QString &computer, const Person &person);
    // Takes this computer's key off the hub, and the hub off the list; also
    // when the hub has taken the key away already.
    [[nodiscard]] std::expected<Hub, Failure> remove(const QString &host);

    // The hub's answer to `request`, through this computer's key; refused
    // when the hub turns it down, or this computer isn't linked to it.
    [[nodiscard]] std::expected<QJsonObject, Failure> ask(const QString &host, const QJsonObject &request) const;

private:
    // The answers to hello and then `requests`, after the hello's answer was checked.
    [[nodiscard]] std::expected<QList<QJsonObject>, Failure> exchange(const QString &host, const QList<QJsonObject> &requests) const;
    [[nodiscard]] std::expected<void, Failure> keep(const QList<Hub> &hubs) const;

    QString m_folder;
    QStringList m_ssh;
};
