#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#include "graphbuilder.h"
#include "library.h"

// Building the knowledge graph after the fact: for books read before the
// graph existed, or read elsewhere, one book or a shelf of them at a time.
// Each book is taken as far as it was read (all of it when it is marked as
// read), or whole when asked, and what was already built is not sent again.
namespace GraphBatch {

struct Choice {
    QList<qint64> books;
    bool whole = false; // to the end of each book, read or not
    bool again = false; // from the first page, also what was built before
};

// What a choice would cost before anything is sent.
struct Plan {
    struct Item {
        qint64 id = 0;
        QString title;
        qsizetype characters = 0; // of the book's text that would be sent
        int requests = 0;         // for its text, and one more when it is then compared with other books
        bool connects = false;    // whether that one more is among them
        QString skipped;          // why this book is left out, when it is
    };
    QList<Item> items;
    int books = 0;                // the ones that would be worked on
    int requests = 0;
    qsizetype characters = 0;
    // Tokens, roughly: the text, and the instructions and known graph that
    // go with every request.
    qint64 tokens() const { return characters / 4 + qint64(requests) * 4000; }
    QJsonObject toJson() const;
};
Plan plan(Library &library, const Choice &choice);

// The books a reader has read or is reading that have text to work from.
QList<qint64> readBooks(const Library &library);

struct Done {
    int books = 0, requests = 0, nodes = 0, edges = 0;
    int connections = 0;  // of those edges, the ones across books
    QStringList problems;
    bool stopped = false; // it was asked to stop before the last book
    QJsonObject toJson() const;
};
// Works through the books in turn. `stop` is asked before every request, and
// `said` hears of each one answered: which book of how many, and its totals.
Done run(Library &library, const Choice &choice, const GraphBuild::Asker &ask, const std::function<bool()> &stop = {},
         const std::function<void(int index, int total, const QString &title, const Done &soFar)> &said = {});

} // namespace GraphBatch
