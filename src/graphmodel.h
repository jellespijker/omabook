#pragma once

#include <QHash>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <optional>

#include "graph.h"
#include "graphlayout.h"

class Backend;

// Says that the knowledge graph changed, to every view of it that is open.
// Whatever writes to the graph (the AI noting what was read, a plugin, a
// view's own editing) calls GraphFeed::announce() when it is done, from any
// thread, and the views read the graph again.
class GraphFeed : public QObject {
    Q_OBJECT

public:
    static GraphFeed *instance();
    static void announce();

signals:
    void changed();
};

// The knowledge graph as one view of it browses it: which graph is shown (a
// scope, a book, a series), around which node and how far, with which kinds
// left out; the node that is selected and everything known of it; and where
// each node is in the picture. The picture and the list beside it are two
// faces of the same rows, so what is selected in one is selected in the other.
//
// A book's graph is never ahead of its reader: it is asked for up to the
// place the reader is at, and nothing here (the search, a node's relations,
// what is around a node) looks past what that returned.
class GraphBrowser : public QObject {
    Q_OBJECT
    // The Backend whose library is browsed. Not called `backend`, so a view
    // can bind it to the window's `backend` without meaning this property.
    Q_PROPERTY(QObject *source READ backend WRITE setBackend NOTIFY backendChanged)
    // What is shown: "scope", "book" or "series", and which one.
    Q_PROPERTY(QString showing READ showing NOTIFY changed)
    Q_PROPERTY(QString scope READ scope NOTIFY changed)
    Q_PROPERTY(qint64 bookId READ bookId NOTIFY changed)
    Q_PROPERTY(QString series READ series NOTIFY changed)
    // The book's title, for a book's graph.
    Q_PROPERTY(QString bookTitle READ bookTitle NOTIFY changed)
    // Whether the graph stops at a place in the book: it is being read.
    Q_PROPERTY(bool bounded READ bounded NOTIFY changed)
    Q_PROPERTY(int nodeCount READ nodeCount NOTIFY changed)
    Q_PROPERTY(int edgeCount READ edgeCount NOTIFY changed)
    // How many nodes there are before the filters, and whether any of them
    // is something a book told rather than the library's own (a book, an
    // author, a series, a tag).
    Q_PROPERTY(int knownCount READ knownCount NOTIFY changed)
    Q_PROPERTY(bool storied READ storied NOTIFY changed)
    // The kinds there are, each {kind, count, shown}, in the order they are
    // always listed in.
    Q_PROPERTY(QVariantList kinds READ kinds NOTIFY changed)
    // The nodes as rows, by kind and then by name: {id, kind, name, summary, links}.
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)
    // The graphs there are: [{scope, things}], the general one first.
    Q_PROPERTY(QVariantList scopes READ scopes NOTIFY changed)
    // The books something is known of: [{bookId, title, series, things}].
    Q_PROPERTY(QVariantList books READ books NOTIFY changed)
    Q_PROPERTY(bool onlyAsked READ onlyAsked WRITE setOnlyAsked NOTIFY changed)
    // Beyond this book: whether a book's graph also shows, as a ring around
    // it, what its things connect to in other books, and how many such
    // things there are to show.
    Q_PROPERTY(bool beyond READ beyond WRITE setBeyond NOTIFY changed)
    Q_PROPERTY(int beyondCount READ beyondCount NOTIFY changed)
    // In the library's graph: only the connections between books, and the
    // books they join.
    Q_PROPERTY(bool onlyAcross READ onlyAcross WRITE setOnlyAcross NOTIFY changed)
    // How many edges away from the node in the centre is shown; 0 for all of it.
    Q_PROPERTY(int depth READ depth WRITE setDepth NOTIFY changed)
    Q_PROPERTY(qint64 centre READ centre NOTIFY changed)
    Q_PROPERTY(bool canGoBack READ canGoBack NOTIFY changed)
    Q_PROPERTY(qint64 selected READ selected WRITE select NOTIFY selectedChanged)
    // Everything known of the selected node; empty when there is none.
    Q_PROPERTY(QVariantMap details READ details NOTIFY selectedChanged)
    Q_PROPERTY(bool settling READ settling NOTIFY settlingChanged)
    // How much wider than tall the picture is laid out, which its view
    // says: about 1.6 for a window, 0.7 for a pane beside the page.
    Q_PROPERTY(double aspect READ aspect WRITE setAspect NOTIFY aspectChanged)

