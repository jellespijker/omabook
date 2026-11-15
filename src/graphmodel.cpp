#include "graphmodel.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QQmlEngine>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <functional>
#include <ranges>
#include <utility>

#include "backend.h"
#include "graphcanvas.h"
#include "graphcontext.h"

namespace {

// What the library itself puts in the graph, as opposed to what a book told.
bool ofTheLibrary(const QString &kind) { return kind == "book" || kind == "author" || kind == "series" || kind == "tag"; }

// Where a kind comes in the lists: the known ones in their order, then the rest.
int rankOf(const QString &kind) {
    const int at = int(GraphBrowser::knownKinds().indexOf(kind));
    return at < 0 ? int(GraphBrowser::knownKinds().size()) : at;
}

// The nodes whose spoilers were opened, for as long as the program runs: a
// choice made for one node of one library, never for the rest.
QSet<QString> &opened() {
    static QSet<QString> nodes;
    return nodes;
}
QString openedKey(const Library *library, qint64 node) { return (library ? library->root() : QString()) + "\n" + QString::number(node); }

bool before(const GraphNode &a, const GraphNode &b) {
    const int ranks = rankOf(a.kind) - rankOf(b.kind);
    if (ranks != 0)
        return ranks < 0;
    if (a.kind != b.kind)
        return a.kind < b.kind;
    const int names = QString::localeAwareCompare(a.name.toLower(), b.name.toLower());
    return names != 0 ? names < 0 : a.id < b.id;
}

// Whether a node goes by every one of the words.
bool goesBy(const GraphNode &node, const QStringList &words) {
    const QString names = (node.name + " " + node.aliases().join(' ')).toLower();
    return std::ranges::all_of(words, [&names](const QString &word) { return names.contains(word); });
}

// The nodes within `depth` edges of one, and the edges among them.
GraphView within(const GraphView &view, qint64 centre, int depth) {
    QSet<qint64> reached{centre};
    QSet<qint64> frontier{centre};
    for (int walked = 0; walked < depth && !frontier.isEmpty(); ++walked) {
        QSet<qint64> next;
        for (const GraphEdge &edge : view.edges) {
            if (frontier.contains(edge.source) && !reached.contains(edge.target))
                next << edge.target;
            if (frontier.contains(edge.target) && !reached.contains(edge.source))
                next << edge.source;
        }
        reached += next;
        frontier = next;
    }
    GraphView near;
    for (const GraphNode &node : view.nodes)
        if (reached.contains(node.id))
            near.nodes << node;
    for (const GraphEdge &edge : view.edges)
        if (reached.contains(edge.source) && reached.contains(edge.target))
            near.edges << edge;
    return near;
}

} // namespace

GraphFeed *GraphFeed::instance() {
    // One for as long as the program runs, and never taken down: whatever
    // still writes to the graph while the program quits can say so safely.
    static GraphFeed *feed = [] {
        auto *made = new GraphFeed;
        // It may first be asked for by a worker; the views are on the GUI thread.
        if (QCoreApplication::instance())
            made->moveToThread(QCoreApplication::instance()->thread());
        return made;
    }();
    return feed;
}

void GraphFeed::announce() { QMetaObject::invokeMethod(instance(), &GraphFeed::changed, Qt::QueuedConnection); }

QStringList GraphBrowser::knownKinds() {
    return {"character", "group", "place", "item", "concept", "event", "thread", "theme", "topic", "question",
            "book", "author", "series", "tag"};
}

GraphBrowser::GraphBrowser(QObject *parent) : QObject(parent) {
    // The picture settles a few milliseconds at a time between frames.
    m_settle.setInterval(16);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        const bool more = m_layout.run(6);
        emit moved();
        if (!more) {
            m_settle.stop();
            emit settlingChanged();
        }
    });
    // Several things changing at once are read once.
    m_refreshSoon.setSingleShot(true);
    // While the graph is being written (a book read in, a story planned)
    // it changes many times a second; the view looks again once it settles.
    m_refreshSoon.setInterval(400);
    connect(&m_refreshSoon, &QTimer::timeout, this, &GraphBrowser::refresh);
    connect(GraphFeed::instance(), &GraphFeed::changed, &m_refreshSoon, qOverload<>(&QTimer::start));
}

QObject *GraphBrowser::backend() const { return m_backend; }

void GraphBrowser::setBackend(QObject *backend) {
    auto *given = qobject_cast<Backend *>(backend);
    if (given == m_backend)
        return;
    if (m_backend)
        m_backend->disconnect(this);
    m_backend = given;
    if (m_backend) {
        // Another program (the command line, an agent) may have written to
        // the library; the backend notices and says so.
        connect(m_backend, &Backend::revisionChanged, &m_refreshSoon, qOverload<>(&QTimer::start));
        // Another library is another graph altogether.
        connect(m_backend, &Backend::libraryChanged, this, [this] { showScope({}); });
        connect(m_backend, &QObject::destroyed, this, [this] { m_backend = nullptr; });
    }
    emit backendChanged();
    load(false);
}

