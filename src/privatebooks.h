#pragma once

#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QtTypes>
#include <expected>

#include "failure.h"

class Library;
struct Book;
struct Collection;

// Books a plugin keeps private (#127): a plugin says in its plugin.json that
// its kind of book, a file format of its own ("private": ["omanotebook"]),
// is the person's alone, as a notebook is. Such a book stays in the person's
// own collections and travels between their own computers like any other,
// but nothing of it (its details, its file, what is known or written of it)
// goes into a shared or a published space, unless the person chose that for
// that book. The choice is undone the same way, and a space then takes back
// what it held of the book, as a part switched off is taken back.
//
// Both are said in the personal space's change log (entity `private`): a
// kind by `kind:<format>`, field `plugin`, with the plugin's id and name; a
// book's choice by the book's global id, field `shared`, "1" or "0", the
// latest winning. So they go to one's other computers, and a kind once said
// stays private after its plugin is removed, or where it was never installed.
namespace PrivateBooks {

// A kind of book a plugin keeps private.
struct Kind {
    QString format; // the extension, lower case, without the dot
    QString plugin; // the plugin's id, its folder's name
    QString title;  // what the plugin is called
};

// Every kind said to be private, here or on one's other computers.
[[nodiscard]] QList<Kind> kinds(const Library &library);

// Says that `kind.plugin` keeps its books of `kind.format` private; once said
// it stays so. Whether it was news. A format of Omabook's own (EPUB, PDF, …)
// is no plugin's to keep, and an extension that is no file's is no kind: both
// are passed over, as false.
[[nodiscard]] std::expected<bool, Failure> declare(const Library &library, const Kind &kind);

// Whether the person chose to let the book go into the spaces it is in.
[[nodiscard]] bool shared(const Library &library, qint64 book);
// Lets a book a plugin keeps private go into the shared and published
// spaces it is in, or (`shared` false) keeps it out again.
[[nodiscard]] std::expected<void, Failure> share(const Library &library, qint64 book, bool shared);

// A book of a kind a plugin keeps private, among those a collection holds.
struct Kept {
    qint64 book = 0;
    QString title;
    Kind kind;
    bool byRule = false; // the collection's rule takes it in, not a choice by hand
    bool shared = false; // the person let it go in all the same
};

// Of `books` (those `collection` holds), the ones of a kind a plugin keeps
// private, each with how the collection holds it and whether it was let go.
[[nodiscard]] QList<Kept> among(const Library &library, const QList<Book> &books, const Collection &collection);
// Of `books`, those that stay out of every space: of a private kind, not let go.
[[nodiscard]] QSet<qint64> withheld(const Library &library, const QList<Book> &books);

// [{"book", "title", "format", "plugin", "plugin_title", "by_rule", "shared"}].
[[nodiscard]] QJsonArray toJson(const QList<Kept> &kept);

} // namespace PrivateBooks
