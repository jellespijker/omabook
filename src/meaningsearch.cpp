#include "meaningsearch.h"

#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <memory>
#include <ranges>

#include "annotationwords.h"
#include "bookops.h"
#include "booktext.h"
#include "document.h"
#include "formats.h"
#include "graphcontext.h"
#include "library.h"
#include "settings.h"
#include "textstretches.h"

using namespace Qt::StringLiterals;

namespace {

// The file a book is read from, and its hash; nothing for a book without one.
struct Source {
    QString path;
    QString hash;
};
[[nodiscard]] std::optional<Source> sourceOf(const Library &library, qint64 bookId) {
    QString error;
    const QString path = BookOps::readableFile(library, bookId, &error);
    if (path.isEmpty())
        return std::nullopt;
    const QList<FormatFile> files = library.formatFiles(bookId);
    const auto file = std::ranges::find_if(
        files, [&path](const FormatFile &one) { return QFileInfo(one.path).canonicalFilePath() == QFileInfo(path).canonicalFilePath(); });
    return Source{.path = path, .hash = file != files.end() && !file->hash.isEmpty() ? file->hash : Library::hashFile(path)};
}

// What the reader may be shown of a book: all of it when asked, else no
// further than their furthest place (all of a book read, none of one not begun).
[[nodiscard]] std::function<bool(qint64, const BookPlace &)> spoilerCut(const Library &library, bool whole) {
    if (whole)
        return {};
    const auto reaches = std::make_shared<QHash<qint64, GraphContext::Reach>>();
    return [&library, reaches](qint64 bookId, const BookPlace &to) {
        auto reach = reaches->constFind(bookId);
        if (reach == reaches->cend())
            reach = reaches->insert(bookId, GraphContext::reachOf(library, bookId));
        return reach->all || (reach->any && to <= BookPlace{.section = reach->place.section, .fraction = reach->place.fraction});
    };
}

} // namespace

MeaningSearch::Freshness MeaningSearch::freshness(const Library &library, qint64 bookId, const QString &model) {
    const auto state = MeaningIndex::state(library, bookId);
    if (!state || !state->built())
        return Freshness::None;
    const auto source = sourceOf(library, bookId);
    return source && source->hash == state->hash && state->model == model ? Freshness::Fresh : Freshness::Stale;
}

std::expected<int, Failure> MeaningSearch::index(const Library &library, const Embeddings::Route &route, qint64 bookId,
                                                 const std::stop_token &stop, const std::function<void(Progress)> &told) {
    if (!library.book(bookId))
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(bookId)}));
    const auto source = sourceOf(library, bookId);
    if (!source)
        return std::unexpected(Failure(Failure::Code::MeaningNoText));
    if (auto asked = MeaningIndex::want(library, bookId, true); !asked)
        return std::unexpected(asked.error());
    auto document = Formats::opened(source->path);
    if (!document)
        return std::unexpected(document.error());
    const std::vector<TextStretches::Stretch> cut = TextStretches::of(BookText(**document));
    if (cut.empty())
        return std::unexpected(Failure(Failure::Code::MeaningNoText));
    std::vector<MeaningIndex::Stretch> stretches;
    stretches.reserve(cut.size());
    const int total = static_cast<int>(cut.size());
    for (int first = 0; first < total; first += batch) {
        if (stop.stop_requested())
            return std::unexpected(Failure(Failure::Code::MeaningCancelled));
        if (told)
            told({.done = first, .total = total});
        const auto these = std::span(cut).subspan(static_cast<size_t>(first), static_cast<size_t>(std::min(batch, total - first)));
        const QStringList texts = these | std::views::transform(&TextStretches::Stretch::text) | std::ranges::to<QStringList>();
        auto vectors = Embeddings::embed(route, texts, Embeddings::Use::Document);
        if (!vectors)
            return std::unexpected(vectors.error());
        for (size_t at = 0; at < these.size(); ++at)
            stretches.push_back({.from = these[at].from, .to = these[at].to, .text = these[at].text, .vector = std::move((*vectors)[at])});
    }
    if (stop.stop_requested())
        return std::unexpected(Failure(Failure::Code::MeaningCancelled));
    if (auto kept = MeaningIndex::store(library, bookId, source->hash, route.model, stretches); !kept)
        return std::unexpected(kept.error());
    if (told)
        told({.done = total, .total = total});
    return total;
}

QList<qint64> MeaningSearch::due(const Library &library, const Settings &settings, const QString &model) {
    QList<qint64> asked = MeaningIndex::wanted(library);
    if (settings.meaningAll())
        for (const Book &book : library.books())
            if (!book.formats.isEmpty() && !asked.contains(book.id))
                asked << book.id;
    std::ranges::sort(asked);
    return asked | std::views::filter([&](qint64 bookId) { return freshness(library, bookId, model) != Freshness::Fresh; })
           | std::ranges::to<QList<qint64>>();
}

std::expected<QList<MeaningIndex::Hit>, Failure> MeaningSearch::search(const Library &library, const Embeddings::Route &route,
                                                                       const Question &question) {
    QSet<qint64> indexed;
    for (const qint64 bookId : question.books)
        if (const auto state = MeaningIndex::state(library, bookId); state && state->built() && state->model == route.model)
            indexed << bookId;
    if (indexed.isEmpty())
        return std::unexpected(Failure(Failure::Code::MeaningNotIndexed));
    auto asked = Embeddings::embed(route, {question.words}, Embeddings::Use::Question, std::chrono::seconds(30));
    if (!asked)
        return std::unexpected(asked.error());
    const MeaningIndex::Scope scope{
        .books = indexed, .model = route.model, .keeps = spoilerCut(library, question.whole), .candidates = std::max(50, question.limit)};
    return MeaningIndex::fused(library, MeaningIndex::nearest(library, asked->front(), scope),
                               MeaningIndex::worded(library, AnnotationWords::phrases(question.words), scope), question.limit);
}

QJsonObject MeaningSearch::toJson(const MeaningIndex::Hit &hit) {
    const bool meant = hit.meaningRank >= 0;
    QString by = meant ? u"meaning"_s : u"words"_s;
    if (meant && hit.wordsRank >= 0)
        by = u"both"_s;
    return hit.from.toJson({{"book", hit.bookId}, {"to", hit.to.toJson()}, {"text", hit.text}, {"by", by}, {"score", hit.score}});
}
