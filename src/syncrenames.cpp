#include "syncrenames.h"

#include <QStringList>
#include <tuple>
#include <utility>

#include "synccopies.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] auto identityOf(const BookMeta &meta) { return std::tie(meta.title, meta.authors, meta.isbn); }

void setIdentity(BookMeta &to, const BookMeta &from) {
    to.title = from.title;
    to.authors = from.authors;
    to.isbn = from.isbn;
}

[[nodiscard]] QString orNone(const QString &text) { return text.isEmpty() ? u"none"_s : text; }

// "The Lady of Crows: title → The Lady of Crows: A Novel; ISBN none → 9780…".
[[nodiscard]] QString described(const BookMeta &from, const BookMeta &to) {
    QStringList changes;
    if (from.title != to.title)
        changes << u"title → "_s + to.title;
    if (from.authors != to.authors)
        changes << u"authors "_s + orNone(from.authorText()) + u" → "_s + orNone(to.authorText());
    if (from.isbn != to.isbn)
        changes << u"ISBN "_s + orNone(from.isbn) + u" → "_s + orNone(to.isbn);
    return from.title + u": "_s + changes.join(u"; "_s);
}

} // namespace

void SyncRenames::hold(const Book &here, const QJsonObject &theirs, Consent consent, SyncManifest::MergedBookDetails &merged,
                       SyncSummary &summary) {
    BookMeta wanted = merged.meta;
    if (consent.details == SyncOptions::Details::Theirs)
        setIdentity(wanted, BookMeta::fromJson(theirs));
    else if (consent.details == SyncOptions::Details::Mine)
        setIdentity(wanted, here.meta);
    if (identityOf(wanted) != identityOf(here.meta)) {
        ++summary.renamedHere;
        summary.titles.renamedHere << described(here.meta, wanted);
        // A book whose rename waits takes none of its details until then: those
        // taken would be stamped with their time, and its title, as old as it
        // is, would seem as new as theirs (SpaceReducer::fieldHlc).
        if (!consent.accept && consent.details == SyncOptions::Details::Undecided) {
            ++summary.renamesHeld;
            merged.meta = here.meta;
            merged.takeFields = false;
            merged.takeCover = false;
            return;
        }
    }
    const bool renamed = identityOf(wanted) != identityOf(here.meta);
    merged.meta = std::move(wanted);
    merged.takeFields = (merged.takeFields || renamed) && (merged.takeCover || !SyncCopies::sameWords(merged.meta, here.meta));
}