public:
    // The kinds Omabook itself makes, in the order they are listed in.
    static QStringList knownKinds();

    struct Shown {
        GraphNode node;
        int links = 0;
        // Of the ring: a thing of another book, with the book it is from
        // (0 and nothing for one that is in no book, or is a book itself).
        bool outer = false;
        qint64 fromBook = 0;
        QString from;
    };
    struct Link {
        int from = 0, to = 0; // rows of shown()
        GraphEdge edge;
    };

    explicit GraphBrowser(QObject *parent = nullptr);

    QObject *backend() const;
    void setBackend(QObject *backend);
    // Tests work on a library of their own, without a backend.
    void setLibrary(const Library *library);

    QString showing() const { return m_what == What::Book ? "book" : m_what == What::Series ? "series" : "scope"; }
    QString scope() const { return m_scope; }
    qint64 bookId() const { return m_what == What::Book ? m_bookId : 0; }
    QString series() const { return m_what == What::Series ? m_series : QString(); }
    QString bookTitle() const { return m_bookTitle; }
    bool bounded() const { return m_what == What::Book && m_upTo.has_value(); }
    int nodeCount() const { return int(m_shown.size()); }
    int edgeCount() const { return int(m_links.size()); }
    int knownCount() const { return m_knownCount; }
    bool storied() const { return m_storied; }
    QVariantList kinds() const { return m_kinds; }
    QVariantList rows() const { return m_rows; }
    QVariantList scopes() const;
    QVariantList books() const;
    bool onlyAsked() const { return m_onlyAsked; }
    void setOnlyAsked(bool only);
    bool beyond() const { return m_beyond; }
    void setBeyond(bool beyond);
    int beyondCount() const { return m_beyondCount; }
    bool onlyAcross() const { return m_onlyAcross; }
    void setOnlyAcross(bool only);
    int depth() const { return m_depth; }
    void setDepth(int depth);
    qint64 centre() const { return m_centre; }
    bool canGoBack() const { return !m_history.isEmpty(); }
    qint64 selected() const { return m_selected; }
    QVariantMap details() const { return m_details; }
    bool settling() const { return m_settle.isActive(); }
    double aspect() const { return m_aspect; }
    void setAspect(double aspect);

    // A whole scope: "" for everything read, "world:…" for a world of its own.
    Q_INVOKABLE void showScope(const QString &scope = {});
    // What is known of one book, as far as it was read: all of it when it
    // is finished, else up to where the reader left it.
    Q_INVOKABLE void showBook(qint64 bookId);
    // The same for the book that is open, up to the place on the page.
    Q_INVOKABLE void showBookUpTo(qint64 bookId, int section, double fraction);
    // Every book of a series, each as far as it was read.
    Q_INVOKABLE void showSeries(const QString &series);
    // Reads the graph again, keeping what is shown and where it is.
    Q_INVOKABLE void refresh();

    Q_INVOKABLE void select(qint64 node);
    // Selects a node wherever it is: one that the filters hide is shown
    // again, and one further off becomes the centre.
    Q_INVOKABLE void jumpTo(qint64 node);
    // The nearest node along an edge towards "left", "right", "up" or
    // "down" of the selected one; any node that way when no edge leads there.
    Q_INVOKABLE void step(const QString &direction);
    // The next or the previous row.
    Q_INVOKABLE void advance(int by);
    // Makes the selected node the centre and shows what is around it.
    Q_INVOKABLE void recentre();
    // Back to what was shown before the last recentring; false when there is nothing to go back to.
    Q_INVOKABLE bool back();
    Q_INVOKABLE void setKindShown(const QString &kind, bool shown);
    Q_INVOKABLE void showEveryKind();
    // The nodes going by these words, the best match first: [{id, kind, name, summary}].
    Q_INVOKABLE QVariantList find(const QString &words) const;

    // Editing by hand. Each says whether it worked.
    Q_INVOKABLE bool edit(qint64 node, const QString &name, const QString &summary);
    Q_INVOKABLE bool merge(qint64 keep, qint64 other);
    Q_INVOKABLE bool remove(qint64 node);
    Q_INVOKABLE bool unlink(qint64 edge);
    // Adds a thing (or finds the one of that kind and name), relates the
    // selected node to it when a relation is given, and selects it.
    Q_INVOKABLE qint64 add(const QString &kind, const QString &name, const QString &summary, const QString &relation);

    // For the picture: the nodes, the edges among them, and where each node is.
    const QList<Shown> &shown() const { return m_shown; }
    const QList<Link> &links() const { return m_links; }
    const QList<QPointF> &positions() const { return m_layout.positions(); }
    int rowOf(qint64 node) const { return m_index.value(node, -1); }
    // The rows one edge away from a row.
    const QList<int> &beside(int row) const { return m_beside.at(row); }
    // Lays the picture out to the end at once, for tests and screenshots.
    void settleNow();