void GraphBrowser::setLibrary(const Library *library) {
    m_library = library;
    load(false);
}

const Library *GraphBrowser::library() const { return m_backend ? m_backend->library() : m_library; }

std::optional<GraphPlace> GraphBrowser::placeIn(const Book &book) const {
    // A book read to its end, or never opened here (read elsewhere, or its
    // graph built whole on request), has no page here to stop at: what is
    // known of it is shown.
    if (book.status() == "read" || book.percent <= 0)
        return std::nullopt;
    // As far as it was ever read: going back to an earlier page doesn't
    // take away what is known already.
    return GraphContext::furthest(*library(), book.id);
}

void GraphBrowser::showWhole() {
    if (m_what != What::Book || !m_upTo)
        return;
    m_upTo.reset();
    load(false);
    emit reframed();
}

QVariantMap GraphBrowser::spoilersAfter() const {
    const auto bound = m_what == What::Book && library() ? GraphContext::spoilersAfter(*library(), m_bookId) : std::nullopt;
    if (!bound)
        return {};
    return {{"bookId", bound->bookId}, {"section", bound->section}, {"fraction", bound->fraction}};
}

void GraphBrowser::showAhead(qint64 node, bool shown) {
    const QString key = openedKey(library(), node);
    if (shown == opened().contains(key))
        return;
    if (shown)
        opened().insert(key);
    else
        opened().remove(key);
    describe();
    emit selectedChanged();
}

QList<GraphNode> GraphBrowser::unspoiled(QList<GraphNode> nodes) const {
    if (!library() || nodes.isEmpty())
        return nodes;
    const Graph graph(*library());
    const auto facts = graph.factsOf(nodes | std::views::transform(&GraphNode::id) | std::ranges::to<QList<qint64>>());
    const GraphKnown known = GraphContext::unspoiled(*library());
    for (GraphNode &node : nodes)
        node = Graph::asKnown(std::move(node), facts.value(node.id), m_what == What::Book ? m_bookId : 0, known);
    return nodes;
}

GraphView GraphBrowser::allowed() const {
    const Library *books = library();
    if (!books)
        return {};
    const Graph graph(*books);
    if (m_what == What::Scope && m_onlyAcross && m_scope.isEmpty() && !(m_centre > 0 && m_depth > 0)) {
        // The connections between books, each thing with the books it is in.
        GraphView view = graph.acrossBooks(300);
        QHash<qint64, GraphNode> bookNodes;
        for (const GraphNode &node : graph.find({}, "book", QString(""), 100000))
            bookNodes.insert(node.bookId, node);
        QSet<qint64> added;
        const QList<GraphNode> things = view.nodes;
        qint64 made = 0;
        for (const GraphNode &thing : things)
            for (const GraphPlace &place : graph.seen(thing.id)) {
                const auto book = bookNodes.constFind(place.bookId);
                if (thing.kind == "book" || book == bookNodes.constEnd())
                    continue;
                if (!added.contains(book->id) && !std::ranges::contains(things, book->id, &GraphNode::id)) {
                    added << book->id;
                    view.nodes << *book;
                }
                // Not an edge the storage holds: only where the thing is from.
                GraphEdge in;
                in.id = --made;
                in.source = thing.id;
                in.target = book->id;
                in.kind = "appears in";
                view.edges << in;
            }
        return view;
    }
    if (m_what == What::Scope)
        return m_centre > 0 && m_depth > 0 ? graph.around(m_centre, m_depth, 400) : graph.whole(m_scope, 2000);
    if (m_what == What::Book)
        return graph.ofBook(m_bookId, m_upTo);
    // A series is its books' graphs laid over each other.
    GraphView all;
    QSet<qint64> nodes, edges;
    for (const Book &book : books->books()) {
        if (book.meta.series.compare(m_series, Qt::CaseInsensitive) != 0)
            continue;
        const GraphView one = graph.ofBook(book.id, placeIn(book));
        for (const GraphNode &node : one.nodes)
            if (!nodes.contains(node.id)) {
                nodes << node.id;
                all.nodes << node;
            }
        for (const GraphEdge &edge : one.edges)
            if (!edges.contains(edge.id)) {
                edges << edge.id;
                all.edges << edge;
            }
    }
    return all;
}

