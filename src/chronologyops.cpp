#include "chronologyops.h"

#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <ranges>

#include "chronicle.h"
#include "chroniclefilter.h"
#include "chronology.h"
#include "graphcontext.h"

using namespace Qt::StringLiterals;

namespace {

using ChronologyOps::Ask;

constexpr auto eventKind = "event"_L1;

// The scope as the graph keeps it: the general graph's is empty, never null.
[[nodiscard]] QString kept(const QString &scope) { return scope.isNull() ? u""_s : scope; }

// A thing named on the command line: by its number (whose kind the writers
// check), or by its name or one of its other names, in the scope and then,
// for a timeline, the general graph that every book's timelines are in.
[[nodiscard]] std::expected<qint64, QString> named(const Graph &graph, const QString &scope, const QString &kind, const QString &word) {
    bool number = false;
    if (const qint64 id = word.toLongLong(&number); number && id > 0)
        return id;
    if (const qint64 found = graph.named(kept(scope), kind, word.simplified()); found > 0)
        return found;
    if (kind == Chronology::timelineKind() && !scope.isEmpty())
        if (const qint64 found = graph.named(u""_s, kind, word.simplified()); found > 0)
            return found;
    return std::unexpected(u"There is no %1 called \"%2\" in the graph."_s.arg(kind, word));
}

// What a thing is called; empty for one that isn't there.
[[nodiscard]] QString nameOf(const Graph &graph, qint64 id) { return graph.node(id).value_or(GraphNode{}).name; }

// Only the books that are in both, where both name some.
[[nodiscard]] std::optional<QSet<qint64>> both(const std::optional<QSet<qint64>> &one, const QSet<qint64> &other) {
    return one ? *one & other : other;
}

// The books asked for, by book, series and collection together; none for every book.
[[nodiscard]] std::expected<std::optional<QSet<qint64>>, QString> booksAsked(const Library &library, const Ask &ask) {
    std::optional<QSet<qint64>> books;
    if (ask.book > 0) {
        if (!library.book(ask.book))
            return std::unexpected(u"There is no book %1 in the library."_s.arg(ask.book));
        books = QSet<qint64>{ask.book};
    }
    const QList<Book> all = ask.series.isEmpty() && ask.collection.isEmpty() ? QList<Book>() : library.books();
    if (!ask.series.isEmpty()) {
        QSet<qint64> inSeries;
        for (const Book &book : all)
            if (book.meta.series.compare(ask.series.simplified(), Qt::CaseInsensitive) == 0)
                inSeries.insert(book.id);
        if (inSeries.isEmpty())
            return std::unexpected(u"No book of the library is in a series called %1."_s.arg(ask.series));
        books = both(books, inSeries);
    }
    if (!ask.collection.isEmpty()) {
        const auto collection = library.collectionNamed(ask.collection);
        if (!collection)
            return std::unexpected(u"There is no collection called %1. omabook collections lists them."_s.arg(ask.collection));
        QSet<qint64> inCollection;
        for (const Book &book : collection->of(all))
            inCollection.insert(book.id);
        books = both(books, inCollection);
    }
    return books;
}

// Who knows a place: the reader, as far as each book was read; with a place
// in the book asked for, that book up to there; or anyone, of everything.
[[nodiscard]] std::expected<GraphKnown, QString> knownBy(const Library &library, const Ask &ask) {
    if (ask.whole && ask.upTo)
        return std::unexpected(u"Cut the chronology at a place (--upto) or take it whole (--whole), not both."_s);
    if (ask.whole)
        return GraphKnown([](const GraphPlace &) { return true; });
    GraphKnown reader = GraphContext::readerKnows(library);
    if (!ask.upTo)
        return reader;
    if (ask.book <= 0 || ask.upTo->bookId != ask.book)
        return std::unexpected(u"--upto is a place in a book: name the book too."_s);
    return GraphKnown([reader = std::move(reader), upTo = *ask.upTo](const GraphPlace &place) {
        return place.bookId == upTo.bookId ? place.notAfter(upTo) : reader(place);
    });
}

[[nodiscard]] QJsonObject cutOf(const Ask &ask) {
    if (ask.whole)
        return {{"at", "whole"}};
    if (ask.upTo)
        return {{"at", "place"}, {"book", ask.upTo->bookId}, {"section", ask.upTo->section}, {"fraction", ask.upTo->fraction}};
    return {{"at", "reader"}};
}

// The stretch of time asked for; a placeholder calendar checks its ends
// as those of a story's own calendar when the stretch is on each lane's.
[[nodiscard]] std::expected<std::optional<EventTime::Reckoning>, QString> spanOf(const Ask &ask) {
    if (ask.from.isEmpty() && ask.to.isEmpty())
        return std::nullopt;
    QJsonObject object{{"calendar", ask.calendar.isEmpty() ? u"each timeline's own"_s : ask.calendar}};
    if (!ask.from.isEmpty())
        object.insert("from", ask.from.trimmed());
    if (!ask.to.isEmpty())
        object.insert("to", ask.to.trimmed());
    const auto time = EventTime::parse(object);
    if (!time)
        return std::unexpected(u"--from, --to: "_s + time.error());
    EventTime::Reckoning span = time->on;
    if (ask.calendar.isEmpty())
        span.calendar.clear();
    return span;
}

[[nodiscard]] std::expected<qint64, QString> timelineAsked(const Library &library, const Ask &ask) {
    if (ask.timeline.isEmpty())
        return 0;
    const Graph graph(library);
    const auto timeline = named(graph, ask.scope, Chronology::timelineKind(), ask.timeline);
    if (!timeline)
        return std::unexpected(timeline.error());
    if (const auto node = graph.node(*timeline); !node || node->kind != Chronology::timelineKind())
        return std::unexpected(u"There is no timeline %1 in the graph."_s.arg(ask.timeline));
    return *timeline;
}

// Marks what was revealed in no book, at any depth: it was put in by hand.
[[nodiscard]] QJsonValue markedByHand(const QJsonValue &value) {
    if (value.isArray()) {
        QJsonArray marked;
        for (const auto &each : value.toArray())
            marked << markedByHand(each);
        return marked;
    }
    if (!value.isObject())
        return value;
    QJsonObject object = value.toObject();
    for (auto member = object.begin(); member != object.end(); ++member)
        member.value() = markedByHand(member.value());
    if (object.contains("revealed") && object.value("revealed").toObject().value("book").toInteger() == 0)
        object.insert("by_hand", true);
    return object;
}

// The books the facts were met or revealed in, as they are called.
void nameBooks(QJsonObject &books, const Library &library, const Chronology::Facts &facts) {
    QSet<qint64> ids;
    for (const GraphNode &event : facts.events) {
        for (const GraphPlace &place : facts.met.value(event.id))
            ids.insert(place.bookId);
        for (const Chronology::Dated &dated : facts.times.value(event.id))
            ids.insert(dated.revealed.place.bookId);
    }
    for (const GraphEdge &edge : facts.edges)
        ids.insert(edge.learned.bookId);
    for (const qint64 id : std::as_const(ids))
        if (const auto book = id > 0 ? library.book(id) : std::nullopt)
            books.insert(QString::number(id), book->meta.title);
}

[[nodiscard]] QJsonArray sideJson(const BookSides::Side &side) {
    QList<qint64> sorted = side.values();
    std::ranges::sort(sorted);
    return QJsonArray::fromVariantList(sorted | std::ranges::to<QVariantList>());
}

} // namespace

