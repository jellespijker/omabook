#include "meaningindex.h"

#include <QDateTime>
#include <QHash>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <ranges>

#include "changelog.h"
#include "graphindex.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// How far down a list a stretch still counts in the fusion: the usual 60,
// which keeps one list's first from drowning the other's.
constexpr double fusionDepth = 60;

// "book_id IN (1, 2, 3)" for these books; none matches nothing.
[[nodiscard]] QString amongBooks(const QSet<qint64> &books, QStringView column) {
    if (books.isEmpty())
        return u"0"_s;
    QStringList numbers;
    for (const qint64 book : books)
        numbers << QString::number(book);
    return u"%1 IN (%2)"_s.arg(column, numbers.join(u", "_s));
}

[[nodiscard]] QList<qint64> numbers(QSqlQuery query) {
    QList<qint64> found;
    while (query.next())
        found << query.value(0).toLongLong();
    return found;
}

[[nodiscard]] bool kept(const MeaningIndex::Scope &scope, qint64 bookId, const BookPlace &to) {
    return !scope.keeps || scope.keeps(bookId, to);
}

// The dot product of a stored vector, as its bytes, and the question.
[[nodiscard]] double dot(const QByteArray &bytes, const MeaningIndex::Vector &question, MeaningIndex::Vector &buffer) {
    if (static_cast<size_t>(bytes.size()) != question.size() * sizeof(float))
        return -2; // made by another model than the question's: never near
    buffer.resize(question.size());
    std::memcpy(buffer.data(), bytes.constData(), static_cast<size_t>(bytes.size()));
    return std::inner_product(buffer.begin(), buffer.end(), question.begin(), 0.0);
}

} // namespace

QStringList MeaningIndex::step() {
    const auto tokenizer = "tokenize=\"unicode61 remove_diacritics 2 categories 'L* N* Co M*'\""_L1;
    return {u"CREATE TABLE IF NOT EXISTS meaning_books (book_id INTEGER PRIMARY KEY REFERENCES books(id) ON DELETE CASCADE, "
            "wanted INTEGER NOT NULL DEFAULT 0, hash TEXT NOT NULL DEFAULT '', model TEXT NOT NULL DEFAULT '', "
            "dimensions INTEGER NOT NULL DEFAULT 0, made INTEGER NOT NULL DEFAULT 0)"_s,
            u"CREATE TABLE IF NOT EXISTS meaning_stretches (id INTEGER PRIMARY KEY, book_id INTEGER NOT NULL REFERENCES books(id) "
            "ON DELETE CASCADE, section INTEGER NOT NULL, from_fraction REAL NOT NULL, to_fraction REAL NOT NULL, text TEXT NOT NULL, "
            "vector BLOB NOT NULL)"_s,
            u"CREATE INDEX IF NOT EXISTS meaning_stretches_book ON meaning_stretches(book_id, section, from_fraction)"_s,
            // Made anew whole from the stretches, so the step can be taken again.
            u"DROP TABLE IF EXISTS meaning_words"_s,
            u"CREATE VIRTUAL TABLE meaning_words USING fts5(text, content='meaning_stretches', content_rowid='id', %1)"_s.arg(tokenizer),
            u"INSERT INTO meaning_words (meaning_words) VALUES ('rebuild')"_s,
            u"CREATE TRIGGER IF NOT EXISTS meaning_stretches_words_insert AFTER INSERT ON meaning_stretches BEGIN "
            "INSERT INTO meaning_words (rowid, text) VALUES (NEW.id, NEW.text); END"_s,
            u"CREATE TRIGGER IF NOT EXISTS meaning_stretches_words_delete AFTER DELETE ON meaning_stretches BEGIN "
            "INSERT INTO meaning_words (meaning_words, rowid, text) VALUES ('delete', OLD.id, OLD.text); END"_s};
}

std::optional<MeaningIndex::State> MeaningIndex::state(const Library &library, qint64 bookId) {
    QSqlQuery query = ChangeLog::query(library,
                                       u"SELECT wanted, hash, model, dimensions, made, (SELECT COUNT(*) FROM meaning_stretches "
                                       "WHERE book_id = b.book_id) FROM meaning_books AS b WHERE book_id = ?"_s,
                                       {bookId});
    if (!query.next())
        return std::nullopt;
    return State{.wanted = query.value(0).toBool(),
                 .hash = query.value(1).toString(),
                 .model = query.value(2).toString(),
                 .dimensions = query.value(3).toInt(),
                 .stretches = query.value(5).toInt(),
                 .made = query.value(4).toLongLong()};
}