void GraphBrowser::show(What what) {
    m_what = what;
    m_centre = 0;
    m_depth = 0;
    m_history.clear();
    m_selected = 0;
    m_bookTitle.clear();
    if (what == What::Book && library())
        if (const auto book = library()->book(m_bookId))
            m_bookTitle = book->meta.title;
    load(false);
    emit reframed();
}

void GraphBrowser::showScope(const QString &scope) {
    m_scope = scope;
    m_upTo.reset();
    // The library's own part of the general graph is brought up to date
    // when it is looked at: its books, authors, series and tags.
    if (scope.isEmpty() && library())
        Graph(*library()).syncLibrary();
    show(What::Scope);
}

void GraphBrowser::showBook(qint64 bookId) {
    m_bookId = bookId;
    m_scope.clear();
    m_upTo.reset();
    if (library())
        if (const auto book = library()->book(bookId))
            m_upTo = placeIn(*book);
    show(What::Book);
}

void GraphBrowser::showBookUpTo(qint64 bookId, int section, double fraction) {
    const GraphPlace place{bookId, section, fraction};
    // Reading on only lets more in: what is shown stays where it is.
    if (m_what == What::Book && m_bookId == bookId && m_upTo) {
        if (m_upTo->section != section || m_upTo->fraction != fraction) {
            m_upTo = place;
            load(true);
        }
        return;
    }
    m_bookId = bookId;
    m_scope.clear();
    m_upTo = place;
    show(What::Book);
}

void GraphBrowser::showSeries(const QString &series) {
    m_series = series;
    m_scope.clear();
    m_upTo.reset();
    show(What::Series);
}

void GraphBrowser::refresh() { load(true); }

