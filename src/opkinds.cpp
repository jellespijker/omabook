#include "opkinds.h"

#include <QList>
#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>

#include "plugindata.h"

using namespace Qt::StringLiterals;

namespace {

constexpr std::array bookFields{"progress"_L1, "same_as"_L1, "title"_L1, "authors"_L1, "series"_L1, "series_index"_L1, "publisher"_L1,
                                "language"_L1, "isbn"_L1, "published"_L1, "description"_L1, "tags"_L1, "identifiers"_L1, "has_cover"_L1,
                                "cover_hash"_L1, "details_at"_L1, "shelf"_L1, "rating"_L1, "finished"_L1, "paper"_L1, "reading_at"_L1,
                                "removed"_L1, "removed_at"_L1,
                                // Two ids the log says are not one book, whatever a link says (#158).
                                "not_same_as"_L1,
                                // Who built its knowledge graph, and when (#219, GraphBuilt).
                                "graph"_L1};

// A book's fields this computer keeps to itself, never sent.
constexpr std::array ownBookFields{"path"_L1, "formats"_L1};

// What else an operation is of, every field of it: the collections, the
// general graph and a story's world, the bookmarks and highlights, the
// spaces followed, what a published or shared space says of itself, and
// the year's reading goal (#68), and the books plugins keep private (#127).
constexpr std::array entities{"collection"_L1, "node"_L1,  "fact"_L1,   "edge"_L1,  "reading_log"_L1, "subscription"_L1, "annotation"_L1,
                              "world"_L1,      "space"_L1, "member"_L1, "owner"_L1, "goal"_L1,        "private"_L1};

struct Forgotten {
    QString entity;
    QString field;
};

QList<Forgotten> &forgotten() {
    thread_local QList<Forgotten> kinds;
    return kinds;
}

template <std::size_t N> [[nodiscard]] bool among(const std::array<QLatin1StringView, N> &names, const QString &name) {
    return std::ranges::any_of(names, [&name](QLatin1StringView one) { return name == one; });
}

[[nodiscard]] bool isForgotten(const SpaceOperation &op) {
    return std::ranges::any_of(
        forgotten(), [&op](const Forgotten &kind) { return op.entity == kind.entity && (kind.field.isEmpty() || op.field == kind.field); });
}

} // namespace

std::span<const QLatin1StringView> OpKinds::travellingBookFields() { return bookFields; }

bool OpKinds::travelsOfABook(const QString &field) { return among(bookFields, field) || field.startsWith("tag:"_L1); }

bool OpKinds::known(const SpaceOperation &op) {
    if (isForgotten(op))
        return false;
    if (PluginData::isPluginSpace(op.space))
        return true;
    if (op.entity == "book"_L1)
        return travelsOfABook(op.field) || among(ownBookFields, op.field);
    return among(entities, op.entity);
}

OpKinds::Forgetting::Forgetting(QString entity, QString field) : m_entity(std::move(entity)), m_field(std::move(field)) {
    forgotten().append({.entity = m_entity, .field = m_field});
}

OpKinds::Forgetting::~Forgetting() {
    forgotten().removeIf([this](const Forgotten &kind) { return kind.entity == m_entity && kind.field == m_field; });
}
