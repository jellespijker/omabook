#pragma once

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <cstdint>
#include <expected>
#include <functional>
#include <stop_token>

#include "embeddings.h"
#include "failure.h"
#include "meaningindex.h"

class Library;
class Settings;

// A book's text found by what it means (#72), written once for the command
// line, the MCP tools and the window: a book indexed (its stretches embedded
// by the route's server and kept, MeaningIndex), the books due to be, and a
// search of some books by the meaning and the words of a question together,
// never past the reader's own place in a book unless the whole of it is asked
// for. Every function here waits for the server, so none is for the window's
// thread; each takes the library of the thread it runs on.
namespace MeaningSearch {

// Whether a book's index is there and made from the file it is read from now
// with this model: none, out of date (the file or the model changed) or fresh.
enum class Freshness : std::uint8_t { None, Stale, Fresh };
[[nodiscard]] Freshness freshness(const Library &library, qint64 bookId, const QString &model);

// How far an indexing has got, in stretches.
struct Progress {
    int done = 0;
    int total = 0;
};
// How many stretches go to the server at once.
inline constexpr int batch = 16;

// Indexes a book, as asked for (it is then kept up to date): reads its text,
// cuts it in stretches, has them embedded a batch at a time and keeps them in
// place of what it had. Stops between batches when `stop` is asked for,
// keeping nothing new. How many stretches it has, or why it couldn't.
[[nodiscard]] std::expected<int, Failure> index(const Library &library, const Embeddings::Route &route, qint64 bookId,
                                                const std::stop_token &stop = {}, const std::function<void(Progress)> &told = {});

// The books to index now: those asked for, or every book with a file when
// the settings say all, that aren't fresh for the route's model.
[[nodiscard]] QList<qint64> due(const Library &library, const Settings &settings, const QString &model);

struct Question {
    QString words;
    QSet<qint64> books; // where to look: one book, or a tenant's
    bool whole = false; // the whole of each book, past the reader's place too
    int limit = 10;
};
// The stretches that best answer the question, by its meaning and its words,
// of the books asked about that are indexed with the route's model. Fails
// when none of them is.
[[nodiscard]] std::expected<QList<MeaningIndex::Hit>, Failure> search(const Library &library, const Embeddings::Route &route,
                                                                      const Question &question);

// A hit as the command line and the tools say it: {book, section, fraction,
// to: {section, fraction}, text, by: "meaning", "words" or "both", score}.
[[nodiscard]] QJsonObject toJson(const MeaningIndex::Hit &hit);

} // namespace MeaningSearch