void GraphBrowser::load(bool keepPlaces) {
    // A picture still finding its shape isn't one to hold still.
    const bool wasSettlingInside = settling();
    QList<qint64> nodesBefore, edgesBefore;
    QHash<qint64, QPointF> placesBefore;
    for (int row = 0; row < m_shown.size(); ++row) {
        nodesBefore << m_shown.at(row).node.id;
        if (keepPlaces && row < m_layout.positions().size())
            placesBefore.insert(m_shown.at(row).node.id, m_layout.positions().at(row));
    }
    for (const Link &link : std::as_const(m_links))
        edgesBefore << link.edge.id;

    m_base = allowed();
    // A centre that is gone (removed, merged away) is no centre.
    if (m_centre > 0 && !std::ranges::contains(m_base.nodes, m_centre, &GraphNode::id)) {
        m_centre = 0;
        m_base = allowed();
    }
    // A book cut at its reader's place is as it was known there already;
    // anything shown whole says of each thing only what its reader has
    // reached, and keeps the rest for the details to offer.
    if (!bounded())
        m_base.nodes = unspoiled(m_base.nodes);
    // What the book connects to in other books, as far as it was read.
    m_outer = m_what == What::Book && library() ? Graph(*library()).beyond(m_bookId, m_upTo) : GraphView();
    m_outer.nodes = unspoiled(m_outer.nodes);
    m_beyondCount = int(m_outer.nodes.size());
    m_knownCount = int(m_base.nodes.size());
    // What is known of the book past the place shown, so the view can say
    // so rather than look as if nothing were known.
    m_hiddenCount = bounded() && library() ? qMax(0, int(Graph(*library()).ofBook(m_bookId).nodes.size()) - m_knownCount) : 0;
    m_storied = std::ranges::any_of(m_base.nodes, std::not_fn(ofTheLibrary), &GraphNode::kind);

    GraphView view = m_base;
    // A scope is cut around its centre by the storage; a book or a series
    // is cut here, out of what its reader may see.
    if (m_what != What::Scope && m_centre > 0 && m_depth > 0)
        view = within(view, m_centre, m_depth);
    if (m_onlyAsked) {
        // The questions, and what each was asked about.
        QSet<qint64> asked;
        for (const GraphNode &node : std::as_const(view.nodes))
            if (node.kind == "question")
                asked << node.id;
        QSet<qint64> kept = asked;
        for (const GraphEdge &edge : std::as_const(view.edges)) {
            if (asked.contains(edge.source))
                kept << edge.target;
            if (asked.contains(edge.target))
                kept << edge.source;
        }
        view.nodes.removeIf([&](const GraphNode &node) { return !kept.contains(node.id); });
    }
    // The kinds are counted before any is left out, so one that is can be
    // put back.
    QHash<QString, int> counts;
    for (const GraphNode &node : std::as_const(view.nodes))
        ++counts[node.kind];
    QStringList kinds = counts.keys();
    std::ranges::sort(kinds, {}, [](const QString &kind) { return std::pair(rankOf(kind), kind); });
    m_kinds.clear();
    for (const QString &kind : std::as_const(kinds))
        m_kinds << QVariantMap{{"kind", kind}, {"count", counts.value(kind)}, {"shown", !m_hidden.contains(kind)}};
    view.nodes.removeIf([&](const GraphNode &node) { return m_hidden.contains(node.kind); });

    // The ring: of what lies beyond, the things tied to something shown.
    QSet<qint64> own, outer;
    for (const GraphNode &node : std::as_const(view.nodes))
        own << node.id;
    QList<qint64> ownNodes = own.values(), ownEdges;
    std::ranges::sort(ownNodes);
    for (const GraphEdge &edge : std::as_const(view.edges))
        if (own.contains(edge.source) && own.contains(edge.target))
            ownEdges << edge.id;
    const bool sameInside = keepPlaces && ownNodes == m_ownNodes && ownEdges == m_ownEdges;
    m_ownNodes = ownNodes;
    m_ownEdges = ownEdges;
    if (m_beyond) {
        QHash<qint64, GraphNode> beyond;
        for (const GraphNode &node : std::as_const(m_outer.nodes))
            beyond.insert(node.id, node);
        for (const GraphEdge &edge : std::as_const(m_outer.edges)) {
            const qint64 far = own.contains(edge.source) ? edge.target : own.contains(edge.target) ? edge.source : 0;
            if (far == 0 || own.contains(far) || !beyond.contains(far))
                continue;
            view.edges << edge;
            if (!outer.contains(far)) {
                outer << far;
                view.nodes << beyond.value(far);
            }
        }
    }

    std::ranges::sort(view.nodes, {}, &GraphNode::id);
    m_shown.clear();
    m_index.clear();
    for (const GraphNode &node : std::as_const(view.nodes)) {
        m_index.insert(node.id, int(m_shown.size()));
        Shown shown;
        shown.node = node;
        if (outer.contains(node.id)) {
            shown.outer = true;
            const auto from = bookOf(node);
            shown.fromBook = from.first;
            shown.from = from.second;
        }
        m_shown.append(shown);
    }
    m_links.clear();
    m_beside = QList<QList<int>>(m_shown.size());
    QList<QPair<int, int>> pairs;
    for (const GraphEdge &edge : std::as_const(view.edges)) {
        const int from = m_index.value(edge.source, -1), to = m_index.value(edge.target, -1);
        if (from < 0 || to < 0)
            continue;
        m_links.append({from, to, edge});
        pairs.append({from, to});
        ++m_shown[from].links;
        ++m_shown[to].links;
        if (!m_beside.at(from).contains(to)) {
            m_beside[from] << to;
            m_beside[to] << from;
        }
    }

    // The rows: the book's own by kind, then what is in other books.
    QList<GraphNode> ordered = view.nodes;
    std::ranges::sort(ordered, [&outer](const GraphNode &a, const GraphNode &b) {
        return outer.contains(a.id) != outer.contains(b.id) ? outer.contains(b.id) : before(a, b);
    });
    m_rows.clear();
    m_order.clear();
    for (const GraphNode &node : std::as_const(ordered)) {
        const Shown &shown = m_shown.at(m_index.value(node.id));
        m_order << node.id;
        m_rows << QVariantMap{{"id", node.id}, {"kind", node.kind}, {"name", node.name}, {"summary", node.summary},
                              {"links", shown.links}, {"outer", shown.outer}, {"from", shown.from}};
    }

    // The same nodes and edges as before are the same picture: nothing moves.
    QList<qint64> nodesNow, edgesNow;
    for (const Shown &shown : std::as_const(m_shown))
        nodesNow << shown.node.id;
    for (const Link &link : std::as_const(m_links))
        edgesNow << link.edge.id;
    if (!keepPlaces || nodesNow != nodesBefore || edgesNow != edgesBefore) {
        QList<QPointF> known(m_shown.size());
        QList<bool> placed(m_shown.size(), false), ring(m_shown.size(), false), fixed(m_shown.size(), false);
        for (int row = 0; row < m_shown.size(); ++row) {
            const auto was = placesBefore.constFind(m_shown.at(row).node.id);
            if (was != placesBefore.constEnd()) {
                known[row] = *was;
                placed[row] = true;
            }
            ring[row] = m_shown.at(row).outer;
            // Only the ring changed: the book's own picture stays as it is.
            fixed[row] = sameInside && !m_shown.at(row).outer && !wasSettlingInside;
        }
        const bool wasSettling = settling();
        m_layout.start(int(m_shown.size()), pairs, known, placed, m_aspect, ring, fixed);
        // A small graph is laid out before it is first seen; a large one
        // goes on settling between frames.
        if (m_layout.run(m_shown.size() <= 300 ? 24 : 4))
            m_settle.start();
        else
            m_settle.stop();
        if (wasSettling != settling())
            emit settlingChanged();
    }

    if (m_selected > 0 && !m_index.contains(m_selected))
        m_selected = 0;
    describe();
    emit changed();
    emit selectedChanged();
    emit moved();
}

