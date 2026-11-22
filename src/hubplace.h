#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <expected>
#include <utility>

#include "failure.h"

// Where a hub keeps what is its own (#179): a state folder (its choices, its
// log, and its secrets when systemd's credentials don't hold them), the socket
// its daemon takes requests at, and the folder of the libraries it keeps,
// chosen at setup. None of it is in a person's settings or keyring: a hub has
// no desktop, and may run as a user of its own (the system variant of its
// systemd unit).
class HubPlace {
public:
    struct Folders {
        QString state;       // its choices, log and secret files
        QString runtime;     // where its socket is
        QString credentials; // systemd's credentials, decrypted for it; empty without
    };

    // systemd's folders when it runs the daemon (StateDirectory=,
    // RuntimeDirectory=, ImportCredential=), and a session finds the same: the
    // system variant's /var/lib/omabook-hub and /run/omabook-hub when they are
    // this user's, else the user's own ($XDG_STATE_HOME/omabook-hub,
    // $XDG_RUNTIME_DIR/omabook-hub).
    [[nodiscard]] static HubPlace usual();

    explicit HubPlace(Folders folders) : m_folders(std::move(folders)) {}

    [[nodiscard]] const QString &state() const noexcept { return m_folders.state; }
    // The socket the daemon listens at, and `omabook hub session` asks at.
    [[nodiscard]] QString socket() const;

    // The folder of the libraries it keeps, as chosen at setup; empty before.
    [[nodiscard]] QString libraries() const;
    [[nodiscard]] std::expected<void, Failure> chooseLibraries(const QString &folder) const;

    // A secret by its name (the snapshots' password, a destination's key):
    // systemd's credential omabook-hub.<name> when there is one, else the file
    // <state>/secrets/<name>, refused unless it is this user's alone (0600).
    // Never the keyring, which a hub doesn't have.
    [[nodiscard]] std::expected<QByteArray, Failure> secret(const QString &name) const;
    // Kept in that file, for this user alone; a credential is set by systemd's
    // tools (systemd-creds), not here.
    [[nodiscard]] std::expected<void, Failure> keepSecret(const QString &name, QByteArrayView bytes) const;

private:
    [[nodiscard]] QString choicesFile() const;

    Folders m_folders;
};
