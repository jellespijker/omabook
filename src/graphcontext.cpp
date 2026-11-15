#include "graphcontext.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <memory>

#include "prompts.h"

namespace {

constexpr qsizetype summaryRoom = 220;

QStringList namesOf(const GraphNode &node) { return QStringList{node.name} + node.aliases(); }

QString line(const GraphNode &node) {
    QString text = node.name + " (" + node.kind;
    if (!node.aliases().isEmpty())
        text += "; also " + node.aliases().join(", ");
    text += ")";
    if (!node.summary.isEmpty())
        text += ": " + node.summary.simplified().left(summaryRoom);
    return text;
}

// Things one to a line, then how they relate, as the model reads them.
QString written(const QList<GraphNode> &nodes, const QList<GraphEdge> &edges, int mostEdges) {
    QHash<qint64, QString> names;
    QStringList lines;
    for (const GraphNode &node : nodes) {
        names.insert(node.id, node.name);
        lines << line(node);
    }
    int written = 0;
    for (const GraphEdge &edge : edges) {
        if (!names.contains(edge.source) || !names.contains(edge.target) || written >= mostEdges)
            continue;
        lines << names.value(edge.source) + " — " + edge.kind + " → " + names.value(edge.target)
                     + (edge.note.isEmpty() ? QString() : " (" + edge.note.left(120) + ")");
        ++written;
    }
    return lines.join('\n');
}

GraphView storyOf(const Library &library, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    return GraphContext::story(library, bookId, upTo);
}

// The start of an answer, to its first sentence end past a line or so.
QString gistOf(const QString &answer) {
    const QString text = answer.simplified();
    if (text.size() <= 320)
        return text;
    qsizetype end = -1;
    for (const QChar mark : {QChar('.'), QChar('!'), QChar('?')}) {
        const qsizetype at = text.indexOf(mark, 120);
        if (at >= 0 && at < 320 && (end < 0 || at < end))
            end = at;
    }
    return end > 0 ? text.left(end + 1) : text.left(319) + "…";
}

// A text cut to fit at the end of a sentence, or marked as cut, so the
// model is not left guessing at half a sentence.
QString brief(const QString &text, qsizetype room) {
    if (text.size() <= room)
        return text;
    const qsizetype end = qMax(text.lastIndexOf(". ", room), qMax(text.lastIndexOf("? ", room), text.lastIndexOf("! ", room)));
    return end > room / 3 ? text.left(end + 1) : text.left(room - 1) + "…";
}

// The most connections across books that go with a question.
constexpr int mostConnections = 6;

// The book a thing is from, for a reader of another book: the latest read
// of the books it was met in, other than that one.
QString titleOf(const Library &library, const Graph &graph, qint64 node, qint64 notBook) {
    for (const GraphPlace &place : graph.seen(node))
        if (place.bookId != notBook)
            if (const auto book = library.book(place.bookId))
                return book->meta.title;
    return {};
}

QString stars(int rating) { return rating > 0 ? QString(", %1 of 5 stars").arg(rating) : QString(); }

} // namespace

GraphContext::Reach GraphContext::reachOf(const Library &library, qint64 bookId) {
    Reach reach;
    const auto book = library.book(bookId);
    if (!book)
        return reach;
    const Progress read = library.progress(bookId);
    reach.all = book->status() == "read";
    reach.any = reach.all || read.percent > 0;
    reach.place = furthest(library, bookId);
    return reach;
}

GraphPlace GraphContext::furthest(const Library &library, qint64 bookId) {
    const Progress read = library.progress(bookId);
    const GraphPlace here{bookId, read.section, read.fraction};
    // Kept by the database as the reading place moves (Library::open).
    const Recap most = library.notes(bookId, "furthest");
    const GraphPlace ever{bookId, most.section, most.fraction};
    return most.updated > 0 && here.notAfter(ever) ? ever : here;
}

std::optional<GraphPlace> GraphContext::spoilersAfter(const Library &library, qint64 bookId) {
    const auto book = library.book(bookId);
    if (!book || book->status() == "read" || book->percent <= 0)
        return std::nullopt;
    return furthest(library, bookId);
}

GraphKnown GraphContext::readerKnows(const Library &library) {
    auto reaches = std::make_shared<QHash<qint64, Reach>>();
    return [&library, reaches](const GraphPlace &at) {
        if (at.bookId == 0)
            return true;
        if (!reaches->contains(at.bookId))
            reaches->insert(at.bookId, reachOf(library, at.bookId));
        return reaches->value(at.bookId).covers(at);
    };
}

