#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <optional>

#include "ai.h"
#include "graph.h"
#include "graphbuilder.h"
#include "library.h"

// Connections across books. Once a book's graph has grown, its most telling
// things (themes, ideas, plot threads, its main characters) are put beside
// things of the same kinds from the other books the reader has read, and the
// model is asked which of them connect: an echo, a contrast, the same idea,
// an answer, a debt. What it finds become edges like any other, marked as
// connections across books (GraphEdge::across) and learned at the place in
// this book where its graph stood when they were noticed.
//
// The spoiler rule holds on both sides: of this book only what its graph
// holds, and of the others only what their reader has reached. It is one
// small request per book and build session, never one per chunk, and what
// is put side by side is bounded whatever the size of the library.
namespace GraphLinks {

// The most that is put side by side, and the most one answer may add.
constexpr int mostOwn = 30, mostOthers = 50, mostBooks = 40, mostFromOneBook = 10, mostLinks = 10, mostTopics = 3;

// What one request compares.
struct Offer {
    QList<GraphNode> own;            // of this book, the most tied-in first
    QList<GraphNode> others;         // of other books, the most relevant first
    QHash<qint64, QString> titles;   // for each of the others, the book it is from
    QString already;                 // the connections there are, as the model is shown them
    bool empty() const { return own.isEmpty() || others.isEmpty(); }
    // Tells one offer from another, so the same comparison is not paid for twice.
    QString mark() const;
};
// This book's things up to a place, and the things of other books chosen by
// relevance: books by the same author, of the same series or with the same
// tags first, then things whose words this book's things share.
Offer offer(const Library &library, qint64 bookId, const GraphPlace &upTo);
AiRequest request(const Library &library, qint64 bookId, const Offer &offer);

struct Applied {
    bool understood = false; // the answer held the JSON asked for
    int links = 0, topics = 0;
};
// Lays a model's answer into the graph: only links between a thing that was
// offered of this book and one that was offered of another, and none
// between two that are connected already. An answer that is not the JSON
// asked for changes nothing.
Applied apply(Library &library, qint64 bookId, const GraphPlace &at, const Offer &offer, const QString &answer);

// Whether connecting a book would send a request now: it has a graph, there
// is something to compare it with, and that comparison was not made before.
bool due(const Library &library, qint64 bookId);
// Whether any other book has a graph its reader has reached, for an estimate
// made before this book's own graph is there.
bool company(const Library &library, qint64 bookId);

struct Connected {
    bool asked = false;      // a request was sent
    int links = 0, topics = 0;
    QString skipped;         // why none was sent, when none was
    QString problem;         // what went wrong, when something did
};
// Connects a book to the others, with one request. `again` asks even when
// the same comparison was made before.
Connected connect(Library &library, qint64 bookId, const GraphBuild::Asker &ask, bool again = false);
// Keeps that a comparison was made, so it is not made again.
void setConnected(Library &library, qint64 bookId, const GraphPlace &at, const Offer &offer);

} // namespace GraphLinks