QList<qint64> MeaningIndex::books(const Library &library) {
    return numbers(ChangeLog::query(library, u"SELECT book_id FROM meaning_books ORDER BY book_id"_s));
}

QList<qint64> MeaningIndex::wanted(const Library &library) {
    return numbers(ChangeLog::query(library, u"SELECT book_id FROM meaning_books WHERE wanted = 1 ORDER BY book_id"_s));
}

std::expected<void, Failure> MeaningIndex::want(const Library &library, qint64 bookId, bool wanted) {
    const bool done = wanted ? ChangeLog::exec(library,
                                               u"INSERT INTO meaning_books (book_id, wanted) VALUES (?, 1) "
                                               "ON CONFLICT (book_id) DO UPDATE SET wanted = 1"_s,
                                               {bookId})
                             : ChangeLog::exec(library, u"DELETE FROM meaning_stretches WHERE book_id = ?"_s, {bookId})
                                   && ChangeLog::exec(library, u"DELETE FROM meaning_books WHERE book_id = ?"_s, {bookId});
    if (!done)
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {u"meaning_books"_s}));
    return {};
}

std::expected<void, Failure> MeaningIndex::store(const Library &library, qint64 bookId, const QString &hash, const QString &model,
                                                 std::span<const Stretch> stretches) {
    const Library::Batch batch(library, Library::Batch::Lock::AtOnce);
    const int dimensions = stretches.empty() ? 0 : static_cast<int>(stretches.front().vector.size());
    bool done = batch.begun() && ChangeLog::exec(library, u"DELETE FROM meaning_stretches WHERE book_id = ?"_s, {bookId});
    for (const Stretch &stretch : stretches) {
        if (!done)
            break;
        done =
            ChangeLog::exec(library,
                            u"INSERT INTO meaning_stretches (book_id, section, from_fraction, to_fraction, text, vector) "
                            "VALUES (?, ?, ?, ?, ?, ?)"_s,
                            {bookId, stretch.from.section, stretch.from.fraction, stretch.to.fraction, stretch.text, pack(stretch.vector)});
    }
    done = done
           && ChangeLog::exec(library,
                              u"INSERT INTO meaning_books (book_id, wanted, hash, model, dimensions, made) VALUES (?, 1, ?, ?, ?, ?) "
                              "ON CONFLICT (book_id) DO UPDATE SET hash = excluded.hash, model = excluded.model, "
                              "dimensions = excluded.dimensions, made = excluded.made"_s,
                              {bookId, hash, model, dimensions, QDateTime::currentSecsSinceEpoch()});
    if (!done) {
        // What part of it was written is not searched: the book is made again
        // when it is next asked for. Nothing better to do if this fails too.
        std::ignore = ChangeLog::exec(library, u"UPDATE meaning_books SET made = 0 WHERE book_id = ?"_s, {bookId});
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {u"meaning_stretches"_s}));
    }
    return {};
}

void MeaningIndex::normalize(Vector &vector) {
    const double length = std::sqrt(std::inner_product(vector.begin(), vector.end(), vector.begin(), 0.0));
    if (length <= 0)
        return;
    std::ranges::transform(vector, vector.begin(), [length](float value) { return static_cast<float>(value / length); });
}

QByteArray MeaningIndex::pack(const Vector &vector) {
    QByteArray bytes(static_cast<qsizetype>(vector.size() * sizeof(float)), Qt::Uninitialized);
    std::memcpy(bytes.data(), vector.data(), static_cast<size_t>(bytes.size()));
    return bytes;
}

MeaningIndex::Vector MeaningIndex::unpack(const QByteArray &bytes) {
    Vector vector(static_cast<size_t>(bytes.size()) / sizeof(float));
    std::memcpy(vector.data(), bytes.constData(), vector.size() * sizeof(float));
    return vector;
}

