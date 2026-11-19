#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>
#include <span>

class Library;

// One operation in a space's change log (`ops` table, ADR 0001).
struct SpaceOperation {
    QString space;
    QString device;
    qint64 seq = 0;
    qint64 hlc = 0;
    QString author;
    QString entity;
    QString id;
    QString field;
    QString value;
    // Who signed it, a person's id (its author), and the signature of its
    // canonical bytes (SignedOps); both empty for one written before
    // operations were signed, or by an Omabook from before that.
    QString signer{};
    QByteArray signature{};

    bool operator==(const SpaceOperation &) const = default;
};

// Replaying and merging space operations (ADR 0001).
//
// Operations commute, associate and are idempotent: any order of the same
// operations yields the same state (fields by latest HLC with device tie-break,
// sets with add winning over concurrent remove, graph facts and sightings by
// earliest revealed moment, removals as tombstones that a later write revives).
class SpaceReplay {
public:
    // Every operation that still matters (Compaction: the snapshot and what came
    // after it, which merge as all of `ops` do), ordered by space, device and seq.
    [[nodiscard]] static QList<SpaceOperation> operations(const Library &library);

    // Applies `ops` (in any order) to `library`, merging with any operations
    // already in `ops` and rebuilding the travelling rows.
    static bool apply(Library &library, std::span<const SpaceOperation> ops);

    // Takes in operations from another computer of the kinds that travel by
    // the log (Replication), and brings the reading places of the books here
    // in step with what the log now says. Answers the books whose place changed.
    static QList<qint64> takePlaces(Library &library, std::span<const SpaceOperation> ops);

    // Rebuilds all travelling rows of `library` solely from its `ops` log.
    static bool replay(Library &library);

    // The latest HLC recorded in `ops` for each detail field of a book.
    [[nodiscard]] static QHash<QString, qint64> bookFieldHlcs(const Library &library, qint64 bookId);

    // The latest HLC recorded in `ops` for an entity (or 0 when none).
    [[nodiscard]] static qint64 entityHlc(const Library &library, const QString &entity, const QString &globalId);
    [[nodiscard]] static qint64 fieldHlc(const Library &library, const QString &entity, const QString &globalId, const QString &field);
};
