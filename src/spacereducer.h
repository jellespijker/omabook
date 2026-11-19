#pragma once

#include <QHash>
#include <QMap>
#include <QString>
#include <QtTypes>
#include <span>
#include <tuple>

#include "spacereplay.h"

// Pure CRDT state reduction for space operations (ADR 0001).
//
// Ordering by `(hlc, device, priority, value)` makes every reduction
// commutative, associative and idempotent: the latest HLC wins, a tie goes to
// the larger device id, and a higher priority makes an add win over a
// concurrent remove at the same stamp.
struct SpaceStamp {
    qint64 hlc = -1;
    QString device;
    int priority = 0;
    QString value;

    bool operator<(const SpaceStamp &other) const noexcept {
        return std::tie(hlc, priority, device, value) < std::tie(other.hlc, other.priority, other.device, other.value);
    }
    bool operator>(const SpaceStamp &other) const noexcept { return other < *this; }
    bool operator==(const SpaceStamp &) const = default;
};

struct SpaceMoment {
    int section = 0;
    double fraction = 0;
    QString anchor;

    [[nodiscard]] bool earlierThan(const SpaceMoment &other) const noexcept;
};

struct ReducedBook {
    QHash<QString, SpaceStamp> fields;
    QMap<QString, SpaceStamp> tagSet;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedCollection {
    QHash<QString, SpaceStamp> fields;
    QMap<QString, SpaceStamp> books;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedNode {
    QHash<QString, SpaceStamp> fields;
    QMap<QString, SpaceStamp> aliases;
    QMap<QString, SpaceMoment> seen;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedFact {
    QString nodeGid;
    QString kind;
    QString value;
    QString bookGid;
    SpaceMoment moment;
    bool hasMoment = false;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedEdge {
    QString sourceGid;
    QString targetGid;
    QString kind;
    SpaceStamp note;
    QString bookGid;
    SpaceMoment learned;
    bool hasLearned = false;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedDay {
    QString bookGid;
    QString day;
    double from = 100.0;
    double to = 0.0;
    bool valid = false;
};

struct ReducedState {
    QMap<QString, ReducedBook> books;
    QMap<QString, ReducedCollection> collections;
    QMap<QString, ReducedNode> nodes;
    QMap<QString, ReducedFact> facts;
    QMap<QString, ReducedEdge> edges;
    QMap<QString, ReducedDay> days;
};

namespace SpaceReducer {

[[nodiscard]] bool isRemoved(const SpaceStamp &alive, const SpaceStamp &removed) noexcept;
void foldBook(ReducedBook &book, const SpaceOperation &op);
[[nodiscard]] qint64 fieldHlc(const ReducedBook &book, const QString &field);
[[nodiscard]] qint64 detailsHlc(const ReducedBook &book);
[[nodiscard]] qint64 readingHlc(const ReducedBook &book);
// Where two computers name one book by different ids, a same-as operation
// (entity "book", field "same_as", the one id and the other as its value)
// links them. Links only ever grow, so every id of a linked group stands
// for its smallest, in any order: each linked id and the one it stands for.
[[nodiscard]] QHash<QString, QString> sameAs(std::span<const SpaceOperation> ops);
// Books are folded under the id their same-as links stand for.
[[nodiscard]] ReducedState reduce(std::span<const SpaceOperation> ops);
// What one computer holds: only the operations its device wrote, which is what
// its rows are (every write there appends one), with the links of all of them.
[[nodiscard]] ReducedState reduce(std::span<const SpaceOperation> ops, const QString &device);

} // namespace SpaceReducer
