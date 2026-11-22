#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <optional>

class Library;

// What changed in a library between two moments, told as the person sees the
// library (#183, the glossary's Library diff): books added or removed, a
// book's details before and after, how many of its notes, highlights and ink
// pages came, went or changed, and how many things, relations and facts of
// the knowledge graph did. Never a list of files. Books, annotations and the
// graph's rows are matched by their global ids (ADR 0009), so a book renamed
// is a book changed, not one removed and another added.
//
// The two sides are libraries: a snapshot's database, opened in a folder of
// its own (SnapshotCopy), and another snapshot's or the live library. Only
// read, never written, but for a global id a row was still missing (Spaces).
namespace LibraryDiff {

// How many came, went, or are there on both sides but differ.
struct Counts {
    int added = 0, removed = 0, changed = 0;

    [[nodiscard]] bool empty() const { return added == 0 && removed == 0 && changed == 0; }
    Counts &operator+=(const Counts &other);
    [[nodiscard]] QJsonObject toJson() const;
};

// One of a book's details before and after: "title", "authors" (joined by
// " & "), "series", "series_index", "publisher", "language", "isbn",
// "published", "description", "tags" (joined by ", ") or "identifiers.<scheme>".
// Empty for a detail it didn't have.
struct Field {
    QString field, before, after;
};

struct BookChange {
    QString id; // its global id
    QString title;
    QStringList authors;
    QList<Field> details;        // for a changed book, the details that differ
    QMap<QString, Counts> notes; // its annotations, by kind ("highlight", "bookmark", "margin-note", "ink")

    [[nodiscard]] bool empty() const;
    [[nodiscard]] QJsonObject toJson() const;
};

struct Diff {
    QList<BookChange> added, removed, changed;
    QMap<QString, Counts> notes;     // every book's annotations, by kind, those of books added and removed too
    Counts things, relations, facts; // the knowledge graph's nodes, edges and facts

    [[nodiscard]] bool empty() const;
    // {"books": {"added": [...], "removed": [...], "changed": [...]}, "annotations": {kind: counts},
    //  "graph": {"things": counts, "relations": counts, "facts": counts}}, each book {"id", "title",
    // "authors", "details": [{"field", "before", "after"}], "annotations": {kind: counts}}.
    [[nodiscard]] QJsonObject toJson() const;
};

// What changed from `before` to `after`.
[[nodiscard]] Diff between(const Library &before, const Library &after);

// What changed of the one book of that global id: none when neither side has it.
[[nodiscard]] std::optional<BookChange> book(const Library &before, const Library &after, const QString &globalId);

} // namespace LibraryDiff
