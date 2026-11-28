#include "synctake.h"

#include <QSet>
#include <algorithm>
#include <optional>
#include <tuple>

#include "hlc.h"
#include "numberoptions.h"
#include "strayidentifiers.h"
#include "syncmanifest.h"
#include "syncplan.h"

using namespace Qt::StringLiterals;

// What a sync takes and gives of a book's reading and names elsewhere (SyncTake).

namespace {

// "generated" (which plugin made a book) never travels: only that plugin
// marks a book as its own, and the mark is what lets it fork and branch a
// story, which must never reach a book by a human author. A sync neither
// sends it nor takes it in.
bool travels(const QString &scheme) { return scheme.compare(QStringLiteral("generated"), Qt::CaseInsensitive) != 0; }

// The names elsewhere they have of a book and this side lacks, but the stray
// ones. Names that came by sync keep the details' time, so they are no reason
// to send the details back.
bool fillNames(Library &library, const Book &mine, const QJsonObject &identifiers, const QSet<QString> &stray, bool dryRun,
               SyncPlan *plan) {
    bool changed = false;
    for (auto it = identifiers.begin(); it != identifiers.end(); ++it)
        if (travels(it.key()) && !mine.identifiers.contains(it.key())
            && !StrayIdentifiers::isStray(stray, it.key(), it.value().toVariant().toString())) {
            changed = true;
            if (plan != nullptr)
                plan->identifier(library, mine, it.key(), it.value().toVariant().toString());
            if (!dryRun)
                library.setIdentifier(mine.id, it.key(), it.value().toVariant().toString(), SyncManifest::myDetailsHlc(library, mine));
        }
    return changed;
}

// What is said of a book's reading once what the other side says fills in
// where this side says nothing, and of each field `newer` there.
[[nodiscard]] Book readingFilled(const Book &mine, const QJsonObject &theirs, const QSet<QString> &newer) {
    const auto at = [&](const char *field, bool lacking) { return lacking || newer.contains(QLatin1StringView(field)); };
    return {.paper = mine.paper || theirs.value("paper").toBool(),
            .shelf = at("shelf", mine.shelf.isEmpty()) ? theirs.value("shelf").toString() : mine.shelf,
            .rating = at("rating", mine.rating == 0) ? theirs.value("rating").toInt() : mine.rating,
            .finished = at("finished", mine.finished.isEmpty()) ? theirs.value("finished").toString() : mine.finished};
}

// The time a reading filled in from what the other side says keeps: the later
// of theirs, as their log says it, and this one's, never the time it is taken
// at, which would put it after a newer change made elsewhere meanwhile once a
// computer or a hub passes it on (#243). None from an Omabook that doesn't
// say it: stamped now, as ever.
[[nodiscard]] std::optional<qint64> filledAt(const Book &mine, const QJsonObject &theirs) {
    const qint64 at = Hlc::normalize(theirs.value("reading_hlc").toInteger());
    if (at <= 0)
        return std::nullopt;
    return std::max(at, Hlc::normalize(mine.readingAt));
}

} // namespace

bool SyncTake::fillReading(Library &library, qint64 id, const QJsonObject &theirs, bool dryRun, const Names &names, SyncPlan *plan) {
    const std::optional<Book> mine = library.book(id);
    if (!mine)
        return false;
    const Book filled = readingFilled(*mine, theirs, names.newerReading);
    const bool reading = std::tie(filled.shelf, filled.rating, filled.finished) != std::tie(mine->shelf, mine->rating, mine->finished);
    const std::optional<qint64> at = filledAt(*mine, theirs);
    // One write, logged once whole: written apart, the paper copy would be logged
    // as this side's before the shelf was taken, or the other way round (#243).
    std::optional<Library::Batch> batch;
    if (!dryRun)
        batch.emplace(library);
    if (reading && !dryRun)
        library.setReading(id, filled.shelf, filled.rating, filled.finished, at);
    if (filled.paper != mine->paper && !dryRun)
        library.setPaper(id, true, at);
    const bool changed = reading || filled.paper != mine->paper;
    if (changed && plan != nullptr)
        plan->reading(library, *mine, filled);
    return (names.take && fillNames(library, *mine, theirs.value("identifiers").toObject(), names.stray, dryRun, plan)) || changed;
}

QStringList SyncTake::readingArguments(qint64 id, const Book &book, std::optional<qint64> detailsAt, bool names, bool readingAt) {
    QStringList arguments{"set",      QString::number(id),          "--status", book.shelf.isEmpty() ? QStringLiteral("none") : book.shelf,
                          "--rating", QString::number(book.rating), "--paper",  book.paper ? QStringLiteral("yes") : QStringLiteral("no")};
    if (!book.finished.isEmpty())
        arguments << "--finished" << book.finished;
    for (auto it = book.identifiers.begin(); it != book.identifiers.end() && names; ++it)
        if (const QString value = it.value().toVariant().toString(); !value.isEmpty() && travels(it.key()))
            arguments << "--identifier" << it.key() + "=" + value;
    if (detailsAt)
        arguments << "--details-at" << QString::number(*detailsAt);
    // A finished day from before only days were taken (#286) goes with its time, and so is taken as it is.
    if (readingAt && (book.readingAt > 0 || (!book.finished.isEmpty() && !Numbers::day(book.finished))))
        arguments << "--reading-at" << QString::number(book.readingAt);
    return arguments;
}
