#include "bookmatch.h"

#include <QHash>
#include <QStringList>
#include <algorithm>

#include "curation.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

// The entry keys that name a book by an identifier or its ISBN: of
// Library::entryKeys all but the one made of its title and authors, which
// the tidy's rules decide more carefully.
[[nodiscard]] QStringList identifierKeys(const QJsonObject &identifiers, const BookMeta &meta) {
    QStringList keys = Library::entryKeys(identifiers, meta.isbn, meta.title, meta.authors);
    keys.removeIf([](const QString &key) { return key.startsWith("entry:title="_L1); });
    return keys;
}

void bySameFile(const Library &library, const QList<BookMatch::Known> &known, QList<BookMatch::Found> &found) {
    for (qsizetype at = 0; at < known.size(); ++at)
        if (const qint64 mine = Spaces::localId(library, u"book"_s, known.at(at).gid); mine > 0 && library.book(mine))
            found[at] = {.book = mine, .how = BookMatch::How::File};
}

void byIdentifier(const QList<Book> &mine, const QList<BookMatch::Known> &known, QList<BookMatch::Found> &found) {
    QHash<QString, qint64> byKey;
    for (const Book &book : mine)
        for (const QString &key : identifierKeys(book.identifiers, book.meta))
            byKey.insert(key, byKey.value(key, book.id));
    for (qsizetype at = 0; at < known.size(); ++at) {
        if (found.at(at).how != BookMatch::How::None)
            continue;
        for (const QString &key : identifierKeys(known.at(at).identifiers, known.at(at).meta))
            if (const qint64 book = byKey.value(key); book > 0) {
                found[at] = {.book = book, .how = BookMatch::How::Identifier};
                break;
            }
    }
}

// The tidy's duplicates among the library's books and the known ones, each
// known one standing in under a number no book here has (below zero).
void byTitleAndAuthors(const QList<Book> &mine, const QList<BookMatch::Known> &known, QList<BookMatch::Found> &found) {
    QList<Book> together = mine;
    for (qsizetype at = 0; at < known.size(); ++at)
        if (found.at(at).how == BookMatch::How::None)
            together << Book{.id = -(at + 1), .meta = known.at(at).meta, .identifiers = known.at(at).identifiers};
    for (const Proposal &proposal : Curation::duplicates(together)) {
        const qint64 one = proposal.bookId;
        const qint64 other = proposal.otherId;
        if (proposal.kind != "duplicate"_L1 || proposal.unsure || (one < 0) == (other < 0))
            continue;
        const qsizetype at = -std::min(one, other) - 1;
        if (found.at(at).how == BookMatch::How::None)
            found[at] = {.book = std::max(one, other), .how = BookMatch::How::TitleAndAuthors};
    }
}

} // namespace

QString BookMatch::describe(How how) {
    switch (how) {
    case How::File:
        return u"the same file"_s;
    case How::Identifier:
        return u"an identifier"_s;
    case How::TitleAndAuthors:
        return u"title and authors"_s;
    case How::None:
        break;
    }
    return {};
}

QList<BookMatch::Found> BookMatch::find(const Library &library, const QList<Known> &known) {
    QList<Found> found(known.size());
    bySameFile(library, known, found);
    const QList<Book> mine = library.books();
    byIdentifier(mine, known, found);
    byTitleAndAuthors(mine, known, found);
    return found;
}
