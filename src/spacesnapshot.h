#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>

#include "spacereplay.h"

// A space's snapshot as one file, `.omabook-space` (ADR 0001, *Compaction*;
// #2): what a published space is when it goes by mail or chat. It is the
// space's operations compacted, with the space's id and kind, its owner (the
// name they chose and their person id), the snapshot's clock and the Omabook
// that wrote it.
struct SpaceSnapshot {
    struct Owner {
        QString name;   // as the owner chose to be called
        QString person; // their id (Person::id), the public half of their key
        bool operator==(const Owner &) const = default;
    };

    QString space; // its id, "published:…"
    QString kind;  // "published"; the kinds of ADR 0001, *Spaces*
    QString name;  // what it is called: the collection's name
    Owner owner;
    // The optional parts it holds (PublishedSpace::Parts), by name.
    QStringList parts;
    // The hybrid logical clock (hlc.h) it was taken at: no operation in it is later.
    qint64 clock = 0;
    QString writtenBy; // the version of Omabook that wrote it
    // Of this space, each with `space` set to it.
    QList<SpaceOperation> operations;

    bool operator==(const SpaceSnapshot &) const = default;
};

// The file's format. JSON, so that whoever shares a space can open the file
// and see all it says before it goes (nothing personal leaves by accident),
// as the log already travels between computers (Replication); Qt reads and
// writes it with no other library, and a collection's worth of it is small.
// Each operation is a line of its own, an array in the order of `ops`
// without the space (the file's): device, sequence, HLC, author, entity, id,
// field, value.
//
// The version is "major.minor". A reader refuses a major version it doesn't
// know, saying why; a minor version only adds, so a reader ignores the
// fields it doesn't know, of the file and at the end of an operation.
namespace SpaceSnapshotFile {

inline constexpr int major = 1;
inline constexpr int minor = 0;
inline constexpr auto suffix = ".omabook-space";

[[nodiscard]] QByteArray write(const SpaceSnapshot &snapshot);
[[nodiscard]] std::expected<SpaceSnapshot, QString> read(QByteArrayView bytes);

[[nodiscard]] std::expected<void, QString> save(const SpaceSnapshot &snapshot, const QString &path);
[[nodiscard]] std::expected<SpaceSnapshot, QString> load(const QString &path);

} // namespace SpaceSnapshotFile