GraphKnown GraphContext::unspoiled(const Library &library) {
    auto bounds = std::make_shared<QHash<qint64, std::optional<GraphPlace>>>();
    return [&library, bounds](const GraphPlace &at) {
        if (at.bookId == 0)
            return true;
        if (!bounds->contains(at.bookId))
            bounds->insert(at.bookId, spoilersAfter(library, at.bookId));
        const std::optional<GraphPlace> bound = bounds->value(at.bookId);
        return !bound || at.notAfter(*bound);
    };
}

GraphView GraphContext::story(const Library &library, qint64 bookId, const std::optional<GraphPlace> &upTo) {
    GraphView view = Graph(library).ofBook(bookId, upTo);
    view.nodes.removeIf([](const GraphNode &node) { return !Prompts::graphKinds().contains(node.kind); });
    QSet<qint64> kept;
    for (const GraphNode &node : std::as_const(view.nodes))
        kept << node.id;
    view.edges.removeIf([&](const GraphEdge &edge) { return !kept.contains(edge.source) || !kept.contains(edge.target); });
    return view;
}

// Names of a letter or two would be found in every other word, and so
// would the short words of a longer name.
bool GraphContext::namedIn(const GraphNode &node, const QString &lower, bool loosely) {
    static const QRegularExpression apart("[^\\p{L}\\p{N}]+");
    const QStringList names = namesOf(node);
    return std::ranges::any_of(names, [&](const QString &name) {
        if (name.size() >= 3 && lower.contains(name.toLower()))
            return true;
        if (!loosely)
            return false;
        const QStringList words = name.toLower().split(apart, Qt::SkipEmptyParts);
        return std::ranges::any_of(words, [&](const QString &word) {
            return word.size() >= 4 && lower.contains(word);
        });
    });
}

QString GraphContext::forQuestion(const Library &library, qint64 bookId, const GraphPlace &upTo, const QString &words, int mostNodes) {
    const GraphView view = storyOf(library, bookId, upTo);
    const QString lower = words.toLower();
    QList<GraphNode> chosen;
    QSet<qint64> taken;
    for (const GraphNode &node : view.nodes)
        if (chosen.size() < mostNodes && namedIn(node, lower, true)) {
            chosen << node;
            taken << node.id;
        }
    if (chosen.isEmpty())
        return {};
    // Then what they are tied to: who a person belongs to, where a thing is.
    QSet<qint64> beside;
    for (const GraphEdge &edge : view.edges) {
        if (taken.contains(edge.source) && !taken.contains(edge.target))
            beside << edge.target;
        if (taken.contains(edge.target) && !taken.contains(edge.source))
            beside << edge.source;
    }
    for (const GraphNode &node : view.nodes)
        if (chosen.size() < mostNodes && beside.contains(node.id))
            chosen << node;
    QString slice = written(chosen, view.edges, mostNodes * 2);
    // And what those things connect to in the other books read: a few
    // lines, each naming the book, so the answer can say "you met a
    // similar idea in ...".
    const Graph graph(library);
    const GraphView beyond = graph.beyond(bookId, upTo);
    QHash<qint64, GraphNode> ends;
    for (const GraphNode &node : chosen + beyond.nodes)
        ends.insert(node.id, node);
    QSet<qint64> asked;
    for (const GraphNode &node : std::as_const(chosen))
        asked << node.id;
    int connections = 0;
    for (const GraphEdge &edge : beyond.edges) {
        if (connections >= mostConnections)
            break;
        const bool outward = asked.contains(edge.source);
        if (!edge.across() || (!outward && !asked.contains(edge.target)))
            continue;
        const GraphNode other = ends.value(outward ? edge.target : edge.source);
        const QString title = titleOf(library, graph, other.id, bookId);
        if (other.id == 0 || title.isEmpty())
            continue;
        slice += "\n" + ends.value(edge.source).name + " — " + edge.relation() + " → " + ends.value(edge.target).name + " [" + other.kind
                 + " in " + title + (other.summary.isEmpty() ? QString() : ": " + brief(other.summary, 140)) + "]"
                 + (edge.note.isEmpty() ? QString() : " (" + brief(edge.note, 160) + ")");
        ++connections;
    }
    return slice;
}

