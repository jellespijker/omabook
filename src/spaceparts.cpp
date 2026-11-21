#include "spaceparts.h"

#include <QString>
#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace {

using SpaceParts::Part;
using SpaceParts::Parts;

constexpr auto switchPrefix = "part:"_L1;

const std::array<Part, 6> registry{{
    {.name = "graph"_L1, .on = &Parts::graph, .switched = false},
    {.name = "questions"_L1, .on = &Parts::questions, .switched = false},
    {.name = "ratings"_L1, .on = &Parts::ratings, .switched = false},
    {.name = "reading"_L1, .on = &Parts::reading, .switched = false},
    {.name = "files"_L1, .on = &Parts::files, .switched = false},
    {.name = "annotations"_L1, .on = &Parts::annotations, .switched = true},
}};

} // namespace

std::span<const Part> SpaceParts::all() { return registry; }

const Part *SpaceParts::named(QStringView name) {
    const auto *const found = std::ranges::find_if(registry, [name](const Part &part) { return name == part.name; });
    return found == registry.end() ? nullptr : &*found;
}

QStringList SpaceParts::names() {
    QStringList list;
    for (const Part &part : registry)
        list << QString(part.name);
    return list;
}

QString SpaceParts::switchField(QLatin1StringView name) { return switchPrefix + name; }

const Part *SpaceParts::switchedBy(QStringView field) {
    if (!field.startsWith(switchPrefix))
        return nullptr;
    const Part *part = named(field.mid(switchPrefix.size()));
    return part != nullptr && part->switched ? part : nullptr;
}

QStringList Parts::names() const {
    QStringList list;
    for (const Part &part : registry)
        if (this->*part.on)
            list << QString(part.name);
    return list;
}

Parts Parts::fromNames(const QStringList &names) {
    Parts parts;
    for (const Part &part : registry)
        parts.*part.on = names.contains(part.name);
    return parts;
}