std::expected<QJsonObject, QString> ChronologyOps::read(const Library &library, const Ask &ask) {
    const auto books = booksAsked(library, ask);
    if (!books)
        return std::unexpected(books.error());
    const auto known = knownBy(library, ask);
    if (!known)
        return std::unexpected(known.error());
    const auto timeline = timelineAsked(library, ask);
    if (!timeline)
        return std::unexpected(timeline.error());
    const auto span = spanOf(ask);
    if (!span)
        return std::unexpected(span.error());
    const Chronology::Narrowing narrowing{.timeline = *timeline, .span = *span};
    const QList<Chronology::Tenant> tenants = Chronology::tenants(library, kept(ask.scope));
    QJsonArray parts;
    QJsonObject titles;
    qsizetype total = 0;
    qsizetype shown = 0;
    for (const Chronology::Tenant &tenant : tenants) {
        Chronology::Facts facts = Chronology::read(library, *known, tenant);
        if (*books)
            facts = Chronology::ofBooks(std::move(facts), **books);
        // A side with none of the books asked for is no part of their chronology.
        if (facts.events.isEmpty() && tenants.size() > 1)
            continue;
        const Chronology::Chronicle whole = Chronology::chronicle(facts);
        const Chronology::Chronicle told = Chronology::narrowed(whole, facts, narrowing);
        total += Chronology::eventsIn(whole).size();
        shown += Chronology::eventsIn(told).size();
        QJsonObject part = markedByHand(told.toJson()).toObject();
        part.insert("side", sideJson(tenant.side));
        parts << part;
        nameBooks(titles, library, facts);
    }
    return QJsonObject{{"cut", cutOf(ask)},
                       {"events", QJsonObject{{"shown", static_cast<qint64>(shown)}, {"total", static_cast<qint64>(total)}}},
                       {"books", titles},
                       {"parts", parts}};
}

