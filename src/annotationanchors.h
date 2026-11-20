#pragma once

#include <QList>
#include <QString>
#include <expected>

#include "library.h"

class BookText;

// A book's highlights and margin notes kept on their passages (#80): each
// passage's places are those of one file of the book (`Annotation::anchored`,
// its hash). When the book is read from another file, a new edition, a typo
// fixed, another format, each is found again in it by its words
// (Passage::findAgain) and its places, words and confidence written anew; one
// that can't be found becomes a lost annotation, kept for the reader to place
// again or remove. Bookmarks are places, not passages, and stay where they are.
namespace AnnotationAnchors {

// What finding a book's passages again in a file did.
struct Summary {
    int kept = 0;  // found where they were
    int moved = 0; // found, at another place or with other words
    int lost = 0;  // not found
    bool operator==(const Summary &) const = default;
};

// Whether any of a book's passages are not yet placed in the file with this hash.
[[nodiscard]] bool needed(const Library &library, qint64 bookId, const QString &fileHash);
// Each of a book's passages not yet placed in that file found again in its
// text, in one write. A lost one is sought again too, when the file is new to it.
Summary anchor(Library &library, qint64 bookId, const BookText &text, const QString &fileHash);
// The same with the file the reader reads the book from (readableFile).
[[nodiscard]] std::expected<Summary, QString> anchor(Library &library, qint64 bookId);

// The book's file the reader opens: the first of its formats that Omabook reads.
[[nodiscard]] std::optional<FormatFile> readableFile(const Library &library, qint64 bookId);

// A book's lost annotations, in the order they were in the book.
[[nodiscard]] QList<Annotation> lost(const Library &library, qint64 bookId);
// An annotation placed on a passage of that file, where the reader selected
// it: its words and places are the passage's, it is no longer lost, and it
// is as sure as can be. Whether there is one of that number.
bool placeAgain(Library &library, qint64 annotationId, const Passage &passage, const QString &fileHash);

} // namespace AnnotationAnchors
