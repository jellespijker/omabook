#pragma once

#include <QString>
#include <QVariantMap>
#include <QtTypes>
#include <expected>
#include <optional>

#include "library.h"

// Annotations added, changed and removed by what a person or an agent types
// rather than selects (#86): the command line's `omabook annotations` and the
// MCP tools it serves. A highlight or a margin note is put on words given,
// found in the book's own text near the place given, as a passage is found
// again (ADR 0004), so it is on the same words as one made in the reader; a
// bookmark is a place. Each is kept and travels as one made in the window.
namespace AnnotationEdits {

inline constexpr auto marginNote = "margin-note";

// What a new annotation is to be.
struct Wanted {
    QString kind;    // "highlight", "margin-note" or "bookmark"
    BookPlace place; // where its words are, or the bookmark is
    QString words;   // the words of its passage; a bookmark's name
    QString color;   // a highlight's, by its name or the reader's for it; the first when empty
    QString note;    // a margin note's words; a note kept with a highlight or a bookmark
};

// A highlight's colour by its own name ("blue") or the one the reader gave
// it ("Questions", `names` as Settings::highlightNames keeps them): the
// colour's own name, or why there is none of that name.
[[nodiscard]] std::expected<QString, QString> colorNamed(const QString &asked, const QVariantMap &names);

// One of a book's annotations, by its number.
[[nodiscard]] std::optional<Annotation> find(const Library &library, qint64 bookId, qint64 id);

// A new annotation on a book: its number, or why not (no such words in the
// book, no words or note, a colour there isn't). The words of a book that has
// no file Omabook reads are placed the first time it is opened.
[[nodiscard]] std::expected<qint64, QString> add(Library &library, qint64 bookId, const Wanted &wanted, const QVariantMap &names = {});

// What may change of one: its note (a margin note's words), a highlight's
// colour, a bookmark's name. A passage's words are the book's and stay.
struct Change {
    std::optional<QString> note;
    std::optional<QString> color;
    std::optional<QString> name;
};
// One of a book's annotations changed: as it is now, or why not.
[[nodiscard]] std::expected<Annotation, QString> change(Library &library, qint64 bookId, qint64 id, const Change &change,
                                                        const QVariantMap &names = {});

// One of a book's annotations removed, or why not (it has none of that number).
[[nodiscard]] std::expected<void, QString> remove(Library &library, qint64 bookId, qint64 id);

} // namespace AnnotationEdits