void GraphBrowser::setAspect(double aspect) {
    // In a few steps, so that dragging a window's edge doesn't lay the
    // picture out again with every pixel.
    aspect = aspect > 1.25 ? 1.6 : aspect < 0.85 ? 0.7 : 1;
    if (aspect == m_aspect)
        return;
    m_aspect = aspect;
    emit aspectChanged();
    load(false);
    emit reframed();
}

void GraphBrowser::settleNow() {
    m_layout.settle();
    if (m_settle.isActive()) {
        m_settle.stop();
        emit settlingChanged();
    }
    emit moved();
}

QVariantList GraphBrowser::scopes() const {
    QVariantList list{QVariantMap{{"scope", QString("")}}};
    if (library())
        for (const QString &scope : Graph(*library()).scopes())
            if (!scope.isEmpty())
                list << QVariantMap{{"scope", scope}};
    return list;
}

QVariantList GraphBrowser::books() const {
    QVariantList list;
    if (!library())
        return list;
    const Graph graph(*library());
    for (const Book &book : library()->books()) {
        const int things = int(graph.ofBook(book.id, placeIn(book)).nodes.size());
        if (things > 0)
            list << QVariantMap{{"bookId", book.id}, {"title", book.meta.title}, {"series", book.meta.series}, {"things", things}};
    }
    return list;
}

void GraphBrowser::setOnlyAsked(bool only) {
    if (only == m_onlyAsked)
        return;
    m_onlyAsked = only;
    load(true);
}

void GraphBrowser::setBeyond(bool beyond) {
    if (beyond == m_beyond)
        return;
    m_beyond = beyond;
    load(true);
    emit reframed();
}

void GraphBrowser::setOnlyAcross(bool only) {
    if (only == m_onlyAcross)
        return;
    m_onlyAcross = only;
    load(false);
    emit reframed();
}

QPair<qint64, QString> GraphBrowser::bookOf(const GraphNode &node) const {
    if (!library() || node.kind == "book")
        return {};
    for (const GraphPlace &place : Graph(*library()).seen(node.id))
        if (place.bookId != (m_what == What::Book ? m_bookId : 0))
            if (const auto book = library()->book(place.bookId))
                return {book->id, book->meta.title};
    return {};
}

GraphBrowser::Visit GraphBrowser::here() const {
    return {m_centre, m_depth, m_selected, m_what, m_scope, m_series, m_bookTitle, m_bookId, m_upTo};
}

void GraphBrowser::setDepth(int depth) {
    depth = std::clamp(depth, 0, 4);
    if (depth == m_depth)
        return;
    m_depth = depth;
    // Something has to be in the middle: the selected node, or failing
    // that the best connected one.
    if (m_depth > 0 && m_centre == 0) {
        m_centre = m_selected;
        if (m_centre == 0 && !m_shown.isEmpty())
            m_centre = std::ranges::max_element(m_shown, {}, &Shown::links)->node.id;
    }
    load(true);
    emit reframed();
}

void GraphBrowser::setKindShown(const QString &kind, bool shown) {
    if (shown == !m_hidden.contains(kind))
        return;
    if (shown)
        m_hidden.remove(kind);
    else
        m_hidden.insert(kind);
    load(true);
}

void GraphBrowser::showEveryKind() {
    if (m_hidden.isEmpty() && !m_onlyAsked)
        return;
    m_hidden.clear();
    m_onlyAsked = false;
    load(true);
}

void GraphBrowser::select(qint64 node) {
    if (node == m_selected || (node != 0 && !m_index.contains(node)))
        return;
    m_selected = node;
    describe();
    emit selectedChanged();
}

void GraphBrowser::jumpTo(qint64 node) {
    if (node <= 0 || !library())
        return;
    if (!m_index.contains(node)) {
        const auto found = Graph(*library()).node(node);
        if (!found)
            return;
        // A book's graph never shows what its reader hasn't met.
        const bool known = std::ranges::contains(m_base.nodes, node, &GraphNode::id);
        if (m_what != What::Scope && !known)
            return;
        m_hidden.remove(found->kind);
        m_onlyAsked = false;
        load(true);
        if (!m_index.contains(node)) {
            m_history.append(here());
            m_centre = node;
            m_depth = std::max(1, m_depth);
            load(true);
            emit reframed();
        }
    }
    select(node);
}

