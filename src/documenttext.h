#pragma once

#include <QString>
#include <QVariantList>

class Document;

// What the reader asks of an open book's text.
namespace DocumentText {

// Words found in it (the reader's find): where each is, by its section,
// how far into it and the chapter it is in, with the words around it, as
// {section, title, fraction, snippet}; at most 200, in the order of the
// book. Nothing for fewer than two letters.
[[nodiscard]] QVariantList find(const Document &document, const QString &words);

// Its text from one place to another (`to`, where the reader is). A
// flowing book's place is partway down a chapter; a paged one's is its
// page, which has been read once the reader is on it, and the first page
// is left out unless `withFirstPage`.
struct Span {
    int fromSection = 0;
    double fromFraction = 0;
    int toSection = 0;
    double toFraction = 0;
};
[[nodiscard]] QString between(const Document &document, const Span &span, bool withFirstPage);

} // namespace DocumentText
