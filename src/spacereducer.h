#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QtTypes>
#include <span>
#include <tuple>
#include <utility>

#include "bookplace.h"
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
    BookPlace place;
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
    // Where it was met, by book: the earliest place since it was last taken away there.
    QMap<QString, SpaceMoment> seen;
    // What that is made of (#136): each place said of a book with its stamp,
    // and the stamp of the latest word that it was taken away from there.
    QMap<QString, QList<std::pair<qint64, SpaceMoment>>> sightings;
    QMap<QString, qint64> unseen;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedFact {
    QString nodeGid;
    QString kind;
    QString value;
    QString bookGid;
    QString tenant; // written by hand in it (#109)
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
    // What other books and tenants noted of it (graph_edges.notes, #109), the latest said.
    SpaceStamp notes;
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
    // How long it took (#68): the most any computer said, as the same
    // day's reading is said again by each that has it.
    qint64 seconds = 0;
    // Whether the day is there: said of a book and a day, and not taken
    // back by a removal ("removed" "1") later than what was said of it. Only
    // a published space takes one back, when its reading is switched off.
    bool valid = false;
    SpaceStamp alive;
    SpaceStamp removed;
};

// A bookmark or highlight (#66): of each field (its book's id, kind, place,
// words and note) the latest written, and a removal as a tombstone.
struct ReducedAnnotation {
    QHash<QString, SpaceStamp> fields;
    SpaceStamp alive;
    SpaceStamp removed;
};

struct ReducedState {
    QMap<QString, ReducedBook> books;
    QMap<QString, ReducedCollection> collections;
    QMap<QString, ReducedNode> nodes;
    QMap<QString, ReducedFact> facts;
    QMap<QString, ReducedEdge> edges;
    QMap<QString, ReducedDay> days;
    QMap<QString, ReducedAnnotation> annotations;
};

namespace SpaceReducer {

[[nodiscard]] bool isRemoved(const SpaceStamp &alive, const SpaceStamp &removed) noexcept;
void foldBook(ReducedBook &book, const SpaceOperation &op);
[[nodiscard]] qint64 fieldHlc(const ReducedBook &book, const QString &field);
[[nodiscard]] qint64 detailsHlc(const ReducedBook &book);
[[nodiscard]] qint64 readingHlc(const ReducedBook &book);
// The fields of which every operation counts, not only the latest: what was
// met where, what was learned, how things relate, the days read, the
// links that line up ids, and the changes of a plugin's sets (PluginData).
[[nodiscard]] bool grows(const QString &field);
// Where a device wrote an operation: its space, its sequence there, its stamp.
struct Written {
    QString space;
    qint64 seq = 0;
    qint64 hlc = 0;
};
// Whether a device wrote `op` after `before`, of one field: by sequence in
// one space, else by stamp, as a device that joined another's space (#51)
// numbers its operations there from 1 again, after those of its own (#216).
[[nodiscard]] bool writtenAfter(const Written &op, const Written &before);
// Where two computers name one book by different ids, a same-as operation
// (entity "book", field "same_as", the one id and the other as its value)
// links them. Links only ever grow, so every id of a linked group stands
// for its smallest, in any order: each linked id and the one it stands for.
// The same of things of the graph (entity "node"), linked once where two
// computers know one by different names.
[[nodiscard]] QHash<QString, QString> sameAs(std::span<const SpaceOperation> ops, QLatin1StringView entity = QLatin1StringView("book"));
// Books and things are folded under the id their same-as links stand for.
[[nodiscard]] ReducedState reduce(std::span<const SpaceOperation> ops);
// What one computer holds: of the operations its device wrote, each field's
// last, which is what its rows are (every write there appends one, and one
// taken from elsewhere keeps the stamp it had there), with the links of all
// of them.
[[nodiscard]] ReducedState reduce(std::span<const SpaceOperation> ops, const QString &device);
// The same, each book and thing under the id its own operations name it by,
// no same-as link followed: what that computer removed by name (a file's
// hash, an entry key), which a link must not lend to another copy (#58).
[[nodiscard]] ReducedState reduceByName(std::span<const SpaceOperation> ops, const QString &device);

} // namespace SpaceReducer
