#pragma once

#include <QStringList>

// The annotations table's formats (LibrarySchema's steps for it).
namespace AnnotationSchema {

// Highlights and margin notes on a passage (#80): the table made anew with
// where a passage ends, its words with a few before and after, the file its
// places were found in, how sure that was, and whether it is lost.
[[nodiscard]] QStringList passages();
// A highlight's colour (#81): the table made anew with the colour's name
// ("yellow", HighlightColors), '' for the first of them.
[[nodiscard]] QStringList colors();
// A part of a page (#115, format 38): the table made anew with `area`, the
// rectangle on its page (PageRegion's measure, "x y width height"; '' for one
// on words) of a highlight or a note on a comic's panel; and the word index
// of what the reader wrote (annotation_words), kept by triggers.
[[nodiscard]] QStringList regions();
// The triggers that keep annotation_words as the table is written: a later
// step that makes the table anew makes these again after it.
[[nodiscard]] QStringList wordTriggers();

} // namespace AnnotationSchema
