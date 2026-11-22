#include "librarydiff.h"

#include <QHash>
#include <QJsonArray>
#include <QLocale>
#include <QSet>
#include <algorithm>
#include <ranges>
#include <utility>

#include "bookink.h"
#include "changeloggraph.h"
#include "library.h"
#include "spaces.h"

namespace {

using namespace Qt::StringLiterals;
using LibraryDiff::Counts;
using LibraryDiff::Field;

[[nodiscard]] QString number(double value) { return value == 0 ? QString() : QString::number(value, 'g', QLocale::FloatingPointShortest); }

// An identifier the person would call one: not bookkeeping, as a site's last
// sync ("goodreads:synced") or the plugin that made a book ("generated") is.
[[nodiscard]] bool personal(const QString &scheme) { return !scheme.contains(u':') && scheme != "generated"_L1; }

// A book's details, as the diff names them, in the order it tells them.
[[nodiscard]] QList<std::pair<QString, QString>> detailsOf(const Book &book) {
    const BookMeta &meta = book.meta;
    QList<std::pair<QString, QString>> details{{u"title"_s, meta.title},
                                               {u"authors"_s, meta.authors.join(u" & "_s)},
                                               {u"series"_s, meta.series},
                                               {u"series_index"_s, number(meta.seriesIndex)},
                                               {u"publisher"_s, meta.publisher},
                                               {u"language"_s, meta.language},
                                               {u"isbn"_s, meta.isbn},
                                               {u"published"_s, meta.published},
                                               {u"description"_s, meta.description},
                                               {u"tags"_s, meta.tags.join(u", "_s)}};
    QStringList schemes = book.identifiers.keys();
    schemes.sort();
    for (const QString &scheme : schemes)
        if (personal(scheme))
            details.append({u"identifiers."_s + scheme, book.identifiers.value(scheme).toVariant().toString()});
    return details;
}

using Details = QList<std::pair<QString, QString>>;

[[nodiscard]] QString valueIn(const Details &details, const QString &name) {
    const auto found = std::ranges::find(details, name, &std::pair<QString, QString>::first);
    return found == details.end() ? QString() : found->second;
}

[[nodiscard]] QList<Field> fieldsBetween(const Book &before, const Book &after) {
    const Details was = detailsOf(before);
    const Details is = detailsOf(after);
    // The fixed details as detailsOf tells them, then every identifier either has, by scheme.
    QStringList names;
    QStringList identifiers;
    for (const auto &[name, value] : was + is) {
        QStringList &into = name.startsWith("identifiers."_L1) ? identifiers : names;
        if (!into.contains(name))
            into << name;
    }
    identifiers.sort();
    QList<Field> fields;
    for (const QString &name : names + identifiers) {
        const QString then = valueIn(was, name);
        const QString now = valueIn(is, name);
        if (then != now)
            fields.append({.field = name, .before = then, .after = now});
    }
    return fields;
}

// An annotation as the diff compares it: its kind, and what the person sees
// of it (where it is, its words, note and colour, and an ink page's strokes).
struct Note {
    QString kind, look;
};
using Notes = QHash<QString, Note>; // by the annotation's global id

[[nodiscard]] Notes notesOf(const Library &library, qint64 bookId) {
    Notes notes;
    QHash<qint64, QStringList> strokes;
    for (const BookInk::Page &page : BookInk::pages(library, bookId)) {
        QStringList &ids = strokes[page.id];
        for (const Ink::Stroke &stroke : page.strokes)
            ids << stroke.id;
        ids.sort();
    }
    for (const Annotation &annotation : library.annotations(bookId)) {
        const QString look = QStringList{QString::number(annotation.place.section),
                                         number(annotation.place.fraction),
                                         QString::number(annotation.to.section),
                                         number(annotation.to.fraction),
                                         annotation.text,
                                         annotation.note,
                                         annotation.color,
                                         strokes.value(annotation.id).join(u',')}
                                 .join(QChar(0x1f));
        notes.insert(Spaces::annotationId(library, annotation.id), {.kind = annotation.kind, .look = look});
    }
    return notes;
}

[[nodiscard]] QMap<QString, Counts> compare(const Notes &before, const Notes &after) {
    QMap<QString, Counts> counts;
    for (const auto &[id, note] : before.asKeyValueRange()) {
        const auto now = after.constFind(id);
        if (now == after.cend())
            ++counts[note.kind].removed;
        else if (now->look != note.look)
            ++counts[note.kind].changed;
    }
    for (const auto &[id, note] : after.asKeyValueRange())
        if (!before.contains(id))
            ++counts[note.kind].added;
    return counts;
}

void add(QMap<QString, Counts> &into, const QMap<QString, Counts> &counts) {
    for (const auto &[kind, count] : counts.asKeyValueRange())
        into[kind] += count;
}

// A library's books by their global ids.
[[nodiscard]] QHash<QString, Book> booksOf(const Library &library) {
    QHash<QString, Book> books;
    for (const Book &book : library.books())
        books.insert(Spaces::bookId(library, book.id), book);
    return books;
}

[[nodiscard]] LibraryDiff::BookChange changeOf(const QString &id, const Book &book) {
    return {.id = id, .title = book.meta.title, .authors = book.meta.authors, .details = {}, .notes = {}};
}

[[nodiscard]] Counts compare(const QSet<QString> &before, const QSet<QString> &after) {
    Counts counts;
    counts.removed = static_cast<int>(std::ranges::count_if(before, [&after](const QString &id) { return !after.contains(id); }));
    counts.added = static_cast<int>(std::ranges::count_if(after, [&before](const QString &id) { return !before.contains(id); }));
    return counts;
}

[[nodiscard]] QJsonObject countsJson(const QMap<QString, Counts> &counts) {
    QJsonObject json;
    for (const auto &[kind, count] : counts.asKeyValueRange())
        json.insert(kind, count.toJson());
    return json;
}

[[nodiscard]] QJsonArray booksJson(const QList<LibraryDiff::BookChange> &books) {
    QJsonArray array;
    for (const LibraryDiff::BookChange &book : books)
        array.append(book.toJson());
    return array;
}

void sortByTitle(QList<LibraryDiff::BookChange> &books) {
    std::ranges::sort(books, [](const auto &a, const auto &b) { return QString::localeAwareCompare(a.title, b.title) < 0; });
}

} // namespace

