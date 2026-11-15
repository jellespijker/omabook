#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "ai.h"
#include "knowledgesearch.h"
#include "library.h"

// Finding books by a question the search box can't put: "the books where a
// mentor dies", "something about control theory I haven't finished". The
// quick model reads the library a batch of books at a time, each book as a
// line of what is known of it (its details, how far it was read, the start
// of its description, and the things the knowledge graph knows of it as far
// as the reader got), and names the books that answer, with a reason each.
// Nothing of the books' text is sent.
namespace AiSearch {

// Books to a request, and requests asked at once: a library of 1,600 books
// is 14 requests, three of them under way at a time.
inline constexpr int batchSize = 120;
inline constexpr int atOnce = 3;
// The most books a question looks through, and the most it answers with.
inline constexpr int mostBooks = 2400;
inline constexpr int mostPicks = 25;

// A book that answers, with why, in a line, and how well (0 to 100).
struct Pick {
    qint64 bookId = 0;
    QString why;
    int score = 0;
};

// The books in batches, at most mostBooks of them.
QList<QList<Book>> batches(const QList<Book> &books, int size = batchSize);
// The request for one batch; `known` adds what the graph knows of each book.
AiRequest request(const QString &question, const QList<Book> &batch, const KnowledgeIndex *known);
// The picks in an answer, of the books of the batch only.
QList<Pick> picksFrom(const QString &answer, const QList<Book> &batch);
// The best first, at most mostPicks, each book once.
QList<Pick> ranked(QList<Pick> picks);

// What a question found is kept, so asking it again doesn't ask the AI: by
// the library, the question, the collection it was asked in and the
// library's change counters, which move with any change to the books, the
// reading or the graph. Kept in the cache folder, the latest few dozen.
QString cacheKey(const Library &library, const QString &question, qint64 collection);
std::optional<QList<Pick>> cached(const QString &key);
void remember(const QString &key, const QList<Pick> &picks);

QJsonObject toJson(const Pick &pick, const std::optional<Book> &book);

// One question asked of a set of books, a few batches at a time. It works
// the same for the window and the command line: a batch that fails ends the
// whole question with the failure, and cancel() stops what is under way.
class Run : public QObject {
    Q_OBJECT

public:
    using Factory = std::function<std::unique_ptr<AiProvider>(QString *why)>;
    Run(Factory ai, QString question, const QList<Book> &books, std::shared_ptr<const KnowledgeIndex> known,
        QObject *parent = nullptr);
    ~Run() override;

    void start();
    void cancel();
    [[nodiscard]] int done() const noexcept { return m_done; }
    [[nodiscard]] int total() const noexcept { return int(m_batches.size()); }
    [[nodiscard]] int books() const noexcept { return m_books; }
    // What was found so far, the best first.
    [[nodiscard]] QList<Pick> picks() const { return ranked(m_picks); }

signals:
    // A batch is in: `done` of `total`.
    void progressed();
    void finished();
    void failed(const QString &error);

private:
    void next();

    Factory m_ai;
    QString m_question;
    QList<QList<Book>> m_batches;
    std::shared_ptr<const KnowledgeIndex> m_known;
    std::vector<std::unique_ptr<AiCall>> m_calls;
    QList<Pick> m_picks;
    int m_asked = 0, m_done = 0, m_books = 0;
    bool m_over = false;
};

} // namespace AiSearch