void GraphBrowser::step(const QString &direction) {
    if (m_shown.isEmpty())
        return;
    const int from = rowOf(m_selected);
    if (from < 0) {
        // Nothing selected yet: the centre, or the best connected node (the
        // first of them, when several are).
        const int centre = rowOf(m_centre);
        select(centre >= 0 ? m_shown.at(centre).node.id : std::ranges::max_element(m_shown, {}, &Shown::links)->node.id);
        return;
    }
    const QPointF way = direction == "left" ? QPointF(-1, 0) : direction == "right" ? QPointF(1, 0)
                        : direction == "up" ? QPointF(0, -1) : QPointF(0, 1);
    const QList<QPointF> &at = positions();
    // How far a node is that way, counting what is off to the side double;
    // nothing for a node more than sixty degrees off.
    const auto cost = [&](int row) {
        const QPointF to = at.at(row) - at.at(from);
        const double along = to.x() * way.x() + to.y() * way.y();
        const double across = std::abs(to.x() * way.y() - to.y() * way.x());
        return along <= 0 || across > along * 1.75 ? -1.0 : along + across * 2;
    };
    int best = -1;
    double least = 0;
    const auto consider = [&](int row) {
        const double costs = row == from ? -1 : cost(row);
        if (costs >= 0 && (best < 0 || costs < least)) {
            best = row;
            least = costs;
        }
    };
    for (const int row : beside(from))
        consider(row);
    // No edge leads that way: the nearest node there, so no part of the
    // picture is out of the keyboard's reach.
    for (int row = 0; best < 0 && row < m_shown.size(); ++row)
        consider(row);
    if (best >= 0)
        select(m_shown.at(best).node.id);
}

void GraphBrowser::advance(int by) {
    if (m_order.isEmpty())
        return;
    const qsizetype count = m_order.size();
    const qsizetype at = m_order.indexOf(m_selected);
    select(m_order.at(at < 0 ? (by > 0 ? 0 : count - 1) : ((at + by) % count + count) % count));
}

void GraphBrowser::recentre() {
    if (m_selected == 0 || (m_selected == m_centre && m_depth > 0))
        return;
    const int row = rowOf(m_selected);
    const Shown chosen = row >= 0 ? m_shown.at(row) : Shown();
    m_history.append(here());
    if (chosen.outer) {
        // A thing of another book: on to that book's graph, around it. One
        // that is in no book is looked at in the graph of everything read.
        const qint64 node = m_selected;
        const qint64 book = chosen.node.kind == "book" ? chosen.node.bookId : chosen.fromBook;
        const auto other = book > 0 && library() ? library()->book(book) : std::nullopt;
        m_what = other ? What::Book : What::Scope;
        m_scope.clear();
        m_bookId = other ? other->id : 0;
        m_bookTitle = other ? other->meta.title : QString();
        m_upTo = other ? placeIn(*other) : std::nullopt;
        m_centre = chosen.node.kind == "book" && other ? 0 : node;
        m_depth = m_centre > 0 ? 1 : 0;
        m_selected = 0;
        load(false);
        select(node);
        emit reframed();
        return;
    }
    m_centre = m_selected;
    m_depth = std::max(1, m_depth);
    load(true);
    emit reframed();
}

bool GraphBrowser::back() {
    if (m_history.isEmpty())
        return false;
    const Visit visit = m_history.takeLast();
    // Back in the same graph the picture keeps its places; another graph is laid out anew.
    const bool same = visit.what == m_what && visit.bookId == m_bookId && visit.scope == m_scope && visit.series == m_series;
    m_what = visit.what;
    m_scope = visit.scope;
    m_series = visit.series;
    m_bookId = visit.bookId;
    m_bookTitle = visit.bookTitle;
    m_upTo = visit.upTo;
    m_centre = visit.centre;
    m_depth = visit.depth;
    load(same);
    select(visit.selected);
    emit reframed();
    return true;
}

QVariantList GraphBrowser::find(const QString &typed) const {
    QVariantList found;
    if (!library())
        return found;
    const QStringList words = typed.toLower().split(' ', Qt::SkipEmptyParts);
    QList<GraphNode> nodes;
    // A scope is searched whole, past what the picture holds; a book only
    // in what its reader has met.
    if (m_what == What::Scope && !words.isEmpty()) {
        // Found only by what may be shown: a name revealed past a reader's
        // place finds nothing, so neither the list nor its count gives it away.
        nodes = unspoiled(Graph(*library()).find(words.join(' '), {}, m_scope, 200));
        nodes.removeIf([&](const GraphNode &node) { return !goesBy(node, words); });
    } else {
        for (const GraphNode &node : m_base.nodes)
            if (goesBy(node, words))
                nodes << node;
    }
    const QString whole = typed.trimmed().toLower();
    // What was typed in full leads, then what starts with it.
    const auto rank = [&](const GraphNode &node) {
        return node.name.toLower() == whole ? 0 : node.name.toLower().startsWith(whole) ? 1 : 2;
    };
    std::ranges::stable_sort(nodes, [&rank](const GraphNode &a, const GraphNode &b) {
        return rank(a) != rank(b) ? rank(a) < rank(b) : QString::localeAwareCompare(a.name.toLower(), b.name.toLower()) < 0;
    });
    for (const GraphNode &node : nodes.mid(0, 40))
        found << QVariantMap{{"id", node.id}, {"kind", node.kind}, {"name", node.name}, {"summary", node.summary}};
    return found;
}

