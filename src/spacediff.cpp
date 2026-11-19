#include "spacediff.h"

#include <QStringList>
#include <algorithm>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

using SpaceDiff::Count;

template <typename Thing> [[nodiscard]] bool alive(const Thing &thing) { return !SpaceReducer::isRemoved(thing.alive, thing.removed); }

// Whether two sets of stamped values say the same, whenever each was said.
template <typename Stamped> [[nodiscard]] bool sameValues(const Stamped &one, const Stamped &other) {
    QStringList keys = one.keys() + other.keys();
    keys.removeDuplicates();
    return std::ranges::all_of(keys, [&one, &other](const QString &key) { return one.value(key).value == other.value(key).value; });
}

// Of each thing there after, whether it came, changed or went; `there` says
// whether one is there, `same` whether it says the same as before.
template <typename Map, typename There, typename Same>
[[nodiscard]] Count count(const Map &before, const Map &after, There there, Same same) {
    Count count;
    for (auto thing = after.cbegin(); thing != after.cend(); ++thing) {
        const auto was = before.constFind(thing.key());
        const bool had = was != before.cend() && there(*was);
        if (!there(*thing))
            count.removed += had ? 1 : 0;
        else if (!had)
            ++count.added;
        else
            count.changed += same(*was, *thing) ? 0 : 1;
    }
    // Operations only add, so nothing there before is missing after; were
    // it, it went.
    for (auto thing = before.cbegin(); thing != before.cend(); ++thing)
        count.removed += !after.contains(thing.key()) && there(*thing) ? 1 : 0;
    return count;
}

// What only grows says nothing new of what is there already.
template <typename Thing> [[nodiscard]] bool always([[maybe_unused]] const Thing &was, [[maybe_unused]] const Thing &is) { return true; }

[[nodiscard]] QJsonObject countJson(const Count &count) {
    return {{"added", count.added}, {"changed", count.changed}, {"removed", count.removed}};
}

[[nodiscard]] QString said(const Count &count, const QString &what) {
    QStringList parts;
    if (count.added > 0)
        parts << u"%1 new"_s.arg(count.added);
    if (count.changed > 0)
        parts << u"%1 changed"_s.arg(count.changed);
    if (count.removed > 0)
        parts << u"%1 gone"_s.arg(count.removed);
    return parts.isEmpty() ? QString() : u"%1: %2"_s.arg(what, parts.join(", "_L1));
}

} // namespace

SpaceDiff::Changes SpaceDiff::between(const ReducedState &before, const ReducedState &after) {
    const auto aThing = [](const ReducedNode &node) { return alive(node) && node.fields.value(u"kind"_s).value != "book"_L1; };
    const auto aDay = [](const ReducedDay &day) { return day.valid; };
    return {
        .books = count(before.books, after.books, alive<ReducedBook>,
                       [](const ReducedBook &was, const ReducedBook &is) {
                           return sameValues(was.fields, is.fields) && sameValues(was.tagSet, is.tagSet);
                       }),
        .collections = count(before.collections, after.collections, alive<ReducedCollection>,
                             [](const ReducedCollection &was, const ReducedCollection &is) {
                                 return sameValues(was.fields, is.fields) && sameValues(was.books, is.books);
                             }),
        .things = count(before.nodes, after.nodes, aThing,
                        [](const ReducedNode &was, const ReducedNode &is) {
                            return sameValues(was.fields, is.fields) && sameValues(was.aliases, is.aliases);
                        }),
        .facts = count(before.facts, after.facts, alive<ReducedFact>, always<ReducedFact>),
        .relations = count(before.edges, after.edges, alive<ReducedEdge>,
                           [](const ReducedEdge &was, const ReducedEdge &is) { return was.note.value == is.note.value; }),
        .days = count(before.days, after.days, aDay,
                      [](const ReducedDay &was, const ReducedDay &is) { return was.from == is.from && was.to == is.to; }),
    };
}

QJsonObject SpaceDiff::toJson(const Changes &changes) {
    return {{"books", countJson(changes.books)},         {"collections", countJson(changes.collections)},
            {"things", countJson(changes.things)},       {"facts", countJson(changes.facts)},
            {"relations", countJson(changes.relations)}, {"reading_days", countJson(changes.days)}};
}

QString SpaceDiff::describe(const Changes &changes) {
    QStringList parts;
    for (const auto &[count, what] : {std::pair{changes.books, u"books"_s}, std::pair{changes.collections, u"collections"_s},
                                      std::pair{changes.things, u"things of the graph"_s}, std::pair{changes.facts, u"facts"_s},
                                      std::pair{changes.relations, u"relations"_s}, std::pair{changes.days, u"reading days"_s}})
        if (const QString line = said(count, what); !line.isEmpty())
            parts << line;
    return parts.isEmpty() ? u"nothing it says changes"_s : parts.join("; "_L1);
}
