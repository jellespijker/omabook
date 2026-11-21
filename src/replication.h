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

#include "bookpairing.h"
#include "library.h"
#include "spacereducer.h"
#include "spacereplay.h"

// Replicating the change log between computers by version vector (ADR 0001).
//
// What travels by the log so far is the books' reading places, details and
// what is said of their reading, with the same-as operations that line up the
// books where two computers' content ids don't (#36, #37), the collections
// and the general knowledge graph and story worlds' (#25), whether one follows a subscribed space
// (#51), and the bookmarks and highlights (#66); everything else
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
    // today's rules (BookPairing): the hashes of its files, else its strong
    // keys no other book here has, and its title.
    using BookNames = BookPairing::Names;

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
    // Of the bookmarks and highlights alone (#66), the last of each space and
    // device held. An Omabook from before they travelled took none of them,
    // though it holds the operations after them: it says no such vector, and
    // once it does, it is sent each it lacks by this one, whatever its other
    // vector says.
    [[nodiscard]] static Vector annotationVector(const Library &library);
    // The same as since() but the bookmarks and highlights, which are those after `annotations`.
    [[nodiscard]] static QList<SpaceOperation> since(const Library &library, const Vector &known, const Vector &annotations);

    [[nodiscard]] static QJsonObject toJson(const Vector &vector);
    [[nodiscard]] static Vector vectorFrom(const QJsonObject &object);
    // Each operation as an array of its nine parts, and `signatures` its
    // signer and signature after them (an Omabook from before signing takes
    // only the nine, so it is sent only those).
    [[nodiscard]] static QJsonArray toJson(std::span<const SpaceOperation> ops, bool signatures = true);
    // Only the operations that travel, and those of a kind this Omabook
    // doesn't know (OpKinds), which it keeps (KeptOps): anything else in
    // `array` is left out.
    [[nodiscard]] static QList<SpaceOperation> operationsFrom(const QJsonArray &array);
    // Every operation of `array` that is whole, of any space: what one's own
    // computers carry of the shared and subscribed spaces (SpaceCarry, #51).
    [[nodiscard]] static QList<SpaceOperation> anyOperationsFrom(const QJsonArray &array);

    // Of the operations a peer sent, those that may be taken in, and how many were refused.
    struct Admitted {
        QList<SpaceOperation> ops;
        int refused = 0;
    };
    // Every signed operation is verified before anything is taken of it, and
    // refused unless it is as its author signed it. An unsigned one, as an
    // Omabook from before signing writes them, is taken only from one of
    // one's own computers (`peer`, a device of the personal space).
    [[nodiscard]] static Admitted admit(const Library &library, const QList<SpaceOperation> &incoming, const QString &peer);
    [[nodiscard]] static QJsonArray toJson(const QList<BookNames> &books);
    [[nodiscard]] static QList<BookNames> booksFrom(const QJsonArray &array);

    // The books here whose id the other side, which knows `theirGids`, may
    // know by another one: those it doesn't know by theirs, nor by a same-as.
    [[nodiscard]] static QList<BookNames> unlinked(const Library &library, const QSet<QString> &theirGids);
    // What finding each of `theirs` here with today's rules (BookPairing)
    // would link; `theirGids` are the ids of all the books they hold.
    [[nodiscard]] static BookPairing::Pairs pairs(const Library &library, const QList<BookNames> &theirs,
                                                  const QSet<QString> &theirGids = {});
    // The same, each found that is known by another id here recorded as a
    // same-as operation once, folding the larger id into the smaller.
    // Answers how many it linked.
    static int link(const Library &library, const QList<BookNames> &theirs, const QSet<QString> &theirGids = {});

    // What taking `incoming` (with the log already here) would make of the
    // reading places of the books here: the books whose place would change.
    [[nodiscard]] static QList<PlaceChange> placeChanges(const Library &library, std::span<const SpaceOperation> incoming = {});
};
