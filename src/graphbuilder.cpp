#include "graphbuilder.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>
#include <ranges>

#include "chronology.h"
#include "graphcontext.h"
#include "graphlinks.h"
#include "prompts.h"

namespace {


// The marker's text: a note with any other text is no marker.
const QString builtMark = "built";

// How much of the known graph goes with one request.
constexpr int mostDetailed = 40, mostNamed = 120, mostThreads = 10, mostEarlierBooks = 6;
constexpr qsizetype summaryRoom = 220;
// How much of the other books' part of the general graph goes with it.
constexpr int mostShared = 30, mostNamedElsewhere = 10, mostWeighed = 400;
constexpr qsizetype elsewhereRoom = 120;

// What a book's graph is drawn from: the things known of it and of the
// earlier books of its series, findable by any of their names.
struct Scene {
    QList<GraphNode> nodes;
    QMultiHash<QString, int> byName; // a name or alias, lower case, to its nodes
    QSet<qint64> books;         // this book and the others of its series
    QString qualifier;          // the series, or the title of a book in none

    const GraphNode *find(const QString &name, const QString &kind = {}) const {
        const QString key = name.simplified().toLower();
        // A place and a person can share a name: the one of the kind asked
        // for is taken where there is one.
        const GraphNode *any = nullptr;
        const QList<int> called = byName.values(key);
        for (const int index : called) {
            const GraphNode &node = nodes.at(index);
            if (kind.isEmpty() || node.kind == kind)
                return &node;
            any = &node;
        }
        return any;
    }
};

// Names as the graph keeps a thing that shares its name with a thing of an
// unrelated book: "Tom (The Other Series)".
QString qualified(const QString &name, const QString &qualifier) { return name + " (" + qualifier + ")"; }

// With `upTo`, the book's own things as its reader knows them there.
Scene sceneOf(const Library &library, qint64 bookId, const std::optional<GraphPlace> &upTo = std::nullopt) {
    Scene scene;
    const auto book = library.book(bookId);
    if (!book)
        return scene;
    const QString series = book->meta.series.trimmed();
    scene.qualifier = series.isEmpty() ? book->meta.title : series;
    scene.books << bookId;
    if (!series.isEmpty())
        for (const Book &other : library.books())
            if (other.meta.series.trimmed().compare(series, Qt::CaseInsensitive) == 0)
                scene.books << other.id;

    const Graph graph(library);
    QSet<qint64> taken;
    auto take = [&](const GraphView &view) {
        for (const GraphNode &node : view.nodes) {
            if (taken.contains(node.id) || !Prompts::graphKinds().contains(node.kind))
                continue;
            taken << node.id;
            scene.nodes << node;
        }
    };
    // The earlier books first, so the book's own things are the latest met.
    const QList<Book> earlier = library.earlierInSeries(bookId);
    for (const Book &before : earlier.mid(qMax(0, earlier.size() - mostEarlierBooks)))
        take(graph.ofBook(before.id));
    take(graph.ofBook(bookId, upTo));

    const QString suffix = " (" + scene.qualifier.toLower() + ")";
    for (int index = 0; index < scene.nodes.size(); ++index) {
        const GraphNode &node = scene.nodes.at(index);
        QStringList names = QStringList{node.name} + node.aliases();
        for (const QString &name : std::as_const(names)) {
            const QString key = name.simplified().toLower();
            scene.byName.insert(key, index);
            if (key.endsWith(suffix))
                scene.byName.insert(key.left(key.size() - suffix.size()), index);
        }
    }
    return scene;
}

QString line(const GraphNode &node, bool detailed) {
    QString text = node.name + " (" + node.kind;
    if (detailed && !node.aliases().isEmpty())
        text += "; also " + node.aliases().join(", ");
    text += ")";
    if (detailed && !node.summary.isEmpty())
        text += ": " + node.summary.simplified().left(summaryRoom);
    return text;
}

// The JSON object in a model's answer, wherever it put it: bare, fenced, or
// after a remark.
std::optional<QJsonObject> objectIn(const QString &answer) {
    const qsizetype start = answer.indexOf('{'), end = answer.lastIndexOf('}');
    if (start < 0 || end <= start)
        return std::nullopt;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(answer.mid(start, end - start + 1).toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    return document.object();
}

// The kind a model meant: its own words for a kind are taken for ours, and
// anything else is an idea of the book's.
QString kindOf(const QString &said) {
    const QString kind = said.simplified().toLower();
    if (Prompts::graphKinds().contains(kind))
        return kind;
    static const QHash<QString, QString> meant{
        {"person", "character"}, {"people", "character"}, {"creature", "character"}, {"location", "place"},
        {"setting", "place"}, {"world", "place"}, {"organization", "group"}, {"organisation", "group"},
        {"faction", "group"}, {"family", "group"}, {"object", "item"}, {"artifact", "item"}, {"plot", "thread"},
        {"mystery", "thread"}, {"question", "thread"}, {"plotline", "thread"}, {"work", "topic"}, {"author", "topic"},
        {"reference", "topic"}, {"book", "topic"}, {"motif", "theme"},
    };
    return meant.value(kind, "concept");
}

// Whether a kind of thing belongs to its story: two books' characters of
// one name are two people, while a theme or a real-world topic is the same
// thing wherever it is met.
bool storyBound(const QString &kind) { return kind != "theme" && kind != "topic"; }

// The kinds that can be one thing in two unrelated books: what a book is
// about, the real world, and ideas, and the real places and institutions
// among the places and groups. Whether a thing of these kinds IS the same
// is the model's call (its "same_as"), never the name's alone.
const QStringList shareable{"theme", "topic", "concept", "group", "place"};

// The thing of another book a model said this one is the same as: by its
// name as it was listed, of the kind given or another shareable one, and
// met in a book that is not this one or of its series, on this one's side.
std::optional<GraphNode> sharedNode(const Graph &graph, const Scene &scene, qint64 bookId, const QString &kind, const QString &name) {
    QStringList kinds{kind};
    for (const QString &other : shareable)
        if (other != kind)
            kinds << other;
    for (const QString &tried : std::as_const(kinds)) {
        const qint64 id = shareable.contains(tried) ? graph.named({}, tried, name) : 0;
        if (id == 0)
            continue;
        const QList<GraphPlace> met = graph.seen(id);
        if (std::ranges::any_of(met, [&](const GraphPlace &at) {
                return !scene.books.contains(at.bookId) && graph.together(bookId, at.bookId);
            }))
            return graph.node(id);
    }
    return std::nullopt;
}

QStringList stringsOf(const QJsonValue &value, int most) {
    QStringList strings;
    for (const QJsonValue &entry : value.toArray()) {
        const QString text = entry.toString().simplified();
        if (!text.isEmpty() && text.size() <= 120 && !strings.contains(text, Qt::CaseInsensitive) && strings.size() < most)
            strings << text;
    }
    return strings;
}

// The edges of an answer, but for those of order (graphtimes.h): each known
// where the words the model quoted are, and failing those once both ends
// have shown, the later of the two.
int linkEdges(Graph &graph, qint64 bookId, const QList<GraphBuild::Passage> &passages, const QJsonArray &edges,
              const GraphBuild::NodeOf &idOf, const QHash<QString, QStringList> &calledAlso) {
    int linked = 0;
    for (qsizetype index = 0; index < edges.size() && index < GraphBuild::mostEdges; ++index) {
        const QJsonObject edge = edges.at(index).toObject();
        const QString from = edge.value("from").toString().simplified();
        const QString to = edge.value("to").toString().simplified();
        const QString relation = edge.value("relation").toString().simplified().left(60);
        const qint64 source = idOf(from);
        const qint64 target = idOf(to);
        if (source == 0 || target == 0 || relation.isEmpty() || GraphBuild::ofOrder(relation))
            continue;
        const int section = GraphBuild::sectionOf(edge.value("section"));
        const GraphMoment first = GraphBuild::placeOf(bookId, passages, section, QStringList{from} + calledAlso.value(from.toLower()));
        const GraphMoment second = GraphBuild::placeOf(bookId, passages, section, QStringList{to} + calledAlso.value(to.toLower()));
        const GraphMoment learned =
            GraphBuild::revealedAt(bookId, passages, section, edge.value("quote"), first.place.notAfter(second.place) ? second : first);
        if (graph.link(source, target, relation, edge.value("note").toString().simplified().left(300), learned.place, learned.anchor) > 0)
            ++linked;
    }
    return linked;
}

} // namespace

QList<QList<GraphBuild::Passage>> GraphBuild::chunks(const QList<Passage> &passages, qsizetype most) {
    most = qMax<qsizetype>(most, 200);
    // A section longer than a request is cut where a paragraph ends, and
    // failing that between words.
    QList<GraphBuild::Passage> pieces;
    for (const GraphBuild::Passage &passage : passages) {
        qsizetype start = 0;
        const qsizetype length = passage.text.size();
        while (length - start > most) {
            qsizetype cut = passage.text.lastIndexOf('\n', start + most);
            if (cut < start + most / 2)
                cut = passage.text.lastIndexOf(' ', start + most);
            if (cut < start + most / 2)
                cut = start + most;
            pieces.append({passage.section, passage.from + (passage.to - passage.from) * double(start) / length,
                           passage.from + (passage.to - passage.from) * double(cut) / length, passage.text.mid(start, cut - start)});
            start = cut;
        }
        pieces.append({passage.section, passage.from + (passage.to - passage.from) * double(start) / qMax<qsizetype>(1, length),
                       passage.to, passage.text.mid(start)});
    }
    QList<QList<GraphBuild::Passage>> chunks;
    qsizetype filled = 0;
    for (const GraphBuild::Passage &piece : std::as_const(pieces)) {
        if (chunks.isEmpty() || filled + piece.text.size() > most) {
            chunks.append(QList<GraphBuild::Passage>());
            filled = 0;
        }
        chunks.last().append(piece);
        filled += piece.text.size();
    }
    return chunks;
}

std::optional<GraphPlace> GraphBuild::builtTo(const Library &library, qint64 bookId) {
    const Recap marker = library.notes(bookId, "graph");
    if (marker.text != builtMark)
        return std::nullopt;
    return GraphPlace{bookId, marker.place};
}

void GraphBuild::setBuiltTo(Library &library, qint64 bookId, const GraphPlace &place) {
    library.setNotes(bookId, "graph", {.place = place.inBook(), .text = builtMark, .updated = 0});
}

void GraphBuild::forgetBuilt(Library &library, qint64 bookId) { library.setNotes(bookId, "graph", {{}, QString(""), 0}); }

QString GraphBuild::known(const Library &library, qint64 bookId, const QList<Passage> &passages) {
    // As things stood where these passages end: what a whole build of the
    // book once found further on is not the model's to know yet.
    const std::optional<GraphPlace> end = passages.isEmpty() ? std::nullopt
                                                             : std::optional(GraphPlace{bookId, passages.last().section, passages.last().to});
    const Scene scene = sceneOf(library, bookId, end);
    QString text;
    for (const GraphBuild::Passage &passage : passages)
        text += passage.text.toLower() + "\n";
    // The things the text names, and the threads still open (the text may
    // move one on without naming it), go with what is known of them; the
    // rest by name only, the most recently met first, so the model reuses a
    // name rather than making the thing anew.
    QList<const GraphNode *> detailed, named;
    int threads = 0;
    for (const GraphNode &node : std::as_const(scene.nodes) | std::views::reverse) {
        const bool touched = GraphContext::namedIn(node, text, true);
        const bool open = !touched && node.kind == "thread" && threads < mostThreads;
        if (open)
            ++threads;
        if ((touched || open) && detailed.size() < mostDetailed)
            detailed << &node;
        else if (named.size() < mostNamed)
            named << &node;
    }
    QStringList lines;
    for (const GraphNode *node : std::as_const(detailed))
        lines << line(*node, true);
    for (const GraphNode *node : std::as_const(named))
        lines << line(*node, false);
    return lines.join('\n');
}

QString GraphBuild::elsewhere(const Library &library, qint64 bookId, const QList<Passage> &passages) {
    const Scene scene = sceneOf(library, bookId);
    const Graph graph(library);
    QSet<qint64> mine;
    for (const GraphNode &node : scene.nodes)
        mine << node.id;
    QString text;
    for (const GraphBuild::Passage &passage : passages)
        text += passage.text.toLower() + "\n";

    // Weighed by what the text says: a name that is in it counts most, a
    // word of a name less. A theme is seldom named by the text it runs
    // through, so the ones no word points at still stand in line, the most
    // lately met first, for the model to recognise.
    struct Weighed {
        GraphNode node;
        int weight = 0;
    };
    QList<Weighed> ideas, named;
    for (const QString &kind : {QString("theme"), QString("topic"), QString("concept")})
        for (const GraphNode &node : graph.find({}, kind, QString(), mostWeighed))
            if (!mine.contains(node.id))
                ideas.append({node, GraphContext::namedIn(node, text) ? 10 : GraphContext::namedIn(node, text, true) ? 3 : 0});
    for (const QString &kind : {QString("group"), QString("place")})
        for (const GraphNode &node : graph.find({}, kind, QString(), mostWeighed))
            if (!mine.contains(node.id) && GraphContext::namedIn(node, text))
                named.append({node, 10});
    const auto first = [](const Weighed &a, const Weighed &b) {
        return a.weight != b.weight ? a.weight > b.weight : a.node.updated > b.node.updated;
    };
    std::ranges::stable_sort(ideas, first);

    // Only what was met in another book, as far as that book was read, and
    // as it was known there.
    QHash<qint64, GraphContext::Reach> reaches;
    const GraphKnown knows = GraphContext::readerKnows(library);
    QStringList lines;
    const auto take = [&](const QList<Weighed> &candidates, int most) {
        int taken = 0;
        for (const Weighed &candidate : candidates.mid(0, most * 3)) {
            if (taken >= most)
                break;
            for (const GraphPlace &at : graph.seen(candidate.node.id)) {
                // Nor of a book on another side of the library.
                if (scene.books.contains(at.bookId) || !graph.together(bookId, at.bookId))
                    continue;
                if (!reaches.contains(at.bookId))
                    reaches.insert(at.bookId, GraphContext::reachOf(library, at.bookId));
                const auto from = reaches.value(at.bookId).covers(at) ? library.book(at.bookId) : std::nullopt;
                if (!from)
                    continue;
                const GraphNode known = Graph::asKnown(candidate.node, graph.facts(candidate.node.id), 0, knows);
                lines << known.name + " (" + known.kind + "; in " + from->meta.title + ")"
                             + (known.summary.isEmpty() ? QString() : ": " + known.summary.simplified().left(elsewhereRoom));
                ++taken;
                break;
            }
        }
    };
    take(ideas, mostShared);
    take(named, mostNamedElsewhere);
    return lines.join('\n');
}

AiRequest GraphBuild::request(const Library &library, qint64 bookId, const QList<Passage> &passages) {
    QString text;
    for (const GraphBuild::Passage &passage : passages)
        text += QString("<section number=\"%1\">\n").arg(passage.section) + passage.text.trimmed() + "\n</section>\n";
    const auto book = library.book(bookId);
    return Prompts::graphExtraction(book ? book->meta : BookMeta(), known(library, bookId, passages), text,
                                    elsewhere(library, bookId, passages));
}

GraphBuild::Applied GraphBuild::apply(Library &library, qint64 bookId, const QList<Passage> &passages, const QString &answer) {
    Applied applied;
    const auto object = objectIn(answer);
    if (!object || passages.isEmpty() || !library.book(bookId)
        || (!object->value("nodes").isArray() && !object->value("edges").isArray()))
        return applied;
    applied.understood = true;

    // One answer is one write: much faster than a statement at a time, and
    // a window watching the library sees it change once.
    const Library::Batch batch(library);
    Graph graph(library);
    const Scene scene = sceneOf(library, bookId);
    const qint64 bookNode = graph.bookNode(bookId);
    // The things of this answer by the names the model used, for its edges.
    QHash<QString, qint64> named;
    QHash<QString, QStringList> calledAlso;

    const QJsonArray nodes = object->value("nodes").toArray();
    for (qsizetype index = 0; index < nodes.size() && index < mostNodes; ++index) {
        const QJsonObject node = nodes.at(index).toObject();
        const QString name = node.value("name").toString().simplified();
        QString kind = kindOf(node.value("kind").toString());
        // A timeline is made from what the answer says of its events (graphtimes.h).
        if (name.isEmpty() || name.size() > 120 || kind == Chronology::timelineKind())
            continue;
        QStringList aliases = stringsOf(node.value("aliases"), 8);
        const QString summary = node.value("summary").toString().simplified().left(600);
        const int section = sectionOf(node.value("section"));
        // Met where one of its names first shows; what is said of it
        // revealed where the words the model quoted are, and met there at
        // the latest.
        GraphMoment met = placeOf(bookId, passages, section, QStringList{name} + aliases);
        const GraphMoment revealed = revealedAt(bookId, passages, section, node.value("quote"), met);
        if (revealed.place.notAfter(met.place) && revealed.anchor != met.anchor)
            met = revealed;
        const GraphPlace place = met.place;

        // A thing the book already has keeps its name and kind. A new one
        // whose name a thing of an unrelated book has is another thing, and
        // is told apart by its series or book.
        // And one the model says is the same thing as a thing of another
        // book joins that node, so the two books meet in it.
        QString called = name;
        bool joined = false;
        const QString sameAs = node.value("same_as").toString().simplified();
        const GraphNode *existing = scene.find(name, kind);
        for (const QString &alias : std::as_const(aliases))
            if (!existing)
                existing = scene.find(alias, kind);
        if (existing) {
            if (existing->name.compare(name, Qt::CaseInsensitive) != 0)
                aliases << name;
            called = existing->name;
            kind = existing->kind;
        } else if (const auto same = sameAs.isEmpty() ? std::nullopt : sharedNode(graph, scene, bookId, kind, sameAs)) {
            if (same->name.compare(name, Qt::CaseInsensitive) != 0)
                aliases << name;
            called = same->name;
            kind = same->kind;
            joined = true;
        } else if (storyBound(kind)) {
            const qint64 other = graph.named({}, kind, name);
            const QList<GraphPlace> met = other > 0 ? graph.seen(other) : QList<GraphPlace>();
            const bool elsewhere = !met.isEmpty() && std::ranges::none_of(met, [&](const GraphPlace &at) {
                return scene.books.contains(at.bookId);
            });
            if (elsewhere)
                called = qualified(name, scene.qualifier);
        }
        aliases.removeAll(called);

        // What a theme or a topic is, or a thing two books share, stays as
        // it was first put; what this book makes of it is said on its tie
        // to the book.
        const bool shared = joined || !storyBound(kind);
        const qint64 before = shared ? graph.named({}, kind, called) : 0;
        const bool keepSummary = before > 0 && !graph.node(before)->summary.isEmpty();
        const qint64 id = graph.upsert({}, kind, called, keepSummary ? QString() : summary, {}, place, met.anchor, revealed);
        if (id == 0)
            continue;
        // Each other name from where it first shows, which for a true name
        // revealed late is late: a reader before it doesn't know it yet.
        for (const QString &alias : std::as_const(aliases))
            graph.learn(id, "alias", alias, momentOf(bookId, passages, section, {alias}).value_or(revealed));
        ++applied.nodes;
        named.insert(name.toLower(), id);
        calledAlso.insert(name.toLower(), aliases);
        for (const QString &alias : std::as_const(aliases))
            named.insert(alias.toLower(), id);
        // Tied to its book, so the graph of everything read hangs together:
        // a book, what is in it, and through that the books that share it.
        graph.link(id, bookNode, kind == "topic" ? "referred to in" : "appears in", shared ? summary : QString(), place, met.anchor);
    }

    const GraphBuild::NodeOf idOf = [&](const QString &name) {
        if (const qint64 id = named.value(name.toLower()))
            return id;
        const GraphNode *known = scene.find(name);
        return known ? known->id : qint64{0};
    };
    applied.edges = linkEdges(graph, bookId, passages, object->value("edges").toArray(), idOf, calledAlso);
    applied.times = applyTimes(graph, library, bookId, passages, *object, idOf);
    return applied;
}

GraphBuild::Built GraphBuild::build(Library &library, const Document &document, qint64 bookId, const GraphPlace &upTo,
                                    const Asker &ask, const std::function<void(const Built &)> &said) {
    Built built;
    const bool flows = document.kind() == Document::Flow;
    const GraphPlace goal{bookId, qBound(0, upTo.section, qMax(0, document.count() - 1)), flows ? qBound(0.0, upTo.fraction, 1.0) : 0.0};
    const std::optional<GraphPlace> after = builtTo(library, bookId);
    built.upTo = after.value_or(GraphPlace{bookId, 0, 0});
    // Already built this far, or further on an earlier reading.
    if (after && goal.notAfter(*after)) {
        built.complete = true;
        return built;
    }
    for (const QList<GraphBuild::Passage> &chunk : chunks(passages(document, after, goal))) {
        QString answer, error;
        if (!ask(request(library, bookId, chunk), &answer, &error)) {
            built.problems << error;
            return built;
        }
        ++built.requests;
        built.sent += size(chunk);
        const Applied applied = apply(library, bookId, chunk, answer);
        if (!applied.understood)
            built.problems << QString("The model's answer for section %1 was not the JSON asked for, so that part was passed over.")
                                  .arg(chunk.first().section);
        built.nodes += applied.nodes;
        built.edges += applied.edges;
        built.times += applied.times;
        built.upTo = {bookId, chunk.last().section, chunk.last().to};
        setBuiltTo(library, bookId, built.upTo);
        if (said)
            said(built);
    }
    built.upTo = goal;
    setBuiltTo(library, bookId, goal);
    built.complete = true;
    return built;
}

GraphBuilder::GraphBuilder(Factory factory, LibraryNow library, QObject *parent)
    : QObject(parent), m_factory(std::move(factory)), m_library(std::move(library)) {}

void GraphBuilder::grow(qint64 bookId, const QList<GraphBuild::Passage> &passages, int most, bool connect) {
    Library *library = m_library ? m_library() : nullptr;
    if (busy() || !library || passages.isEmpty())
        return;
    m_root = library->root();
    m_bookId = bookId;
    m_chunks = GraphBuild::chunks(passages).mid(0, qMax(1, most));
    m_connect = connect;
    next();
}

void GraphBuilder::next() {
    // The call that just answered is still on the stack below this.
    if (m_call)
        m_call.release()->deleteLater();
    Library *library = m_library ? m_library() : nullptr;
    // The last chunk is in: what this session added is compared with the
    // other books once, not chunk by chunk.
    if (m_chunks.isEmpty() && m_connect && connectNow(library))
        return;
    QString why;
    std::unique_ptr<AiProvider> provider = m_chunks.isEmpty() ? nullptr : m_factory(&why);
    // Another library, or the book gone from it: what was asked is dropped.
    if (!provider || !library || library->root() != m_root || !library->book(m_bookId)) {
        m_chunks.clear();
        emit busyChanged();
        if (!why.isEmpty())
            emit failed(why);
        return;
    }
    const QList<GraphBuild::Passage> chunk = m_chunks.takeFirst();
    m_call = std::make_unique<AiCall>(std::move(provider));
    connect(m_call.get(), &AiCall::done, this, [this, chunk](const QString &answer) {
        Library *now = m_library ? m_library() : nullptr;
        if (now && now->root() == m_root && now->book(m_bookId)) {
            // An answer that can't be read is passed over like any other
            // part with nothing in it: asking again would cost the same.
            const GraphBuild::Applied applied = GraphBuild::apply(*now, m_bookId, chunk, answer);
            GraphBuild::setBuiltTo(*now, m_bookId, {m_bookId, chunk.last().section, chunk.last().to});
            emit grown(m_bookId, applied.nodes, applied.edges);
        } else {
            m_chunks.clear();
        }
        next();
    });
    connect(m_call.get(), &AiCall::failed, this, [this](const QString &error) {
        // Nothing is marked as built, so the same pages go again next time.
        m_chunks.clear();
        m_connect = false;
        next();
        emit failed(error);
    });
    emit busyChanged();
    m_call->ask(GraphBuild::request(*library, m_bookId, chunk));
}

// Asks for the connections across books when there is something new to
// compare; false when there is nothing to ask.
bool GraphBuilder::connectNow(Library *library) {
    m_connect = false;
    if (!library || library->root() != m_root || !library->book(m_bookId) || !GraphLinks::due(*library, m_bookId))
        return false;
    std::unique_ptr<AiProvider> provider = m_factory(nullptr);
    if (!provider)
        return false;
    const GraphPlace at = *GraphBuild::builtTo(*library, m_bookId);
    const GraphLinks::Offer offer = GraphLinks::offer(*library, m_bookId, at);
    m_call = std::make_unique<AiCall>(std::move(provider));
    connect(m_call.get(), &AiCall::done, this, [this, at, offer](const QString &answer) {
        Library *now = m_library ? m_library() : nullptr;
        if (now && now->root() == m_root && now->book(m_bookId)) {
            const GraphLinks::Applied applied = GraphLinks::apply(*now, m_bookId, at, offer, answer);
            GraphLinks::setConnected(*now, m_bookId, at, offer);
            emit grown(m_bookId, applied.topics, applied.links + 2 * applied.topics);
        }
        next();
    });
    // Without an answer nothing is kept, so it is asked again next time.
    connect(m_call.get(), &AiCall::failed, this, [this] { next(); });
    emit busyChanged();
    m_call->ask(GraphLinks::request(*library, m_bookId, offer));
    return true;
}

void GraphBuilder::cancel() {
    m_chunks.clear();
    m_connect = false;
    if (!m_call)
        return;
    m_call->cancel();
    m_call.release()->deleteLater();
    emit busyChanged();
}