QString GraphContext::digest(const Library &library, qint64 bookId, const GraphPlace &upTo, const QStringList &kinds, int mostNodes) {
    GraphView view = storyOf(library, bookId, upTo);
    view.nodes.removeIf([&](const GraphNode &node) { return !kinds.contains(node.kind); });
    // The most tied-in are the ones that matter; cut, they are kept.
    QHash<qint64, int> ties;
    for (const GraphEdge &edge : std::as_const(view.edges)) {
        ++ties[edge.source];
        ++ties[edge.target];
    }
    std::ranges::stable_sort(view.nodes, std::greater{}, [&](const GraphNode &node) { return ties.value(node.id); });
    return written(view.nodes.mid(0, mostNodes), view.edges, mostNodes * 2);
}

QString GraphContext::picture(const Library &library, const QString &question, qsizetype room) {
    const Graph graph(library);
    QHash<qint64, Book> books;
    for (const Book &book : library.books())
        books.insert(book.id, book);
    QStringList parts;

    // What was read when: a line a book, the latest first.
    struct Read {
        qint64 bookId = 0;
        QString first, last;
        int days = 0;
        double to = 0;
    };
    QHash<qint64, Read> reads;
    for (const ReadingDay &day : graph.readingLog()) {
        Read &read = reads[day.bookId];
        read.bookId = day.bookId;
        if (read.first.isEmpty())
            read.first = day.day;
        read.last = day.day;
        read.to = qMax(read.to, day.to);
        ++read.days;
    }
    QList<Read> history = reads.values();
    std::ranges::sort(history, std::greater{}, &Read::last);
    QStringList lines;
    for (const Read &read : history.mid(0, 30)) {
        const auto book = books.constFind(read.bookId);
        if (book == books.constEnd())
            continue;
        lines << QString("%1: read %2 to %3 on %4, to %5%%6").arg(book->meta.title, read.first, read.last,
                                                                 read.days == 1 ? QString("1 day") : QString("%1 days").arg(read.days))
                     .arg(qRound(read.to)).arg(stars(book->rating));
    }
    if (!lines.isEmpty())
        parts << "Reading history, the latest first:\n" + lines.join('\n');

    // The books a thing was met in, by title.
    auto metIn = [&](const GraphNode &node, int most) {
        QStringList titles;
        const QList<GraphPlace> places = graph.seen(node.id);
        for (const GraphPlace &place : places)
            if (books.contains(place.bookId) && titles.size() < most)
                titles << books.value(place.bookId).meta.title;
        return QPair<int, QString>(places.size(), titles.join("; "));
    };
    // The themes, ideas and topics met in the most books: those are what
    // the reader keeps coming back to.
    auto mostMet = [&](const QString &kind, const QString &title, int most) {
        struct Met {
            GraphNode node;
            int count = 0;
            QString in;
        };
        QList<Met> met;
        for (const GraphNode &node : graph.find({}, kind, QString(), 300)) {
            const auto [count, in] = metIn(node, 3);
            if (count > 0)
                met.append({node, count, in});
        }
        std::ranges::stable_sort(met, std::greater{}, &Met::count);
        QStringList entries;
        for (const Met &entry : met.mid(0, most))
            entries << entry.node.name + " (" + entry.in + ")";
        if (!entries.isEmpty())
            parts << title + ":\n" + entries.join('\n');
    };
    mostMet("theme", "Themes, with the books they were met in", 25);
    mostMet("topic", "Real-world topics, works and authors the books touch", 25);
    mostMet("concept", "Ideas and systems of the books' worlds", 15);

    // The connections noticed between books, the latest first: what the
    // reader's books have to say to one another.
    const GraphView across = graph.acrossBooks(40);
    QHash<qint64, GraphNode> ends;
    for (const GraphNode &node : across.nodes)
        ends.insert(node.id, node);
    lines.clear();
    for (const GraphEdge &edge : across.edges) {
        const QString from = titleOf(library, graph, edge.source, 0), to = titleOf(library, graph, edge.target, 0);
        if (lines.size() >= 12 || from.isEmpty() || to.isEmpty())
            continue;
        lines << ends.value(edge.source).name + " [" + from + "] — " + edge.relation() + " → " + ends.value(edge.target).name + " ["
                     + to + "]" + (edge.note.isEmpty() ? QString() : ": " + brief(edge.note, 140));
    }
    if (!lines.isEmpty())
        parts << "Connections between books:\n" + lines.join('\n');

    // What the question's own words name in the graph, with what is known.
    QStringList touched;
    QSet<qint64> shown;
    const GraphKnown knows = readerKnows(library);
    const QStringList words = question.toLower().split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word.size() < 4 || touched.size() >= 15)
            continue;
        for (const GraphNode &node : graph.find(word, {}, QString(), 8)) {
            if (shown.contains(node.id) || !Prompts::graphKinds().contains(node.kind) || touched.size() >= 15)
                continue;
            shown << node.id;
            const QString in = metIn(node, 3).second;
            // As far as the reader got in each book: never a summary or a
            // name from past their page.
            touched << line(Graph::asKnown(node, graph.facts(node.id), 0, knows)) + (in.isEmpty() ? QString() : " [in: " + in + "]");
        }
    }
    if (!touched.isEmpty())
        parts << "In the graph for the words of the question:\n" + touched.join('\n');

    // The latest of what was asked, with the gist of the answer.
    QList<GraphNode> asked = graph.find({}, "question", QString(), 500);
    std::ranges::stable_sort(asked, std::greater{}, &GraphNode::updated);
    lines.clear();
    for (const GraphNode &node : asked.mid(0, 12)) {
        const qint64 of = node.data.value("book").toInteger();
        lines << node.name + (books.contains(of) ? " [of " + books.value(of).meta.title + "]" : QString())
                     + (node.summary.isEmpty() ? QString() : " — " + brief(node.summary, 200));
    }
    if (!lines.isEmpty())
        parts << "Questions the reader asked lately:\n" + lines.join('\n');

    // Each part gets its share, so a long history can't crowd out the rest.
    QString text;
    const qsizetype share = parts.isEmpty() ? 0 : room / parts.size();
    for (const QString &part : std::as_const(parts)) {
        const QString fitted = part.size() <= share ? part : part.left(part.lastIndexOf('\n', share));
        text += fitted + "\n\n";
    }
    return text.trimmed();
}

