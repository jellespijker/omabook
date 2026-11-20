#pragma once

#include <QList>
#include <QString>
#include <QVariantMap>
#include <QtTypes>

#include "library.h"

class Settings;

// The reader's own annotations as the AI is given them (#86): the reading
// companion and ask-the-book may draw on one's highlights, margin notes and
// bookmarks ("what did I highlight about…"), but only on those up to where
// the reader is, never one past it, and only where the privacy rule of
// handwriting and speech lets them go (RecognitionRoute): what one wrote and
// marked goes to one's own servers (this computer, the tailnet, an address
// marked as one's own); to someone else's computer (a hosted server, Claude
// Code, the Anthropic API) only when hosted servers are allowed in the
// settings, the question asked being the asking for it.
namespace AnnotationContext {

// Why the reader's annotations may not go to the AI the settings choose; empty when they may.
[[nodiscard]] QString whyNot(const Settings &settings);

// Whether the whole of an annotation's passage, or a bookmark's place, is at or before `upTo`.
[[nodiscard]] bool reached(const Annotation &annotation, const BookPlace &upTo);

// The highlights, margin notes and bookmarks up to `upTo`, a line each in
// the order of the book with where it is; a highlight's colour by the name
// the reader gave it (`colorNames`, Settings::highlightNames). Lost ones and
// ink, which have no place or no words, are left out; of very many, those
// nearest the reader's place. Empty when there are none.
[[nodiscard]] QString lines(const QList<Annotation> &annotations, const BookPlace &upTo, const QVariantMap &colorNames = {});

// Where a question about a book up to and including a section (all of it
// when negative) stops for its annotations: the earlier of the end of that
// section and the reader's place in the book.
[[nodiscard]] BookPlace cutFor(const Library &library, qint64 bookId, int upToSection = -1);

// A book's annotations up to `upTo`, as the companion and ask-the-book are
// given them; empty when there are none or the privacy rule keeps them back.
[[nodiscard]] QString forQuestion(const Library &library, qint64 bookId, const BookPlace &upTo, const Settings &settings);

} // namespace AnnotationContext
