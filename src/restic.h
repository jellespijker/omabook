#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>
#include <expected>

#include "failure.h"

// restic, which makes the snapshots (ADR 0014): encrypted, deduplicated, kept
// in a repository on a disk or elsewhere. Omabook only drives it; nothing of
// restic's is built here. Each call runs the program to its end, its password
// read from a file (never the command line), and with none of the person's
// own RESTIC_* settings, so what it does is what is asked here.
struct ResticRepository {
    QString location;     // restic's --repo: a folder, sftp:..., s3:..., rclone:...
    QString passwordFile; // read by restic; 0600, or one of systemd's credentials
    // Or the password itself, as a hub reads its secrets (HubPlace::secret):
    // handed to restic alone, in its environment, never on its command line.
    QByteArray password;
    QString program = QStringLiteral("restic");
};

namespace Restic {

// A snapshot as restic lists it.
struct Snapshot {
    QString id;     // the whole id
    QDateTime time; // in this computer's time zone
    QStringList tags;
    QStringList paths;

    [[nodiscard]] QString shortId() const { return id.left(8); }
    // What follows `prefix` in its first tag that begins with it; empty for none.
    [[nodiscard]] QString tagged(const QString &prefix) const;
};

// Makes a new repository at `repository.location`.
[[nodiscard]] std::expected<void, Failure> init(const ResticRepository &repository);

// What to keep in one snapshot: paths whole, but for what `excluding` names
// (restic's patterns), labelled with `tags`.
struct Backup {
    QStringList paths;
    QStringList excluding;
    QStringList tags;
};
// Takes the snapshot, and says its id.
[[nodiscard]] std::expected<QString, Failure> backup(const ResticRepository &repository, const Backup &backup);

// The snapshots that have every one of `tags`, oldest first.
[[nodiscard]] std::expected<QList<Snapshot>, Failure> snapshots(const ResticRepository &repository, const QStringList &tags);

// Forgets the snapshots of these ids and gives back the room only they took.
[[nodiscard]] std::expected<void, Failure> forget(const ResticRepository &repository, const QStringList &ids);

// Restores the snapshot `id` into `target`, each path as it was under it
// (target + the path), only what `including` names (whole folders) when given.
[[nodiscard]] std::expected<void, Failure> restore(const ResticRepository &repository, const QString &id, const QString &target,
                                                   const QStringList &including = {});

// `path` as a restic pattern that matches only that path: its *, ?, [ and \ escaped.
[[nodiscard]] QString literal(const QString &path);

} // namespace Restic