bool GraphBrowser::edit(qint64 node, const QString &name, const QString &summary) {
    if (!library() || !Graph(*library()).edit(node, name, summary))
        return false;
    load(true);
    GraphFeed::announce();
    return true;
}

bool GraphBrowser::merge(qint64 keep, qint64 other) {
    if (!library() || !Graph(*library()).merge(keep, other))
        return false;
    load(true);
    select(keep);
    GraphFeed::announce();
    return true;
}

bool GraphBrowser::remove(qint64 node) {
    if (!library() || !Graph(*library()).remove(node))
        return false;
    load(true);
    GraphFeed::announce();
    return true;
}

bool GraphBrowser::unlink(qint64 edge) {
    if (!library() || !Graph(*library()).unlink(edge))
        return false;
    load(true);
    GraphFeed::announce();
    return true;
}

qint64 GraphBrowser::add(const QString &kind, const QString &name, const QString &summary, const QString &relation) {
    if (!library())
        return 0;
    Graph graph(*library());
    // A thing added to a book's graph was met in that book, where its
    // reader is now, so it is part of that graph and of no place ahead.
    std::optional<GraphPlace> place;
    if (m_what == What::Book) {
        const Progress progress = library()->progress(m_bookId);
        place = m_upTo ? *m_upTo : GraphPlace{m_bookId, progress.section, progress.fraction};
    }
    const qint64 made = graph.upsert(m_what == What::Scope ? m_scope : QString(), kind, name, summary, {}, place);
    if (made == 0)
        return 0;
    if (m_selected > 0 && m_selected != made && !relation.trimmed().isEmpty())
        graph.link(m_selected, made, relation, {}, place);
    m_hidden.remove(kind.simplified().toLower());
    load(true);
    jumpTo(made);
    GraphFeed::announce();
    return made;
}