LibraryDiff::Counts &LibraryDiff::Counts::operator+=(const Counts &other) {
    added += other.added;
    removed += other.removed;
    changed += other.changed;
    return *this;
}

QJsonObject LibraryDiff::Counts::toJson() const { return {{u"added"_s, added}, {u"removed"_s, removed}, {u"changed"_s, changed}}; }

bool LibraryDiff::BookChange::empty() const {
    return details.isEmpty() && std::ranges::all_of(notes, [](const Counts &counts) { return counts.empty(); });
}

QJsonObject LibraryDiff::BookChange::toJson() const {
    QJsonArray fields;
    for (const Field &field : details)
        fields.append(QJsonObject{{u"field"_s, field.field}, {u"before"_s, field.before}, {u"after"_s, field.after}});
    return {{u"id"_s, id},
            {u"title"_s, title},
            {u"authors"_s, QJsonArray::fromStringList(authors)},
            {u"details"_s, fields},
            {u"annotations"_s, countsJson(notes)}};
}

bool LibraryDiff::Diff::empty() const {
    return added.isEmpty() && removed.isEmpty() && changed.isEmpty() && things.empty() && relations.empty() && facts.empty();
}

QJsonObject LibraryDiff::Diff::toJson() const {
    return {
        {u"books"_s, QJsonObject{{u"added"_s, booksJson(added)}, {u"removed"_s, booksJson(removed)}, {u"changed"_s, booksJson(changed)}}},
        {u"annotations"_s, countsJson(notes)},
        {u"graph"_s, QJsonObject{{u"things"_s, things.toJson()}, {u"relations"_s, relations.toJson()}, {u"facts"_s, facts.toJson()}}}};
}

LibraryDiff::Diff LibraryDiff::between(const Library &before, const Library &after) {
    Diff diff;
    const QHash<QString, Book> was = booksOf(before);
    const QHash<QString, Book> is = booksOf(after);
    for (const auto &[id, book] : was.asKeyValueRange()) {
        const auto now = is.constFind(id);
        if (now == is.cend()) {
            BookChange gone = changeOf(id, book);
            gone.notes = compare(notesOf(before, book.id), {});
            add(diff.notes, gone.notes);
            diff.removed.append(gone);
            continue;
        }
        BookChange change = changeOf(id, *now);
        change.details = fieldsBetween(book, *now);
        change.notes = compare(notesOf(before, book.id), notesOf(after, now->id));
        add(diff.notes, change.notes);
        if (!change.empty())
            diff.changed.append(change);
    }
    for (const auto &[id, book] : is.asKeyValueRange()) {
        if (was.contains(id))
            continue;
        BookChange come = changeOf(id, book);
        come.notes = compare({}, notesOf(after, book.id));
        add(diff.notes, come.notes);
        diff.added.append(come);
    }
    for (QList<BookChange> *books : {&diff.added, &diff.removed, &diff.changed})
        sortByTitle(*books);
    const ChangeLogGraph::GlobalIds graphBefore = ChangeLogGraph::globalIds(before);
    const ChangeLogGraph::GlobalIds graphAfter = ChangeLogGraph::globalIds(after);
    diff.things = compare(graphBefore.things, graphAfter.things);
    diff.relations = compare(graphBefore.relations, graphAfter.relations);
    diff.facts = compare(graphBefore.facts, graphAfter.facts);
    return diff;
}

std::optional<LibraryDiff::BookChange> LibraryDiff::book(const Library &before, const Library &after, const QString &globalId) {
    const qint64 was = Spaces::localId(before, u"book"_s, globalId);
    const qint64 is = Spaces::localId(after, u"book"_s, globalId);
    const std::optional<Book> then = was > 0 ? before.book(was) : std::nullopt;
    const std::optional<Book> now = is > 0 ? after.book(is) : std::nullopt;
    if (!then && !now)
        return std::nullopt;
    BookChange change = changeOf(globalId, now ? *now : *then);
    if (then && now)
        change.details = fieldsBetween(*then, *now);
    change.notes = compare(then ? notesOf(before, then->id) : Notes{}, now ? notesOf(after, now->id) : Notes{});
    return change;
}
