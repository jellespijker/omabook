#include "annotationlayers.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonDocument>
#include <QMap>
#include <algorithm>
#include <tuple>
#include <utility>

#include "graphcontext.h"
#include "heldspaces.h"
#include "inkcodec.h"
#include "plugindata.h"
#include "spaceannotations.h"
#include "spacereducer.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

using AnnotationLayers::Annotation;
using AnnotationLayers::Person;

constexpr auto strokesField = "set:strokes"_L1;
constexpr auto annotationsPart = "annotations"_L1;

[[nodiscard]] QString text(const ReducedAnnotation &annotation, const QString &field) { return annotation.fields.value(field).value; }

// Whether a change of a set holds no more than the limits let one: an object of
// what it adds and removes, at most PluginData::maxItems of them.
[[nodiscard]] bool fairChange(const QString &value) {
    const QJsonDocument change = QJsonDocument::fromJson(value.toUtf8());
    if (!change.isObject())
        return false;
    const QJsonValue added = change.object().value("add");
    const QJsonValue removed = change.object().value("remove");
    if ((!added.isUndefined() && !added.isObject()) || (!removed.isUndefined() && !removed.isArray()))
        return false;
    return added.toObject().size() + removed.toArray().size() <= PluginData::maxItems;
}

// Whether an operation of another person's is within ADR 0002's limits.
[[nodiscard]] bool fair(const SpaceOperation &op) {
    if (op.value.toUtf8().size() > PluginData::maxValueBytes || op.id.size() > PluginData::maxIdLength
        || op.field.size() > PluginData::maxNameLength)
        return false;
    return op.field != strokesField || fairChange(op.value);
}

// The strokes of one annotation, of at most PluginData::maxItems, the earliest.
[[nodiscard]] std::vector<Ink::Stroke> strokesOf(const QList<SpaceOperation> &ops) {
    if (ops.isEmpty())
        return {};
    const QJsonArray things = PluginData::reduce(ops);
    const QJsonObject items = things.isEmpty() ? QJsonObject() : things.first()["sets"]["strokes"].toObject();
    Ink::Read read = Ink::fromItems(items);
    std::ranges::stable_sort(
        read.strokes, [](const Ink::Stroke &a, const Ink::Stroke &b) { return a.began != b.began ? a.began < b.began : a.id < b.id; });
    if (std::cmp_greater(read.strokes.size(), PluginData::maxItems))
        read.strokes.resize(static_cast<std::size_t>(PluginData::maxItems));
    return std::move(read.strokes);
}

[[nodiscard]] BookPlace placeOf(const ReducedAnnotation &annotation, const QString &section, const QString &fraction) {
    return {.section = text(annotation, section).toInt(), .fraction = text(annotation, fraction).toDouble()};
}

// One person's annotations of one space, as the reader may see them.
class Reading {
public:
    Reading(const Library &library, const HeldSpaces::Held &space, const GraphKnown &known, qint64 bookId)
        : m_library(&library), m_space(&space), m_known(&known), m_bookId(bookId), m_books(SpaceReducer::sameAs(space.ops)) {}

    // Of the operations `ops` (one person's), the annotations; how many of them were refused.
    qsizetype read(const QList<SpaceOperation> &ops, QList<Annotation> &into) {
        QList<SpaceOperation> taken;
        QHash<QString, QList<SpaceOperation>> strokes;
        qsizetype refused = 0;
        for (const SpaceOperation &op : ops) {
            if (!fair(op)) {
                ++refused;
                continue;
            }
            if (op.field == strokesField)
                strokes[op.id] << op;
            else
                taken << op;
        }
        const ReducedState state = SpaceReducer::reduce(taken);
        for (auto annotation = state.annotations.cbegin(); annotation != state.annotations.cend(); ++annotation)
            if (std::optional<Annotation> one = shown(annotation.key(), *annotation)) {
                one->strokes = strokesOf(strokes.value(annotation.key()));
                into << std::move(*one);
            }
        return refused;
    }

private:
    [[nodiscard]] qint64 here(const QString &bookGid) {
        const QString book = m_books.value(bookGid, bookGid);
        if (!m_here.contains(book))
            m_here.insert(book, SubscribedSpace::bookHere(*m_library, m_space->space.space, book));
        return m_here.value(book);
    }

    [[nodiscard]] std::optional<Annotation> shown(const QString &gid, const ReducedAnnotation &reduced) {
        const QString kind = text(reduced, u"kind"_s);
        if (SpaceReducer::isRemoved(reduced.alive, reduced.removed) || !SpaceAnnotations::travels(kind))
            return std::nullopt;
        const qint64 book = here(text(reduced, u"book"_s));
        if (book <= 0 || (m_bookId > 0 && book != m_bookId))
            return std::nullopt;
        Annotation annotation{.id = gid,
                              .kind = kind,
                              .book = book,
                              .place = placeOf(reduced, u"section"_s, u"fraction"_s),
                              .to = {},
                              .quote = AnnotationLayers::plain(text(reduced, u"quote"_s)),
                              .before = AnnotationLayers::plain(text(reduced, u"before"_s)),
                              .after = AnnotationLayers::plain(text(reduced, u"after"_s)),
                              .note = AnnotationLayers::plain(text(reduced, u"note"_s)),
                              .color = AnnotationLayers::plain(text(reduced, u"color"_s)),
                              .created = text(reduced, u"created"_s).toLongLong(),
                              .strokes = {}};
        annotation.to = reduced.fields.contains(u"to_section"_s) ? placeOf(reduced, u"to_section"_s, u"to_fraction"_s) : annotation.place;
        if (annotation.quote.isEmpty())
            annotation.quote = AnnotationLayers::plain(text(reduced, u"text"_s));
        // Never ahead of the reader: the whole of it lies where they have been.
        if (!(*m_known)(GraphPlace{book, std::max(annotation.place, annotation.to)}))
            return std::nullopt;
        return annotation;
    }