void GraphBrowser::describe() {
    m_details.clear();
    const int row = rowOf(m_selected);
    if (row < 0 || !library())
        return;
    const Library *books = library();
    const Graph graph(*books);
    const GraphNode &node = m_shown.at(row).node;
    m_details = {{"id", node.id}, {"kind", node.kind}, {"name", node.name}, {"summary", node.summary},
                 {"aliases", node.aliases()}, {"scope", node.scope}, {"bookId", node.bookId},
                 {"answer", node.data.value("answer").toString()}, {"url", node.data.value("url").toString()},
                 {"outer", m_shown.at(row).outer}, {"fromBook", m_shown.at(row).fromBook}, {"from", m_shown.at(row).from}};

    // What a book's reader has not reached is kept apart, as "ahead".
    const GraphKnown known = GraphContext::unspoiled(*books);
    QHash<qint64, std::optional<Book>> bookCache;
    const auto bookOfId = [&](qint64 id) -> const std::optional<Book> & {
        if (!bookCache.contains(id))
            bookCache.insert(id, books->book(id));
        return *bookCache.constFind(id);
    };
    const auto momentMap = [&](const GraphMoment &moment) {
        const auto &book = bookOfId(moment.place.bookId);
        return QVariantMap{{"bookId", moment.place.bookId}, {"title", book ? book->meta.title : QString()},
                           {"section", moment.place.section}, {"fraction", moment.place.fraction}, {"anchor", moment.anchor},
                           {"readable", book && !book->formats.isEmpty()}};
    };

    // The books it is in, each with where it was first met there.
    QVariantList places;
    bool metHere = false, metAhead = false;
    for (const GraphPlace &place : graph.seen(node.id)) {
        const auto &book = bookOfId(place.bookId);
        // Of the book being read, only what is behind the reader.
        if (!book || (m_upTo && place.bookId == m_upTo->bookId && !place.notAfter(*m_upTo)))
            continue;
        if (!known(place)) {
            metAhead = true;
            continue;
        }
        metHere = true;
        places << momentMap(graph.firstMet(node.id, place.bookId).value_or(GraphMoment{place, {}}));
    }
    m_details.insert("places", places);
    // Met only past where the reader is: everything said of it is ahead.
    m_details.insert("nodeAhead", metAhead && !metHere);

    // When what is known of it was revealed: each summary it had and each
    // name it was given, at its moment; and what lies ahead, apart.
    QVariantList timeline, ahead;
    const qint64 within = m_what == What::Book ? m_bookId : 0;
    for (const GraphReveal &step : graph.timeline(node.id, within)) {
        // Of a book cut at a place, nothing past it: not even as a spoiler.
        if (m_upTo && step.revealed.place.bookId == m_upTo->bookId && !step.revealed.place.notAfter(*m_upTo) && known(step.revealed.place))
            continue;
        QVariantMap entry = momentMap(step.revealed);
        entry.insert("what", step.what);
        entry.insert("text", step.text);
        entry.insert("other", step.other);
        entry.insert("otherName", step.otherName);
        entry.insert("outgoing", step.outgoing);
        if (!known(step.revealed.place)) {
            const auto bound = GraphContext::spoilersAfter(*books, step.revealed.place.bookId);
            entry.insert("after", bound ? QVariantMap{{"bookId", bound->bookId}, {"section", bound->section}} : QVariantMap());
            ahead << entry;
        } else if (step.what == "summary" || step.what == "alias") {
            timeline << entry;
        }
    }
    m_details.insert("timeline", timeline);
    m_details.insert("aheadCount", ahead.size());
    // Its words only once asked for, for this node.
    const bool open = opened().contains(openedKey(books, node.id));
    m_details.insert("aheadShown", open && !ahead.isEmpty());
    m_details.insert("ahead", open ? ahead : QVariantList());
    m_details.insert("aheadAfter", ahead.isEmpty() ? QVariantMap() : ahead.first().toMap().value("after").toMap());

    // How it relates to other things. A scope knows all of a node's
    // edges; a book only those its reader has learned.
    GraphView near = m_what == What::Scope ? graph.around(node.id, 1, 400) : m_base;
    // And, of a book, what its things connect to in other books.
    QSet<qint64> far;
    if (m_what == What::Book) {
        for (const GraphNode &other : std::as_const(m_outer.nodes))
            far << other.id;
        near.nodes += m_outer.nodes;
        near.edges += m_outer.edges;
    }
    QHash<qint64, const GraphNode *> byId;
    for (const GraphNode &other : near.nodes)
        byId.insert(other.id, &other);
    QVariantList relations;
    for (const GraphEdge &edge : near.edges) {
        if (edge.source != node.id && edge.target != node.id)
            continue;
        // A relation learned past the reader's place is one of the things ahead.
        if (edge.learned.bookId > 0 && !known(edge.learned))
            continue;
        const bool outgoing = edge.source == node.id;
        const GraphNode *other = byId.value(outgoing ? edge.target : edge.source);
        if (!other)
            continue;
        // "echoes Tide-binding, in The Salt Clock": a connection across
        // books says which book its other end is from.
        const bool elsewhere = far.contains(other->id) || (m_what == What::Scope && edge.across());
        relations << QVariantMap{{"edge", edge.id}, {"relation", edge.relation()}, {"note", edge.note}, {"outgoing", outgoing},
                                 {"bookId", edge.learned.bookId}, {"section", edge.learned.section}, {"fraction", edge.learned.fraction},
                                 {"anchor", edge.anchor},
                                 {"other", other->id}, {"otherName", other->name}, {"otherKind", other->kind},
                                 {"across", edge.across()}, {"otherFrom", elsewhere ? bookOf(*other).second : QString()}};
    }
    m_details.insert("relations", relations);

    if (node.kind == "book" && node.bookId > 0) {
        if (const auto book = books->book(node.bookId)) {
            m_details.insert("status", book->status());
            m_details.insert("rating", book->rating);
            m_details.insert("percent", book->percent);
            m_details.insert("finished", book->finished);
            m_details.insert("authors", book->meta.authors.join(", "));
            m_details.insert("readable", !book->formats.isEmpty());
        }
        QVariantList days;
        for (const ReadingDay &day : graph.readingLog(node.bookId))
            days << QVariantMap{{"day", day.day}, {"from", day.from}, {"to", day.to}};
        m_details.insert("days", days);
    }
}

void registerGraphTypes() {
    // Once, however many backends are made; a static's first use is the
    // one that runs it, whichever thread gets there.
    [[maybe_unused]] static const bool registered = [] {
        qmlRegisterType<GraphBrowser>("Omabook", 1, 0, "GraphBrowser");
        qmlRegisterType<GraphCanvas>("Omabook", 1, 0, "GraphCanvas");
        qmlRegisterType<GraphMark>("Omabook", 1, 0, "GraphMark");
        return true;
    }();
}
