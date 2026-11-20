// The picture of the library the AI is given for a question asked of all of
// it, and for suggestions (GraphContext::picture): of one tenant, the
// graph's, and of nothing beyond it (ADR 0003, #95).

#include <QHash>
#include <QPair>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>
#include <functional>

#include "graphcontext.h"

namespace {

// The most things the words of a question to the whole library name.
constexpr qsizetype mostTouched = 15;

QString stars(int rating) { return rating > 0 ? QString(", %1 of 5 stars").arg(rating) : QString(); }

// What the picture is drawn from: the graph, and the books of its tenant by number.
struct Drawn {
    Graph graph;
    QHash<qint64, Book> books;

    // How many books a thing was met in, and the titles of the first few.
    [[nodiscard]] QPair<int, QString> metIn(const GraphNode &node, int most) const {
        QStringList titles;
        const QList<GraphPlace> places = graph.seen(node.id);
        for (const GraphPlace &place : places)
            if (books.contains(place.bookId) && titles.size() < most)
                titles << books.value(place.bookId).meta.title;
        return {static_cast<int>(places.size()), titles.join("; ")};
    }
};

// What was read when: a line a book, the latest first.
QString history(const Drawn &drawn) {
    struct Read {
        qint64 bookId = 0;
        QString first, last;
        int days = 0;
        double to = 0;
    };
    QHash<qint64, Read> reads;
    for (const ReadingDay &day : drawn.graph.readingLog()) {
        if (!drawn.books.contains(day.bookId))
            continue;
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
        const Book &book = drawn.books[read.bookId];
        lines << QString("%1: read %2 to %3 on %4, to %5%%6")
                     .arg(book.meta.title, read.first, read.last, read.days == 1 ? QString("1 day") : QString("%1 days").arg(read.days))
                     .arg(qRound(read.to))
                     .arg(stars(book.rating));
    }
    return lines.isEmpty() ? QString() : "Reading history, the latest first:\n" + lines.join('\n');
}

// The things of a kind met in the most books: what the reader keeps coming back to.
QString mostMet(const Drawn &drawn, const QString &kind, const QString &title, int most) {
    struct Met {
        GraphNode node;
        int count = 0;
        QString in;
    };
    QList<Met> met;
    for (const GraphNode &node : drawn.graph.find({}, kind, QString(), 300)) {
        const auto [count, in] = drawn.metIn(node, 3);
        if (count > 0)
            met.append({node, count, in});
    }
    std::ranges::stable_sort(met, std::greater{}, &Met::count);
    QStringList entries;
    for (const Met &entry : met.mid(0, most))
        entries << entry.node.name + " (" + entry.in + ")";
    return entries.isEmpty() ? QString() : title + ":\n" + entries.join('\n');
}

// The connections noticed between books, the latest first: what the
// reader's books have to say to one another.
QString connections(const Drawn &drawn) {
    const GraphView across = drawn.graph.acrossBooks(40);
    QHash<qint64, GraphNode> ends;
    for (const GraphNode &node : across.nodes)
        ends.insert(node.id, node);
    QStringList lines;
    for (const GraphEdge &edge : across.edges) {
        const QString from = GraphContext::bookOf(drawn.graph, edge.source, 0);
        const QString to = GraphContext::bookOf(drawn.graph, edge.target, 0);
        if (lines.size() >= 12 || from.isEmpty() || to.isEmpty())
            continue;
        lines << ends.value(edge.source).name + " [" + from + "] — " + edge.relation() + " → " + ends.value(edge.target).name + " [" + to
                     + "]" + (edge.note.isEmpty() ? QString() : ": " + GraphContext::brief(edge.note, 140));
    }
    return lines.isEmpty() ? QString() : "Connections between books:\n" + lines.join('\n');
}

// What the graphs someone shared know of the words of a question, as far as
// the reader got in each book of the tenant, said to be theirs.
void sharedTouched(const Graph &graph, const QStringList &words, const GraphKnown &knows, QStringList &touched, QSet<qint64> &shown) {
    const auto named = [&words](const GraphNode &node) {
        const QString names = (QStringList{node.name} + node.aliases()).join('\n').toLower();
        return std::ranges::any_of(words, [&names](const QString &word) { return word.size() >= 4 && names.contains(word); });
    };
    for (const GraphLayers::Layer &layer : GraphContext::layers(graph))
        for (const Graph::Sighting &seen : layer.sightings()) {
            const GraphNode node = Graph::asKnown(seen.node, layer.facts.value(seen.node.id), seen.bookId, knows);
            const auto book = graph.library().book(seen.bookId);
            if (touched.size() >= mostTouched || !book || shown.contains(node.id) || !GraphNode::storyKinds().contains(node.kind)
                || !named(node))
                continue;
            shown << node.id;
            touched << GraphContext::line(node) + " [in: " + book->meta.title + "; from "
                           + GraphLayers::from(node, GraphLayers::Said::English) + ", not the reader's own]";
        }
}

// What the words of a question name in the graph, a line a thing, as far as
// the reader got in each book: never a summary or a name from past their
// page. Then what the graphs someone shared know of them, as far too.
QString touchedBy(const Drawn &drawn, const QString &question) {
    QStringList touched;
    QSet<qint64> shown;
    const GraphKnown knows = GraphContext::readerKnows(drawn.graph.library());
    const QStringList words = question.toLower().split(QRegularExpression("[^\\p{L}\\p{N}]+"), Qt::SkipEmptyParts);
    for (const QString &word : words) {
        if (word.size() < 4 || touched.size() >= mostTouched)
            continue;
        for (const GraphNode &node : drawn.graph.find(word, {}, QString(), 8)) {
            if (shown.contains(node.id) || !GraphNode::storyKinds().contains(node.kind) || touched.size() >= mostTouched)
                continue;
            shown << node.id;
            const QString in = drawn.metIn(node, 3).second;
            touched << GraphContext::line(Graph::asKnown(node, drawn.graph.facts(node.id), 0, knows))
                           + (in.isEmpty() ? QString() : " [in: " + in + "]");
        }
    }
    sharedTouched(drawn.graph, words, knows, touched, shown);
    return touched.isEmpty() ? QString() : "In the graph for the words of the question:\n" + touched.join('\n');
}

// The latest of what was asked, with the gist of the answer.
QString questions(const Drawn &drawn) {
    QList<GraphNode> asked = drawn.graph.find({}, "question", QString(), 500);
    std::ranges::stable_sort(asked, std::greater{}, &GraphNode::updated);
    QStringList lines;
    for (const GraphNode &node : asked.mid(0, 12)) {
        const qint64 of = node.data.value("book").toInteger();
        lines << node.name + (drawn.books.contains(of) ? " [of " + drawn.books.value(of).meta.title + "]" : QString())
                     + (node.summary.isEmpty() ? QString() : " — " + GraphContext::brief(node.summary, 200));
    }
    return lines.isEmpty() ? QString() : "Questions the reader asked lately:\n" + lines.join('\n');
}

} // namespace

