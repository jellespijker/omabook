#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>
#include <span>
#include <utility>

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
// Each batch is sealed with one of the space's keys; since a member was
// removed, the newer ones with a key they don't hold (SharedKeys).
//
// When the space lets its books' files travel (#63), they are kept beside
// the batches, in `files/`: each one sealed with the space's key as a batch
// is (its parts the file in pieces, the space's id and the file's hash
// their additional data), and named by a hash of the file's hash keyed with
// the key the space was founded with, which every member holds, so the
// folder says nothing of which file it is. Written once, whole, by whoever
// has the file; opened only by a member who wants it, and checked against
// its hash.
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
// number held here), each batch opened with whichever of `keys` (every
// generation of the space's key held, #49) it was sealed with and checked
// whole. A device's batches are read in order and up to the first that
// doesn't open: what comes after it waits, so that nothing is taken past
// what is missing (a later key may open it).
[[nodiscard]] Read read(const QString &folder, const QString &space, std::span<const SpaceKey> keys, const QHash<QString, qint64> &held);

// Where the book's file of `hash` is kept in the folder of `space`, named
// with `keys` (every generation of the space's key held, the founding one
// first); empty without a key.
[[nodiscard]] QString fileOf(const QString &folder, const QString &space, std::span<const SpaceKey> keys, const QString &hash);
// Keeps the file at `path`, whose hash is `hash`, sealed with `sealing`,
// unless it is there already. Whether it wrote it.
[[nodiscard]] std::expected<bool, QString> writeFile(const QString &folder, const QString &space, std::span<const SpaceKey> keys,
                                                     const SpaceKey &sealing, const std::pair<QString, QString> &hashAndPath);
// Opens the file of `hash` into `into`, with whichever of `keys` it was
// sealed with, refused unless it is whole and its content has that hash.
[[nodiscard]] std::expected<void, QString> readFile(const QString &folder, const QString &space, std::span<const SpaceKey> keys,
                                                    const QString &hash, const QString &into);

} // namespace SpaceFolder
