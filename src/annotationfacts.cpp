#include "annotationfacts.h"

#include <QList>
#include <QSqlQuery>
#include <QVariant>
#include <optional>

#include "annotationlayers.h"
#include "changelog.h"
#include "graph.h"
#include "library.h"

using namespace Qt::StringLiterals;

namespace {

using AnnotationFacts::Source;

// A few words of a passage, to find the spot again (GraphMoment::anchor).
[[nodiscard]] QString anchorOf(const QString &quote) { return quote.simplified().section(u' ', 0, 7); }

[[nodiscard]] std::optional<Source> own(const Library &library, qint64 id) {
    QSqlQuery query = ChangeLog::query(library, "SELECT book_id FROM annotations WHERE id = ?", {id});
    if (!query.next())
        return std::nullopt;
    const qint64 book = query.value(0).toLongLong();
    query.finish();
    for (const Annotation &annotation : library.annotations(book))
        if (annotation.id == id) {
            const QString quote = annotation.quote.isEmpty() ? annotation.text : annotation.quote;
            return Source{.book = book,
                          .place = annotation.place,
                          .words = (annotation.note.trimmed().isEmpty() ? quote : annotation.note).trimmed(),
                          .anchor = anchorOf(quote)};
        }
    return std::nullopt;
}

[[nodiscard]] std::optional<Source> theirs(const Library &library, const QString &gid) {
    for (const AnnotationLayers::Person &person : AnnotationLayers::read(library).people)
        for (const AnnotationLayers::Annotation &annotation : person.annotations)
            if (annotation.id == gid)
                return Source{.book = annotation.book,
                              .place = annotation.place,
                              .words = (annotation.note.trimmed().isEmpty() ? annotation.quote : annotation.note).trimmed(),
                              .anchor = anchorOf(annotation.quote)};
    return std::nullopt;
}

// The thing of that name in the graph, as it is called or one of its names.
[[nodiscard]] qint64 namedThing(const Graph &graph, const QString &name) {
    for (const GraphNode &node : graph.find(name))
        if (node.name.compare(name, Qt::CaseInsensitive) == 0)
            return node.id;
    return 0;
}

} // namespace

std::expected<Source, Failure> AnnotationFacts::of(const Library &library, const QString &annotation) {
    bool number = false;
    const qint64 id = annotation.toLongLong(&number);
    std::optional<Source> found = number ? own(library, id) : theirs(library, annotation.trimmed());
    if (!found)
        return std::unexpected(Failure(Failure::Code::NoSuchAnnotation, {annotation}));
    if (found->words.isEmpty())
        return std::unexpected(Failure(Failure::Code::AnnotationWithoutWords));
    return *found;
}

std::expected<AnnotationFacts::Made, Failure> AnnotationFacts::make(const Library &library, const Source &source, const Thing &thing) {
    if (source.words.trimmed().isEmpty())
        return std::unexpected(Failure(Failure::Code::AnnotationWithoutWords));
    // The graph of the book's tenant: what is learned in it is kept there.
    Graph graph = Graph::ofReader(library, source.book);
    const GraphPlace at{source.book, source.place};
    Made made;
    bool number = false;
    const qint64 id = thing.which.toLongLong(&number);
    if (number && graph.node(id))
        made.node = id;
    else if (!number)
        made.node = namedThing(graph, thing.which.simplified());
    if (made.node == 0 && !number && !thing.kind.trimmed().isEmpty() && !thing.which.trimmed().isEmpty()) {
        made.node = graph.upsert({}, thing.kind.trimmed(), thing.which.simplified(), {}, {}, at, source.anchor);
        made.madeNode = made.node > 0;
    }
    if (made.node == 0)
        return std::unexpected(Failure(Failure::Code::NoSuchThing, {thing.which}));
    made.fact = graph.learn(made.node, u"summary"_s, source.words.trimmed(), GraphMoment{at, source.anchor});
    if (made.fact == 0)
        return std::unexpected(Failure(Failure::Code::FactNotKept));
    return made;
}
