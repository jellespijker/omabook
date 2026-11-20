#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

#include "ai.h"
#include "document.h"
#include "graph.h"
#include "graphpassages.h"
#include "graphtimes.h"
#include "library.h"

// Growing the knowledge graph from what is read. The model is shown only the
// text read since the graph of the book was last brought up to date, and the
// few known things that text could touch; it answers with nodes and edges as
// JSON, which are laid into the graph with the place each was learned at. A
// note of the kind "graph" kept with the book marks how far that got, so no
// page is ever sent twice and nothing past the reader's place is sent at all.
namespace GraphBuild {

// The most text sent with one request, in characters (some six thousand
// tokens): a chapter or two. Longer stretches are cut into several requests.
constexpr qsizetype chunkSize = 24000;
// Less than this since the last time is not worth a request yet.
constexpr qsizetype worthAsking = 1500;

// Passages (graphpassages.h) as requests: each at most `most` characters, cut between
// paragraphs where a section is longer than that.
QList<QList<Passage>> chunks(const QList<Passage> &passages, qsizetype most = chunkSize);

// How far the graph of a book has been built; nothing when it never was.
std::optional<GraphPlace> builtTo(const Library &library, qint64 bookId);
void setBuiltTo(Library &library, qint64 bookId, const GraphPlace &place);
// Forgets how far it got, so the next build starts at the first page. What
// is in the graph stays, and is brought up to date rather than doubled.
void forgetBuilt(Library &library, qint64 bookId);

// What the graph knows that these passages could touch, as the model is
// shown it: things of this book and the earlier books of its series whose
// names occur in the text and the threads still open, with their summaries;
// then the most recently met of the rest by name only. Bounded whatever the
// size of the graph.
QString known(const Library &library, qint64 bookId, const QList<Passage> &passages);
// What the general graph holds from the reader's OTHER books that these
// passages could be about too, as the model is shown it, each with the book
// it is from: the themes, real-world topics and ideas that the text's words
// point at (and failing that the ones most lately met), and the groups and
// places whose name the text holds. Only what the reader of those books has
// reached, by name and one line, and at most forty of them, so two books
// that share a theme meet in one node.
QString elsewhere(const Library &library, qint64 bookId, const QList<Passage> &passages);
AiRequest request(const Library &library, qint64 bookId, const QList<Passage> &passages);

struct Applied {
    bool understood = false; // the answer held the JSON asked for
    int nodes = 0, edges = 0;
    Timed times; // what it said of when its events happen
};
// Lays a model's answer into the graph. Every node and edge is placed in
// the passages it came from, never after their end; an answer that is not
// the JSON asked for changes nothing.
Applied apply(Library &library, qint64 bookId, const QList<Passage> &passages, const QString &answer);

// Asks a model one thing and waits; BookOps::ask is one.
using Asker = std::function<bool(const AiRequest &request, QString *answer, QString *error)>;

struct Built {
    int requests = 0, nodes = 0, edges = 0;
    qsizetype sent = 0;       // characters of the book's text sent
    Timed times;              // the times of events noted, and those dropped
    QStringList problems;     // answers that could not be read, and the call that failed
    bool complete = false;    // it got as far as was asked
    GraphPlace upTo;          // how far the graph of the book now goes
};
// Brings the graph of a book up to a place, a request at a time, each
// starting where the last ended. A call that fails stops it there, to be
// picked up the next time; an answer that can't be read is passed over.
// `said` hears of each request as it is answered.
Built build(Library &library, const Document &document, qint64 bookId, const GraphPlace &upTo, const Asker &ask,
            const std::function<void(const Built &)> &said = {});

} // namespace GraphBuild

// The same, for the window: beside the reader, without holding it up. It is
// given the passages read since the graph was last built and sends them, a
// request at a time, to a model of its own.
class GraphBuilder : public QObject {
    Q_OBJECT

public:
    using Factory = std::function<std::unique_ptr<AiProvider>(QString *why)>;
    // The library is asked for when an answer arrives, since the window can
    // switch libraries while a request is under way.
    using LibraryNow = std::function<Library *()>;

    GraphBuilder(Factory factory, LibraryNow library, QObject *parent = nullptr);

    bool busy() const { return m_call != nullptr; }
    // Sends these passages of a book, `most` requests at most; what is left
    // waits for the next time. Does nothing while it is already at work.
    // With `connect`, the book is then compared with the other books read,
    // once, in one more request (see GraphLinks).
    void grow(qint64 bookId, const QList<GraphBuild::Passage> &passages, int most = 3, bool connect = false);
    void cancel();

signals:
    void busyChanged();
    // The graph of a book took in what a request found.
    void grown(qint64 bookId, int nodes, int edges);
    void failed(const QString &error);

private:
    void next();
    bool connectNow(Library *library);

    Factory m_factory;
    LibraryNow m_library;
    QString m_root; // the library the book is in, by its folder
    qint64 m_bookId = 0;
    QList<QList<GraphBuild::Passage>> m_chunks;
    bool m_connect = false; // the connections across books are still to be asked for
    std::unique_ptr<AiCall> m_call;
};