    // All outlive the reading.
    const Library *m_library;
    const HeldSpaces::Held *m_space;
    const GraphKnown *m_known;
    qint64 m_bookId;
    QHash<QString, QString> m_books;
    QHash<QString, qint64> m_here;
};

// Of a space, each person's operations of annotations: of a published space
// all its owner's; of a shared one with its part on, each writer's but this
// library's own.
[[nodiscard]] QMap<QString, QList<SpaceOperation>> byPerson(const HeldSpaces::Held &space) {
    QMap<QString, QList<SpaceOperation>> people;
    const bool shared = space.kind == "shared"_L1;
    if (shared && !space.parts.contains(annotationsPart))
        return people;
    for (const SpaceOperation &op : space.ops) {
        if (op.entity != "annotation"_L1)
            continue;
        const QString person = shared ? op.author : space.space.owner;
        if (!person.isEmpty() && person != space.me)
            people[person] << op;
    }
    return people;
}

[[nodiscard]] Person &personIn(QList<Person> &people, const QString &id) {
    const auto found = std::ranges::find(people, id, &Person::person);
    if (found != people.end())
        return *found;
    people << Person{.person = id, .name = {}, .spaces = {}, .annotations = {}};
    return people.last();
}

[[nodiscard]] bool inOrder(const Annotation &a, const Annotation &b) {
    return std::tie(a.book, a.place, a.id) < std::tie(b.book, b.place, b.id);
}

} // namespace

AnnotationLayers::Layers AnnotationLayers::read(const Library &library, const GraphKnown &known, qint64 bookId) {
    Layers layers;
    for (const HeldSpaces::Held &space : HeldSpaces::held(library)) {
        const QMap<QString, QList<SpaceOperation>> people = byPerson(space);
        Reading reading(library, space, known, bookId);
        for (auto ops = people.cbegin(); ops != people.cend(); ++ops) {
            Person &person = personIn(layers.people, ops.key());
            layers.refused += reading.read(*ops, person.annotations);
            person.spaces << space.space.space;
            if (person.name.isEmpty())
                person.name = space.kind == "shared"_L1 ? plain(HeldSpaces::memberName(library, space.space.space, ops.key()))
                                                        : plain(space.space.ownerName);
        }
    }
    // Of a person with nothing the reader has reached, nothing is said.
    layers.people.removeIf([](const Person &person) { return person.annotations.isEmpty(); });
    for (Person &person : layers.people) {
        std::ranges::sort(person.annotations, inOrder);
        // One annotation carried by two spaces they share with this library is shown once.
        const auto same = std::ranges::unique(person.annotations, [](const Annotation &a, const Annotation &b) { return a.id == b.id; });
        person.annotations.erase(same.begin(), same.end());
    }
    std::ranges::sort(layers.people, [](const Person &a, const Person &b) {
        return std::tuple(a.name.isEmpty(), a.name.toLower(), a.person) < std::tuple(b.name.isEmpty(), b.name.toLower(), b.person);
    });
    return layers;
}

AnnotationLayers::Layers AnnotationLayers::read(const Library &library, qint64 bookId) {
    return read(library, GraphContext::readerKnows(library), bookId);
}

QColor AnnotationLayers::colorOf(const QString &person) {
    const QByteArray hash = QCryptographicHash::hash(person.toUtf8(), QCryptographicHash::Sha1);
    const int hue = (static_cast<quint8>(hash.at(0)) * 256 + static_cast<quint8>(hash.at(1))) % 360;
    return QColor::fromHsl(hue, 160, 120);
}

QString AnnotationLayers::calledOf(const Person &person) { return person.name.isEmpty() ? person.person.left(8) : person.name; }

QString AnnotationLayers::plain(const QString &words) {
    QString kept;
    kept.reserve(words.size());
    for (const QChar c : words) {
        const char16_t unit = c.unicode();
        const bool turns =
            (unit >= 0x202A && unit <= 0x202E) || (unit >= 0x2066 && unit <= 0x2069) || unit == 0x200E || unit == 0x200F || unit == 0x061C;
        const bool control = c.category() == QChar::Other_Control && unit != u'\n' && unit != u'\t';
        if (!turns && !control)
            kept += c;
    }
    return kept;
}

QJsonObject AnnotationLayers::toJson(const Person &person) {
    QJsonArray annotations;
    for (const Annotation &one : person.annotations)
        annotations << one.place.toJson({{"id", one.id},
                                         {"kind", one.kind},
                                         {"book", one.book},
                                         {"to", one.to.toJson()},
                                         {"quote", one.quote},
                                         {"note", one.note},
                                         {"color", one.color},
                                         {"strokes", static_cast<qint64>(one.strokes.size())}});
    return {{"person", person.person},
            {"name", calledOf(person)},
            {"color", colorOf(person.person).name()},
            {"spaces", QJsonArray::fromStringList(person.spaces)},
            {"annotations", annotations}};
}

QJsonArray AnnotationLayers::toJson(const Layers &layers) {
    QJsonArray people;
    for (const Person &person : layers.people)
        people << toJson(person);
    return people;
}
