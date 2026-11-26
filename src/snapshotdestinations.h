#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <utility>

#include "failure.h"
#include "hubplace.h"
#include "restic.h"
#include "snapshots.h"

// Where else a computer keeps its snapshots (ADR 0014, GLOSSARY: Destination):
// a second disk of its own, a place away from home that restic reaches
// (sftp:, rest:, s3:, b2:, rclone:...), or another of the person's hubs
// (hub:<host>, #196), reached over the hub's session (HubStore), which keeps
// them and can't read them, and counts as a place away. Each is a restic repository of its
// own with a password of its own, shown once as its recovery key, and gets the
// snapshots by `restic copy` from the computer's repository, then is thinned
// as that one is.
struct SnapshotDestination {
    // A folder of this computer's (a second disk), a place restic reaches over the network, or a hub.
    enum class Kind : std::uint8_t { Disk, Away, Hub };

    QString name;     // lower-case letters, digits and dashes: it names its secrets
    QString location; // restic's --repo
    // The names of what restic is handed in its environment for it (AWS_SECRET_ACCESS_KEY,
    // RESTIC_REST_PASSWORD...): the values are kept as secrets, never here.
    QStringList credentials;

    [[nodiscard]] Kind kind() const;
    // The host of the hub it is on, as this computer is linked to it; empty for another kind.
    [[nodiscard]] QString hub() const;
    [[nodiscard]] static QString kindName(Kind kind);
};

// How a destination's last copy went, kept beside the choices: what
// `omabook snapshot destination list` and the hub's warnings (#184) show.
struct DestinationState {
    QDateTime tried;   // when its last copy began
    QDateTime copied;  // when one last went through
    bool done = false; // whether the last one did
    bool absent = false;
    QString said; // why not, in English
    QString latestId;
    QDateTime latest; // its newest snapshot
    qint64 size = -1; // the bytes it takes there, when known
};

// The destinations of a computer's snapshots, kept in its snapshots' state
// folder (destinations.json, the names of their credentials but none of
// their values) and their secrets as the hub keeps its own (HubPlace::secret:
// systemd's credential, else a 0600 file under secrets/), on a hub the hub's.
class SnapshotDestinations {
public:
    // How the repository named `name` that a hub at `host` keeps for this
    // computer is reached (#196): its location, what restic is handed for it,
    // and what stands between (ResticRepository::through); its password is the
    // destination's own, added here.
    using HubReach = std::function<std::expected<ResticRepository, Failure>(const QString &host, const QString &name)>;

    explicit SnapshotDestinations(SnapshotPlace place);

    // How every destination on a hub is reached, set once as the program starts (HubStoreBridge::linked).
    static void reachHubsBy(HubReach reach);

    [[nodiscard]] QList<SnapshotDestination> list() const;
    [[nodiscard]] std::optional<SnapshotDestination> find(const QString &name) const;
    [[nodiscard]] std::optional<DestinationState> stateOf(const QString &name) const;
    // As `snapshot destination list --json` and the window show it: {"name", "location", "kind",
    // "credentials", "copied", "last": {"tried", "copied", "absent", "said", "latest_id", "latest", "size"}},
    // "last" only once a copy was tried.
    [[nodiscard]] QJsonObject json(const SnapshotDestination &destination) const;

    // Makes the destination's repository, cut into chunks as the computer's
    // is so copies stay deduplicated, keeps its password and `credentials`
    // (name and value) as secrets, and gives the password back as its
    // recovery key. A folder must be on a disk that is there (its parent is).
    [[nodiscard]] std::expected<QString, Failure> add(const SnapshotDestination &asked,
                                                      const QList<std::pair<QString, QByteArray>> &credentials);
    // Forgets the destination and its secrets; what is kept there stays.
    [[nodiscard]] std::expected<void, Failure> remove(const QString &name);

    struct Tested {
        qsizetype snapshots = 0;
        std::optional<Restic::Snapshot> latest;
    };
    // Opens the destination with its password: reached, and what it holds.
    [[nodiscard]] std::expected<Tested, Failure> test(const QString &name) const;

    // Copies the snapshots the destination doesn't hold yet, then forgets
    // there what the retention keeps no longer at `now`; a second disk that
    // isn't there is DestinationAbsent. Either way its state is kept.
    [[nodiscard]] std::expected<DestinationState, Failure> copy(const QString &name, const QDateTime &now) const;
    struct Round {
        QStringList copied;
        QList<std::pair<QString, Failure>> failed;
    };
    // Copies to each destination in turn; one that fails doesn't stop the others.
    [[nodiscard]] Round copyAll(const QDateTime &now) const;

    // Three copies on two kinds of medium, one away from home: this
    // computer's, and at least a second disk and a place away whose last
    // copies went through.
    [[nodiscard]] bool backedUp() const;

    // The destination's repository as restic opens it: its password and credentials read from its secrets.
    [[nodiscard]] std::expected<ResticRepository, Failure> repositoryOf(const SnapshotDestination &destination) const;
    // The names of its secrets.
    [[nodiscard]] static QString passwordSecret(const QString &name);
    [[nodiscard]] static QString credentialsSecret(const QString &name);

private:
    [[nodiscard]] HubPlace secrets() const;
    [[nodiscard]] std::expected<SnapshotDestination, Failure> checked(SnapshotDestination destination) const;
    // Where restic finds it, without its password.
    [[nodiscard]] static std::expected<ResticRepository, Failure> placeOf(const SnapshotDestination &destination);
    [[nodiscard]] bool write(const QList<SnapshotDestination> &destinations) const;
    void keepState(const QString &name, const DestinationState &state) const;
    [[nodiscard]] std::expected<void, Failure> thin(const ResticRepository &repository, const QDateTime &now) const;

    SnapshotPlace m_place;
};
