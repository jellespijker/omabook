#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QVariantMap>
#include <memory>
#include <optional>
#include <vector>

#include "graph.h"
#include "library.h"

// Finding books by what the knowledge graph knows of them: the people,
// places, groups, ideas, items, events, plot threads, themes and topics met
// in a book, and the questions asked of it. A search for "Mara" finds the
// book Mara Venn is in, and says so.
//
// Only what the reader already knows counts, by the rule the rest of the
// app keeps (GraphContext::reachOf): all of a book marked as read, up to the
// furthest place of one being read, and nothing of a book not begun, however
// far its graph was built. A name a thing was given later in the book, or a
// summary from past the reader's page, is not looked in. What was put in by
// hand with no place in a book (a name, a summary, a tie to a book, a
// question) is the reader's own and always counts. The graphs of the spaces
// subscribed to count too, as far as the reader has read each book there
// (GraphContext::layers), and each match says whose it is.
//
// The graph is read once into memory, in a few statements, and a search
// then looks through that: nothing is read from the database per keystroke.

// Why a book was found: the thing, and where in the book it was met (a
// place of no book when it was asked or put in by hand without one).
struct KnownMatch {
    qint64 bookId = 0;
    qint64 node = 0;
    QString kind;
    QString name;
    GraphMoment met;
    // Found in what is said of the thing rather than its names (knows: only).
    bool inSummary = false;
    // Of a graph someone shared (GraphLayers::from): "Ada's shared graph";
    // empty for one's own.
    QString from{};

    // {"node", "kind", "name", "met": {"book", "section", "fraction", "anchor"}, "from"}.
    QJsonObject toJson() const;
    QVariantMap toMap() const;
};

class KnowledgeIndex {
public:
    // The kinds of things looked in, in the order they say most of a book.
    static QStringList kinds();

    // Everything a reader of each book knows, read from the library now:
    // in each of these tenants apart (ADR 0003), so that what is known of a
    // book is only what its own tenant knows; in every tenant of the library
    // when none are named.
    static KnowledgeIndex build(const Library &library);
    static KnowledgeIndex build(const Library &library, const QList<Tenant> &tenants);

    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }
    // How many (thing, book) pairs are known.
    [[nodiscard]] qsizetype size() const noexcept { return qsizetype(m_entries.size()); }

    // The books a word finds, each with the thing that found it: the word
    // begins one of the thing's known names, or one of their words ("venn"
    // finds "Mara Venn"); with `deep`, also a word of what is known to be
    // said of it. A word shorter than three letters finds nothing by name.
    [[nodiscard]] QHash<qint64, KnownMatch> find(const QString &word, bool deep = false) const;
    // What is known of one book, the kinds that say most of it first, at
    // most `most`: what the AI is told of a book it is searching.
    [[nodiscard]] QList<KnownMatch> of(qint64 bookId, int most = 10) const;

    // What the library's change counters said when it was read.
    Library::Changes changes;

private:
    struct Entry {
        qint64 book = 0, node = 0;
        int kind = 0;           // an index into kinds()
        QString name;           // as shown
        QString names, said;    // the known names, and what is said of it, case folded
        GraphMoment met;
        QString from; // a graph someone shared it is of
    };
    // Takes in what a tenant's graph knows of its books; a thing and book
    // already taken (a book in two tenants) is not taken again.
    void take(const Graph &graph, const GraphKnown &known, QHash<std::pair<qint64, qint64>, qsizetype> &taken);

    std::vector<Entry> m_entries;
    QHash<qint64, QList<qsizetype>> m_byBook;
};

// The words of one search, looked up in an index the first time each is
// asked for and remembered after, with why each book was found by what is
// known. This is what Book::matches is given to look past a book's details.
class KnownWords {
public:
    explicit KnownWords(std::shared_ptr<const KnowledgeIndex> index = {}) : m_index(std::move(index)) {}

    [[nodiscard]] bool hasIndex() const noexcept { return m_index && !m_index->empty(); }
    // Whether the word finds the book through what is known; remembers why.
    bool operator()(const Book &book, const QString &word, bool deep);
    // With looking off, only words already looked up are answered and the
    // others find nothing: what the shelf does while the typing goes on.
    void setLooking(bool looking) noexcept { m_looking = looking; }
    [[nodiscard]] bool looking() const noexcept { return m_looking; }
    // Whether a word was asked for that wasn't looked up for want of looking.
    [[nodiscard]] bool missed() const noexcept { return m_missed; }
    void forgetWhy() { m_why.clear(); m_missed = false; }
    [[nodiscard]] std::optional<KnownMatch> why(qint64 bookId) const;
    // A Book::Knows that asks this; it must not outlive it.
    [[nodiscard]] Book::Knows asker();

private:
    std::shared_ptr<const KnowledgeIndex> m_index;
    QHash<QString, QHash<qint64, KnownMatch>> m_found; // by "deep:word" or ":word"
    QHash<qint64, KnownMatch> m_why;
    bool m_looking = true;
    bool m_missed = false;
};

namespace KnowledgeSearch {

// One book a search found, with why when it was what is known that found it.
struct Found {
    Book book;
    std::optional<KnownMatch> why;
};
// The books a search finds: by their details as Book::matches reads them,
// and by what is known of them. The index is read here when none is given.
QList<Found> search(const Library &library, const QList<Book> &books, const QString &query,
                    const std::shared_ptr<const KnowledgeIndex> &index = {});

} // namespace KnowledgeSearch
