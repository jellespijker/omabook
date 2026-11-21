#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <optional>

#include "library.h"

// What says two books on two computers are one book (#158, ADR 0009).
//
// A book's identity is its global id (Spaces::bookId): once two copies are
// paired, by the same id or a same-as link the log follows (BookLinks), they
// stay paired and nothing else pairs them. An identifier only pairs a book at
// its first meeting with one never synced before (BookPairing), and only an
// identifier of a strong scheme: a valid ISBN, a Goodreads id, a Calibre uuid.
// Calibre's custom columns ("genre", "uri", "set_id"), a site's bookkeeping
// ("goodreads:synced") and every other scheme name no book: many books share
// them.
namespace BookIdentity {

// The keys of the strong schemes a book has, as entry keys are written
// ("entry:isbn=9780140424386", "entry:goodreads=1", "entry:calibre=<uuid>"):
// an ISBN-10 or -13 whose check digit holds, as its ISBN-13, from its ISBN or
// its "isbn" identifier; the number of a Goodreads id; a Calibre id that is a
// uuid (one that is Calibre's number for a book in one library names none).
[[nodiscard]] QStringList strongKeys(const QJsonObject &identifiers, const QString &isbn);
[[nodiscard]] QStringList strongKeys(const Book &book);

// What a global id is made of when a book without a file is given one
// (Spaces::ensureBookId): its strong keys, then its title and authors.
[[nodiscard]] QStringList idKeys(const Book &book);

// An ISBN-10 or ISBN-13 as its ISBN-13, when its check digit holds.
[[nodiscard]] std::optional<QString> isbn13(const QString &text);

// Whether two books' titles agree, once case and punctuation are set aside,
// and a subtitle (after a colon, a dash, or in brackets at the end) one of
// them has and the other lacks: "The Lady of Crows" and "The Lady of Crows:
// A Novel" agree; "Dark Ages: Ravnos" and "Dark Ages: Tremere" don't.
[[nodiscard]] bool titlesAgree(const QString &one, const QString &other);

// Whether the same global id on two computers is the same book: the content
// of a file ("b:"), an id made for the book alone ("u:"), or a key of a strong
// scheme or of its title and authors ("e:"). One made of another identifier,
// as an Omabook before 0.2.2 made one of a book's first identifier
// ("e:entry:genre=scifan"), names whichever books share it.
[[nodiscard]] bool strongId(const QString &gid);

} // namespace BookIdentity
