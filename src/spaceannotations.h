#pragma once

#include <QSet>
#include <QString>
#include <functional>

#include "spacereducer.h"
#include "spacereplay.h"

// What a space with its annotations part switched on (SpaceParts, #85)
// holds of a person's annotations: their highlights, margin notes and book
// ink in the space's books, every field of each as the personal space has
// it, a margin note's or a page's strokes too ("set:strokes"). A bookmark is
// one's own way back into a book and stays home. One removed goes as its
// removal alone, so whoever had it takes it back and learns nothing of what
// it said.
//
// What the others make of it, as far as each has read, is AnnotationLayers'.
namespace SpaceAnnotations {

// The kinds that go into a space.
[[nodiscard]] bool travels(const QString &kind);

struct Chosen {
    QSet<QString> alive;   // by global id: every field goes
    QSet<QString> removed; // only the removal goes
};

// Of a person's annotations as `state` has them, those of a book `holds` says
// the space holds (by the book's global id as the annotation names it).
[[nodiscard]] Chosen choose(const ReducedState &state, const std::function<bool(const QString &bookGid)> &holds);

// Whether an operation of the personal space goes into the space.
[[nodiscard]] bool goes(const SpaceOperation &op, const Chosen &chosen);

} // namespace SpaceAnnotations
