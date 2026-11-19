#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <span>

#include "library.h"
#include "spacereducer.h"
#include "spacereplay.h"

// Replicating the change log between computers by version vector (ADR 0001).
//
// What travels by the log so far is the books' reading places, details and
// what is said of their reading, with the same-as operations that line up the
// books where two computers' content ids don't (#36, #37), and the
// collections; everything else
// still travels by the manifest, one kind moving over at a time. Two computers exchange what they have of it, per
// space and device the last sequence number (`{laptop: 812, desktop: 77}`),
// and send each other the operations after that. Taking an operation twice
// changes nothing.
//
// The vector is of the operations that travel, never of the whole log: a
// computer holds every one of them a device wrote up to the number it says,
// because it is always sent all of them after that number at once.
class Replication {
public:
    // Space, then device, then the last sequence number held.
    using Vector = QMap<QString, QMap<QString, qint64>>;

    // A book as the other side needs it to find its own copy of it with
    // today's rules (the hashes of its files, else its entry keys).
    struct BookNames {
        QString gid;
        QStringList hashes;
        QStringList keys;
    };

    // A book whose reading place the log says otherwise than its row.
    struct PlaceChange {
        qint64 book = 0;
        Progress place;
    };

    [[nodiscard]] static bool travels(const SpaceOperation &op);

    // A book as the log says it is, in the shape of a manifest's book: its
    // details with when each changed, and what is said of its reading.
    [[nodiscard]] static QJsonObject bookView(const ReducedBook &book);

    // Every operation of the log that travels, in order of space, device and sequence.
    [[nodiscard]] static QList<SpaceOperation> operations(const Library &library);
    [[nodiscard]] static Vector vector(const Library &library);
    // The operations that travel which a computer with `known` lacks.
    [[nodiscard]] static QList<SpaceOperation> since(const Library &library, const Vector &known);

    [[nodiscard]] static QJsonObject toJson(const Vector &vector);
    [[nodiscard]] static Vector vectorFrom(const QJsonObject &object);
    [[nodiscard]] static QJsonArray toJson(std::span<const SpaceOperation> ops);
    // Only the operations that travel: anything else in `array` is left out.
    [[nodiscard]] static QList<SpaceOperation> operationsFrom(const QJsonArray &array);
    [[nodiscard]] static QJsonArray toJson(const QList<BookNames> &books);
    [[nodiscard]] static QList<BookNames> booksFrom(const QJsonArray &array);

    // The books here whose id the other side, which knows `theirGids`, may
    // know by another one: those it doesn't know by theirs, nor by a same-as.
    [[nodiscard]] static QList<BookNames> unlinked(const Library &library, const QSet<QString> &theirGids);
    // Finds each book here with today's rules (a file with the same content,
    // or for a book without a file the same entry key) and, where it is
    // known by another id here, records a same-as operation once, folding
    // the larger id into the smaller. Answers how many it linked.
    static int link(const Library &library, const QList<BookNames> &theirs);

    // What taking `incoming` (with the log already here) would make of the
    // reading places of the books here: the books whose place would change.
    [[nodiscard]] static QList<PlaceChange> placeChanges(const Library &library, std::span<const SpaceOperation> incoming = {});
};
