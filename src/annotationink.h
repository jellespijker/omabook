#pragma once

#include <QString>
#include <QtTypes>

#include <expected>
#include <vector>

#include "inkstroke.h"

class Library;

// Strokes kept on an annotation (#90): a handwritten margin note's ink, kept
// as book ink keeps a page's (#83, ADR 0005). They are a set of the
// annotation in the change log ("set:strokes", as a plugin's sets are, ADR
// 0002): one operation an ink batch, of each stroke the latest word holding,
// so they go with the annotation to one's other computers and what two of
// them wrote is all kept, an erasing on either going to both. They are never
// rows, so the library's format is the same with them as without.
namespace AnnotationInk {

// The annotation's strokes, one over the other in the order they began (a tie
// by id, the same on every computer); none for one that has none.
[[nodiscard]] std::vector<Ink::Stroke> strokes(const Library &library, qint64 annotationId);
[[nodiscard]] bool has(const Library &library, qint64 annotationId);

// What an ink surface did to them (an ink batch, an undo of one), kept in
// the change log in one write. Why not, when it can't be: there is no such
// annotation.
std::expected<void, QString> write(const Library &library, qint64 annotationId, const Ink::Change &change);

} // namespace AnnotationInk
