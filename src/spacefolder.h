#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>
#include <span>

#include "spacekey.h"
#include "spacereplay.h"

// A shared space's operations in a folder that something else syncs
// (Syncthing, a mounted share): a transport (ADR 0001, *Transports are
// adapters*; #38) that is trusted with nothing but keeping files.
//
// The folder holds, for each space, a folder named by the space's id
// (without "shared:"), and in it the batches each member's computer wrote:
// `<device>-<first>-<last>.omaseal`, the numbers the first and last of that
// device's operations it holds (twelve digits). Each is append-only: written
// once, whole (to a temporary name, then renamed), never changed, and
// a computer writes only its own. A batch is the operations signed by their
// authors (SignedOps) and sealed with the space's key (SpaceKey), so the
// folder never holds plaintext, and a batch changed there, cut short, or
// sealed for another space is refused whole. Reading takes the batches with
// operations newer than what is held of each device (a version vector).
//
// WebDAV and S3 would carry the same files; they are not built.
namespace SpaceFolder {

inline constexpr auto suffix = ".omaseal";

// The folder in `folder` that holds the batches of `space`.
[[nodiscard]] QString of(const QString &folder, const QString &space);

// The last number of `device`'s operations in its batches there; 0 for none.
[[nodiscard]] qint64 lastWritten(const QString &folder, const QString &space, const QString &device);

// Writes `ops`, all of `device` and signed, as one sealed batch. The file's path.
[[nodiscard]] std::expected<QString, QString> write(const QString &folder, const QString &space, const SpaceKey &key, const QString &device,
                                                    std::span<const SpaceOperation> ops);

struct Read {
    QList<SpaceOperation> ops; // each as its author signed it
    qsizetype batches = 0;     // opened
    QStringList refused;       // of each batch refused, its name and why
};
// The operations of the batches newer than `held` (each device's last
// number held here), each batch opened and checked whole.
[[nodiscard]] Read read(const QString &folder, const QString &space, const SpaceKey &key, const QHash<QString, qint64> &held);

} // namespace SpaceFolder