std::expected<EventTime, QString> ChronologyOps::timeOf(const Said &said) {
    const QString text = said.time.trimmed();
    if (text.startsWith(u'{'))
        return EventTime::parse(QStringView(text));
    QJsonObject object;
    if (!text.isEmpty()) {
        object.insert("calendar", said.calendar.isEmpty() ? EventTime::gregorianName() : said.calendar);
        if (const qsizetype dots = text.indexOf(".."_L1); dots >= 0) {
            const QString from = text.left(dots).trimmed();
            const QString to = text.mid(dots + 2).trimmed();
            if (from.isEmpty() && to.isEmpty())
                return std::unexpected(u"A span has a beginning, an end or both: 1939..1945, 1939.. or ..1945."_s);
            if (!from.isEmpty())
                object.insert("from", from);
            if (!to.isEmpty())
                object.insert("to", to);
        } else {
            object.insert("at", text);
        }
    }
    if (said.approximate)
        object.insert("approximate", true);
    if (!said.words.isEmpty())
        object.insert("said", said.words);
    return EventTime::parse(object);
}

std::expected<QJsonObject, QString> ChronologyOps::setWhen(Graph &graph, const QString &event, const Said &said, const QString &scope) {
    const auto id = named(graph, scope, eventKind, event);
    if (!id)
        return std::unexpected(id.error());
    const auto time = timeOf(said);
    if (!time)
        return std::unexpected(time.error());
    const auto fact = Chronology::setWhen(graph, *id, *time);
    if (!fact)
        return std::unexpected(fact.error());
    QJsonObject when = time->toJson();
    when.insert("precision", EventTime::name(time->precision()));
    return QJsonObject{{"event", *id}, {"name", nameOf(graph, *id)}, {"fact", *fact}, {"time", when}, {"by_hand", true}};
}

std::expected<QJsonObject, QString> ChronologyOps::order(Graph &graph, const QString &event, const QString &relation, const QString &other,
                                                         const QString &scope) {
    const auto first = named(graph, scope, eventKind, event);
    if (!first)
        return std::unexpected(first.error());
    const auto second = named(graph, scope, eventKind, other);
    if (!second)
        return std::unexpected(second.error());
    QString said = relation.simplified().toLower();
    if (said == "same"_L1)
        said = u"same time as"_s;
    const auto edge = Chronology::order(graph, *first, said, *second);
    if (!edge)
        return std::unexpected(edge.error());
    return QJsonObject{{"edge", *edge},    {"event", *first},  {"name", nameOf(graph, *first)},
                       {"relation", said}, {"other", *second}, {"other_name", nameOf(graph, *second)},
                       {"by_hand", true}};
}

std::expected<QJsonObject, QString> ChronologyOps::place(Graph &graph, const QString &event, const QString &timeline,
                                                         const QString &scope) {
    const auto id = named(graph, scope, eventKind, event);
    if (!id)
        return std::unexpected(id.error());
    const auto line = named(graph, scope, Chronology::timelineKind(), timeline);
    if (!line)
        return std::unexpected(line.error() + u" Make it with: omabook chronology timeline add <name>"_s);
    const auto edge = Chronology::placeOn(graph, *id, *line);
    if (!edge)
        return std::unexpected(edge.error());
    return QJsonObject{
        {"edge", *edge},  {"event", *id}, {"name", nameOf(graph, *id)}, {"timeline", *line}, {"timeline_name", nameOf(graph, *line)},
        {"by_hand", true}};
}

std::expected<QJsonObject, QString> ChronologyOps::addTimeline(Graph &graph, const QString &name, const QString &kind) {
    const auto whose = kind.isEmpty() ? std::optional(Chronology::Whose::Reader) : Chronology::whose(kind.simplified().toLower());
    if (!whose)
        return std::unexpected(u"\"%1\" is no kind of timeline: book, series, world, history or reader."_s.arg(kind));
    if (name.simplified().isEmpty() && *whose != Chronology::Whose::History)
        return std::unexpected(u"A timeline needs a name."_s);
    const qint64 id = Chronology::timeline(graph, *whose, name);
    const auto node = id > 0 ? graph.node(id) : std::nullopt;
    if (!node)
        return std::unexpected(u"The timeline could not be kept."_s);
    return QJsonObject{{"timeline", id}, {"name", node->name}, {"whose", Chronology::name(*whose)}, {"by_hand", true}};
}