signals:
    void backendChanged();
    // What is shown changed: the nodes, the edges, the filters.
    void changed();
    void selectedChanged();
    // Nodes moved, while the picture settles.
    void moved();
    void settlingChanged();
    void aspectChanged();
    // A whole other graph is shown, or it was recentred: the picture should
    // bring it all into view again.
    void reframed();

private:
    enum class What { Scope, Book, Series };
    // What was shown, to go back to: which graph, around what, how far.
    struct Visit {
        qint64 centre = 0;
        int depth = 0;
        qint64 selected = 0;
        What what = What::Scope;
        QString scope, series, bookTitle;
        qint64 bookId = 0;
        std::optional<GraphPlace> upTo;
    };
    Visit here() const;
    // The book a thing is from, other than the one shown: its id and title.
    QPair<qint64, QString> bookOf(const GraphNode &node) const;

    const Library *library() const;
    // Where a book's reader is, or nothing when the book is finished.
    std::optional<GraphPlace> placeIn(const Book &book) const;
    // What may be shown at all, before the centre and the filters.
    GraphView allowed() const;
    void show(What what);
    void load(bool keepPlaces);
    void describe();

    Backend *m_backend = nullptr;
    const Library *m_library = nullptr;
    What m_what = What::Scope;
    QString m_scope, m_series, m_bookTitle;
    qint64 m_bookId = 0;
    std::optional<GraphPlace> m_upTo;
    QSet<QString> m_hidden;
    bool m_onlyAsked = false;
    bool m_beyond = true, m_onlyAcross = false;
    int m_beyondCount = 0;
    // What the book shown connects to in other books, whether shown or not.
    GraphView m_outer;
    QList<qint64> m_ownNodes, m_ownEdges;
    int m_depth = 0;
    qint64 m_centre = 0;
    QList<Visit> m_history;
    qint64 m_selected = 0;

    // The graph before the kinds are filtered, for a node's relations and
    // the search, which the toggles don't narrow.
    GraphView m_base;
    QList<Shown> m_shown;
    QList<Link> m_links;
    QHash<qint64, int> m_index;
    QList<QList<int>> m_beside;
    QVariantList m_kinds, m_rows;
    QList<qint64> m_order;
    QVariantMap m_details;
    int m_knownCount = 0;
    bool m_storied = false;
    GraphLayout m_layout;
    double m_aspect = 1;
    QTimer m_settle, m_refreshSoon;
};

// Makes GraphBrowser, GraphCanvas and GraphMark known to QML, as
// `import Omabook`. Safe to call more than once.
void registerGraphTypes();