qint64 GraphContext::noteQuestion(Library &library, qint64 bookId, const std::optional<GraphPlace> &place, const QString &question,
                                  const QString &answer) {
    Graph graph(library);
    const auto book = bookId > 0 ? library.book(bookId) : std::nullopt;
    QString name = question.simplified().left(140);
    if (name.isEmpty() || (bookId > 0 && !book))
        return 0;
    // The same words asked of another book are another question.
    if (const qint64 before = graph.named({}, "question", name))
        if (graph.node(before)->data.value("book").toInteger() != bookId)
            name += " (" + (book ? book->meta.title : QString("the library")) + ")";
    QJsonObject data{{"asked", QDateTime::currentDateTime().toString(Qt::ISODate)}, {"of", book ? "book" : "library"}};
    if (book)
        data.insert("book", bookId);
    const std::optional<GraphPlace> at = book && place ? std::optional(GraphPlace{bookId, place->section, place->fraction}) : std::nullopt;
    const qint64 id = graph.upsert({}, "question", name, gistOf(answer), data, at);
    if (id == 0)
        return 0;
    const QString said = (question + "\n" + answer).toLower();
    int mentions = 0;
    if (book) {
        graph.link(id, graph.bookNode(bookId), "asked about", {}, at);
        // The things of the book the question and its answer name, as far
        // as the reader had got.
        for (const GraphNode &node : storyOf(library, bookId, at).nodes)
            if (mentions < 12 && namedIn(node, said)) {
                graph.link(id, node.id, "mentions", {}, at);
                ++mentions;
            }
        return id;
    }
    // Asked of the library: the books the answer names, and the themes,
    // ideas and topics the question does.
    for (const Book &other : library.books())
        if (mentions < 12 && other.meta.title.size() >= 4 && said.contains(other.meta.title.toLower())) {
            graph.link(id, graph.bookNode(other.id), "mentions");
            ++mentions;
        }
    const QStringList words = question.toLower().split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word.size() < 4)
            continue;
        for (const GraphNode &node : graph.find(word, {}, QString(), 8))
            if (mentions < 20 && Prompts::graphKinds().contains(node.kind) && namedIn(node, said)) {
                graph.link(id, node.id, "mentions");
                ++mentions;
            }
    }
    return id;
}
