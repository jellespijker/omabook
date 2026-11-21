#pragma once

#include <QLatin1StringView>
#include <QStringList>
#include <QStringView>
#include <span>

// The parts a space may hold beyond its collection and its books (ADR 0001,
// *Spaces*; the parts registry of ADR 0002, #85): one table of them, which
// what publishes, founds, switches, lists and asks for parts reads, so a part
// is named, switched and told of the same way everywhere.
//
// What is a person's own (the stars they gave, how far they read, what they
// wrote in the margins) is off until they switch it on: nothing personal
// leaves by accident.
namespace SpaceParts {

// The parts switched on.
struct Parts {
    bool graph = false;     // the knowledge graph of its books: things, what is known of them, how they relate
    bool questions = false; // the questions asked of its books
    bool ratings = false;   // the stars given them
    bool reading = false;   // how far each was read, its shelf and finished day, and the days it was read on
    // Which file each book has (format, hash and size, never where it lies),
    // so that whoever the space reaches can fetch it (SpaceFiles, #63).
    bool files = false;
    // The highlights, margin notes and book ink of its books (#85), which its
    // other members see as an annotation layer (AnnotationLayers).
    bool annotations = false;

    // By name ("graph", "questions", …), as a snapshot keeps them; a name not
    // known here is left out.
    [[nodiscard]] QStringList names() const;
    [[nodiscard]] static Parts fromNames(const QStringList &names);

    bool operator==(const Parts &) const = default;
};

// One part, as the registry knows it.
struct Part {
    QLatin1StringView name;
    bool Parts::*on = nullptr;
    // Switched on and off in a shared space after its founding, by an
    // operation of the space (entity "space", field "part:<name>"), rather than
    // only chosen at its founding. Files have a switch of their own (SpaceFiles).
    bool switched = false;
};

// Every part, in the order they are listed.
[[nodiscard]] std::span<const Part> all();
// The part of that name, or none.
[[nodiscard]] const Part *named(QStringView name);
// The names of every part.
[[nodiscard]] QStringList names();
// The field of a shared space's operation that switches a part.
[[nodiscard]] QString switchField(QLatin1StringView name);
// The part an operation's field switches, or none.
[[nodiscard]] const Part *switchedBy(QStringView field);

} // namespace SpaceParts
