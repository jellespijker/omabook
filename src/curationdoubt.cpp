// What folding a duplicate would not keep, said before anyone applies it (#282).

#include <QHash>
#include <algorithm>
#include <utility>

#include "curation.h"
#include "librarymerge.h"

using namespace Qt::StringLiterals;

QList<Proposal> Curation::withDropped(QList<Proposal> proposals, const QList<Book> &books) {
    const auto numbered = [&books](qint64 id) { return std::ranges::find(books, id, &Book::id); };
    QHash<qint64, Book> kept; // each kept book as the folds before leave it
    for (Proposal &proposal : proposals) {
        const auto keep = numbered(proposal.bookId);
        const auto other = numbered(proposal.otherId);
        if (proposal.kind != "duplicate"_L1 || keep == books.end() || other == books.end())
            continue;
        if (!kept.contains(proposal.bookId))
            kept.insert(proposal.bookId, *keep);
        Book &now = kept[proposal.bookId];
        proposal.dropped = LibraryMerge::dropped(now, *other);
        now = LibraryMerge::after(now, *other);
    }
    return proposals;
}

QString Curation::doubt(const Proposal &proposal) {
    QStringList said;
    if (!proposal.why.isEmpty())
        said << proposal.why;
    if (!proposal.dropped.isEmpty())
        said << u"Folding it in would not keep book %1's %2, which differ from book %3's."_s.arg(proposal.otherId)
                    .arg(proposal.dropped.join(u", "_s))
                    .arg(proposal.bookId);
    return said.join(u' ');
}
