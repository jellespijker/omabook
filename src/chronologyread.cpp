#include "chronologyread.h"

#include <QCoreApplication>
#include <ranges>

#include "booksides.h"
#include "chronicle.h"
#include "graphcontext.h"
#include "graphlayers.h"

using namespace Chronology;
using namespace Qt::StringLiterals;
using Reading::From;

namespace {

// As many as a graph has: what an event involves is read of all of it.
constexpr int everyThing = 1'000'000;

// In the catalogs: bin/i18n reads what tr() is handed.
const auto tr = [](const char *text) { return QCoreApplication::translate("Chronology", text); };

// The books of what the chronology is opened from.
[[nodiscard]] QSet<qint64> booksOf(const Library &library, const From &from) {
    QSet<qint64> books;
    if (from.what == From::What::Book) {
        books.insert(from.bookId);
        return books;
    }
    if (from.what == From::What::Scope)
        return books;
    const auto collection = from.what == From::What::Collection ? library.collectionNamed(from.name) : std::nullopt;
    for (const Book &book : library.books())
        if (from.what == From::What::Collection ? collection && collection->contains(book)
                                                : book.meta.series.compare(from.name, Qt::CaseInsensitive) == 0)
            books.insert(book.id);
    return books;
}

// The name of a side, when a whole graph is shown a side at a time.
[[nodiscard]] QString nameOf(const Library &library, const BookSides::Side &side) {
    QStringList names;
    for (const Collection &collection : library.collections())
        if (collection.parent == 0 && side.contains(collection.id))
            names << collection.name;
    if (side.contains(BookSides::none))
        names << tr("Books in no collection");
    return names.join(u", "_s);
}

void merge(Facts &into, const Facts &more) {
    into.events += more.events;
    into.timelines += more.timelines;
    for (auto times = more.times.cbegin(); times != more.times.cend(); ++times)
        into.times.insert(times.key(), *times);
    for (auto met = more.met.cbegin(); met != more.met.cend(); ++met)
        into.met.insert(met.key(), *met);
    into.edges += more.edges;
}

// Of these things and relations, what each event involves, of those known.
void involve(Reading::Read &read, const QList<GraphNode> &nodes, const QList<GraphEdge> &edges, const GraphKnown &known) {
    QHash<qint64, GraphNode> things;
    for (const GraphNode &node : nodes)
        if (Reading::involvedKinds().contains(node.kind))
            things.insert(node.id, node);
    for (const GraphEdge &edge : edges) {
        if (!known(edge.learned))
            continue;
        const auto tie = [&](qint64 event, qint64 thing) {
            const auto found = read.events.find(event);
            if (found == read.events.end() || !things.contains(thing) || found->involved.contains(thing))
                return;
            found->involved.append(thing);
            read.involved.insert(thing, things.value(thing));
        };
        tie(edge.source, edge.target);
        tie(edge.target, edge.source);
    }
}

void keepEvents(Reading::Read &read, const Facts &facts) {
    for (const GraphNode &event : facts.events) {
        Reading::Event &kept = read.events[event.id];
        kept.node = event;
        kept.from = GraphLayers::from(event);
        kept.met = facts.met.value(event.id);
        for (const GraphPlace &place : std::as_const(kept.met))
            if (place.bookId > 0)
                kept.books.insert(place.bookId);
    }
}

} // namespace

const QStringList &Reading::involvedKinds() {
    static const QStringList kinds{u"character"_s, u"place"_s, u"group"_s, u"theme"_s};
    return kinds;
}

Reading::Read Reading::read(const Library &library, const From &from) {
    const GraphKnown known = GraphContext::readerKnows(library);
    const BookSides sides = BookSides::of(library);
    Read told;
    told.books = booksOf(library, from);
    const QString scope = from.what == From::What::Scope ? from.scope : QString();
    // A whole graph a side at a time; a book, a series or a collection on the side of its books.
    QList<Tenant> tenants;
    if (from.what == From::What::Scope)
        tenants = Chronology::tenants(library, scope);
    else
        tenants.append(Tenant{.scope = scope, .side = sides.sideOf(told.books)});
    // The shared graphs are of the general graph, and are the reader's as far as they read.
    const QList<GraphLayers::Layer> layers = scope.isEmpty() ? GraphContext::layers(library) : QList<GraphLayers::Layer>{};
    const QList<Facts> shared = layers | std::views::transform([](const GraphLayers::Layer &layer) { return Chronology::read(layer); })
                                | std::ranges::to<QList<Facts>>();
    for (const Tenant &tenant : tenants) {
        Facts facts = Chronology::read(library, known, tenant);
        for (const Facts &layer : shared)
            merge(facts, within(layer, sides, tenant.side));
        keepEvents(told, facts);
        told.parts.append({.name = tenants.size() > 1 ? nameOf(library, tenant.side) : QString(), .chronicle = chronicle(facts)});
    }
    const GraphView whole = Graph(library).whole(scope.isNull() ? u""_s : scope, everyThing);
    involve(told, whole.nodes, whole.edges, known);
    for (const GraphLayers::Layer &layer : layers)
        involve(told, layer.nodes, layer.edges, [](const GraphPlace &) { return true; });
    return told;
}
