#pragma once

#include <QJsonObject>
#include <QString>
#include <QtTypes>

#include "spacereducer.h"

// What a space's operations change of what it says (#7): of a space held
// before and the same with more operations, how many books and collections
// came, changed and went, and of its graph how many things, facts and
// relations. What following a link offers before it is taken in.
namespace SpaceDiff {

struct Count {
    qsizetype added = 0, changed = 0, removed = 0;
    [[nodiscard]] qsizetype any() const { return added + changed + removed; }
    bool operator==(const Count &) const = default;
};

struct Changes {
    Count books, collections, things, facts, relations, days;
    [[nodiscard]] qsizetype any() const {
        return books.any() + collections.any() + things.any() + facts.any() + relations.any() + days.any();
    }
    bool operator==(const Changes &) const = default;
};

[[nodiscard]] Changes between(const ReducedState &before, const ReducedState &after);

// {"books": {"added", "changed", "removed"}, …}
[[nodiscard]] QJsonObject toJson(const Changes &changes);
// "2 books new, 1 changed; 1 collection gone; 4 facts new", or "nothing it says changes".
[[nodiscard]] QString describe(const Changes &changes);

} // namespace SpaceDiff