QList<MeaningIndex::Hit> MeaningIndex::nearest(const Library &library, const Vector &question, const Scope &scope) {
    QList<Hit> hits;
    Vector buffer;
    QSqlQuery query =
        ChangeLog::query(library,
                         u"SELECT s.id, s.book_id, s.section, s.from_fraction, s.to_fraction, s.vector FROM meaning_stretches "
                         "AS s JOIN meaning_books AS b ON b.book_id = s.book_id WHERE b.made > 0 AND b.model = ? AND %1"_s.arg(
                             amongBooks(scope.books, u"s.book_id")),
                         {scope.model});
    while (query.next()) {
        const qint64 bookId = query.value(1).toLongLong();
        const int section = query.value(2).toInt();
        const BookPlace to{.section = section, .fraction = query.value(4).toDouble()};
        if (!kept(scope, bookId, to))
            continue;
        hits.append({.id = query.value(0).toLongLong(),
                     .bookId = bookId,
                     .from = {.section = section, .fraction = query.value(3).toDouble()},
                     .to = to,
                     .text = {},
                     .nearness = dot(query.value(5).toByteArray(), question, buffer)});
    }
    const auto keep = std::min<qsizetype>(hits.size(), std::max(0, scope.candidates));
    std::ranges::partial_sort(hits, hits.begin() + keep, std::ranges::greater{}, &Hit::nearness);
    hits.resize(keep);
    for (qsizetype rank = 0; rank < hits.size(); ++rank)
        hits[rank].meaningRank = static_cast<int>(rank);
    return hits;
}

QList<MeaningIndex::Hit> MeaningIndex::worded(const Library &library, const QStringList &words, const Scope &scope) {
    const QStringList asked = words | std::views::transform(GraphIndex::phrase)
                              | std::views::filter([](const std::optional<QString> &phrase) { return phrase.has_value(); })
                              | std::views::transform([](const std::optional<QString> &phrase) { return *phrase; })
                              | std::ranges::to<QStringList>();
    QList<Hit> hits;
    if (asked.isEmpty())
        return hits;
    // More than are wanted, since the spoiler cut may leave out the best.
    QSqlQuery query = ChangeLog::query(library,
                                       u"SELECT s.id, s.book_id, s.section, s.from_fraction, s.to_fraction FROM meaning_words JOIN "
                                       "meaning_stretches AS s ON s.id = meaning_words.rowid WHERE meaning_words MATCH ? AND %1 "
                                       "ORDER BY bm25(meaning_words) LIMIT ?"_s.arg(amongBooks(scope.books, u"s.book_id")),
                                       {asked.join(u" AND "_s), std::max(0, scope.candidates) * 20});
    while (query.next() && hits.size() < scope.candidates) {
        const qint64 bookId = query.value(1).toLongLong();
        const int section = query.value(2).toInt();
        const BookPlace to{.section = section, .fraction = query.value(4).toDouble()};
        if (!kept(scope, bookId, to))
            continue;
        hits.append({.id = query.value(0).toLongLong(),
                     .bookId = bookId,
                     .from = {.section = section, .fraction = query.value(3).toDouble()},
                     .to = to,
                     .text = {},
                     .nearness = 0,
                     .meaningRank = -1,
                     .wordsRank = static_cast<int>(hits.size())});
    }
    return hits;
}

QList<MeaningIndex::Hit> MeaningIndex::fused(const Library &library, const QList<Hit> &meaning, const QList<Hit> &words, int limit) {
    QHash<qint64, Hit> together;
    for (const Hit &hit : meaning)
        together.insert(hit.id, hit);
    for (const Hit &hit : words) {
        Hit &one = together[hit.id];
        if (one.id == 0)
            one = hit;
        one.wordsRank = hit.wordsRank;
    }
    QList<Hit> hits = together.values();
    for (Hit &hit : hits)
        hit.score = (hit.meaningRank >= 0 ? 1 / (fusionDepth + hit.meaningRank + 1) : 0)
                    + (hit.wordsRank >= 0 ? 1 / (fusionDepth + hit.wordsRank + 1) : 0);
    std::ranges::sort(
        hits, [](const Hit &one, const Hit &other) { return one.score != other.score ? one.score > other.score : one.id < other.id; });
    hits.resize(std::min<qsizetype>(hits.size(), std::max(0, limit)));
    for (Hit &hit : hits)
        if (QSqlQuery text = ChangeLog::query(library, u"SELECT text FROM meaning_stretches WHERE id = ?"_s, {hit.id}); text.next())
            hit.text = text.value(0).toString();
    return hits;
}