QString GraphContext::picture(const Graph &graph, const QString &question, qsizetype room) {
    Drawn drawn{.graph = graph, .books = {}};
    for (const Book &book : graph.library().books())
        if (graph.sees(book.id))
            drawn.books.insert(book.id, book);
    QStringList parts{history(drawn),
                      mostMet(drawn, "theme", "Themes, with the books they were met in", 25),
                      mostMet(drawn, "topic", "Real-world topics, works and authors the books touch", 25),
                      mostMet(drawn, "concept", "Ideas and systems of the books' worlds", 15),
                      connections(drawn),
                      touchedBy(drawn, question),
                      questions(drawn)};
    parts.removeAll(QString());
    // Each part gets its share, so a long history can't crowd out the rest.
    QString text;
    const qsizetype share = parts.isEmpty() ? 0 : room / parts.size();
    for (const QString &part : std::as_const(parts)) {
        const QString fitted = part.size() <= share ? part : part.left(part.lastIndexOf('\n', share));
        text += fitted + "\n\n";
    }
    return text.trimmed();
}

std::expected<QString, QString> GraphContext::pictureIn(const Library &library, const QString &collection, const QString &question,
                                                        bool wanted) {
    if (!wanted)
        return QString();
    const auto tenant = Tenancy::chosen(library, collection);
    if (!tenant)
        return std::unexpected(tenant.error());
    return picture(Graph::forTenant(library, *tenant), question);
}
