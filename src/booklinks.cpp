#include "booklinks.h"

#include <QHash>
#include <algorithm>
#include <optional>
#include <utility>

#include "bookidentity.h"
#include "changelog.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool isBookLink(const SpaceOperation &op) { return op.entity == "book"_L1 && op.field == "same_as"_L1; }

[[nodiscard]] bool isApart(const SpaceOperation &op) { return op.entity == "book"_L1 && op.field == "not_same_as"_L1; }

[[nodiscard]] bool ofFiles(const SpaceOperation &link) { return link.id.startsWith("b:"_L1) && link.value.startsWith("b:"_L1); }

[[nodiscard]] bool holds(const BookLinks::Titles &titles, const QSet<QString> &apart, const SpaceOperation &link) {
    if (ofFiles(link))
        return true;
    if (apart.contains(BookLinks::pairOf(link.id, link.value)))
        return false;
    const std::optional<QString> one = BookLinks::titleAt(titles, link.id, link.hlc);
    const std::optional<QString> other = BookLinks::titleAt(titles, link.value, link.hlc);
    return !one || !other || BookIdentity::titlesAgree(*one, *other);
}

// The title each id has now: as this library has it, for a book here, else
// as the log says it last. What is here is what the person sees and names;
// the log may still say the title an older Omabook gave a book after it was
// put right here (#166).
[[nodiscard]] QHash<QString, QString> titlesNow(const Library &library, const BookLinks::Titles &titles) {
    QHash<QString, QString> now;
    for (auto it = titles.cbegin(); it != titles.cend(); ++it)
        now.insert(it.key(), std::ranges::max(*it, {}, &std::pair<qint64, QString>::first).second);
    for (const Book &book : library.books())
        if (!book.meta.title.trimmed().isEmpty())
            now.insert(Spaces::bookId(library, book.id), book.meta.title);
    return now;
}

void writeApart(const Library &library, const SpaceOperation &link) {
    ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                .entity = u"book"_s,
                                .id = std::max(link.id, link.value),
                                .field = u"not_same_as"_s,
                                .value = std::min(link.id, link.value),
                                .hlc = std::nullopt});
}

[[nodiscard]] QString pairTitles(const QString &one, const QString &other) {
    return std::min(one, other) + u" / "_s + std::max(one, other);
}

} // namespace

BookLinks::Titles BookLinks::titlesOf(std::span<const SpaceOperation> ops) {
    Titles titles;
    for (const SpaceOperation &op : ops)
        if (op.entity == "book"_L1 && op.field == "title"_L1 && !op.value.trimmed().isEmpty())
            titles[op.id] << std::pair{op.hlc, op.value};
    return titles;
}

std::optional<QString> BookLinks::titleAt(const Titles &titles, const QString &id, qint64 at) {
    const auto found = titles.constFind(id);
    if (found == titles.cend())
        return std::nullopt;
    std::optional<std::pair<qint64, QString>> before;
    std::optional<std::pair<qint64, QString>> after;
    for (const auto &[hlc, title] : *found) {
        if (hlc <= at && (!before || hlc >= before->first))
            before = std::pair{hlc, title};
        else if (hlc > at && (!after || hlc < after->first))
            after = std::pair{hlc, title};
    }
    const auto chosen = before ? before : after;
    return chosen ? std::optional(chosen->second) : std::nullopt;
}

QString BookLinks::pairOf(const QString &one, const QString &other) { return std::min(one, other) + u'|' + std::max(one, other); }

QSet<QString> BookLinks::apart(std::span<const SpaceOperation> ops) {
    QSet<QString> pairs;
    for (const SpaceOperation &op : ops)
        if (isApart(op) && !op.value.isEmpty())
            pairs.insert(pairOf(op.id, op.value));
    return pairs;
}

QList<SpaceOperation> BookLinks::trusted(std::span<const SpaceOperation> ops) {
    const Titles titles = titlesOf(ops);
    const QSet<QString> pairs = apart(ops);
    QList<SpaceOperation> kept;
    kept.reserve(static_cast<qsizetype>(ops.size()));
    std::ranges::copy_if(ops, std::back_inserter(kept),
                         [&](const SpaceOperation &op) { return !isBookLink(op) || holds(titles, pairs, op); });
    return kept;
}

QSet<QString> BookLinks::weaklyPaired(std::span<const SpaceOperation> trustedOps) {
    const QHash<QString, QString> canonical = SpaceReducer::sameAs(trustedOps);
    QSet<QString> weak;
    for (const SpaceOperation &op : trustedOps)
        if (isBookLink(op) && !ofFiles(op))
            weak.insert(canonical.value(op.id, op.id));
    return weak;
}

QStringList BookLinks::record(const Library &library, std::span<const SpaceOperation> ops) {
    const Titles titles = titlesOf(ops);
    QSet<QString> pairs = apart(ops);
    QStringList written;
    const Library::Batch batch(library);
    for (const SpaceOperation &link : ops) {
        if (!isBookLink(link) || pairs.contains(pairOf(link.id, link.value)) || holds(titles, pairs, link))
            continue;
        pairs.insert(pairOf(link.id, link.value));
        writeApart(library, link);
        written << pairTitles(titleAt(titles, link.id, link.hlc).value_or(link.id),
                              titleAt(titles, link.value, link.hlc).value_or(link.value));
    }
    return written;
}

QStringList BookLinks::unlink(const Library &library, std::span<const SpaceOperation> ops, const QString &title, bool dryRun) {
    const QHash<QString, QString> now = titlesNow(library, titlesOf(ops));
    const auto named = [&](const QString &id) { return now.value(id).trimmed().compare(title.trimmed(), Qt::CaseInsensitive) == 0; };
    QStringList unlinked;
    const Library::Batch batch(library);
    for (const SpaceOperation &link : trusted(ops)) {
        const QString one = now.value(link.id);
        const QString other = now.value(link.value);
        if (!isBookLink(link) || ofFiles(link) || !(named(link.id) || named(link.value)) || one.isEmpty() || other.isEmpty()
            || BookIdentity::titlesAgree(one, other))
            continue;
        if (!dryRun)
            writeApart(library, link);
        unlinked << pairTitles(one, other);
    }
    unlinked.removeDuplicates();
    return unlinked;
}
