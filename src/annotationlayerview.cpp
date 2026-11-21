#include "annotationlayerview.h"

#include <QTextDocument>
#include <QVariantMap>
#include <algorithm>
#include <utility>

#include "annotationfacts.h"
#include "inksurface.h"
#include "marginnotes.h"
#include "reader.h"
#include "settings.h"
#include "textgeometry.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QVariantList variants(const QList<QRectF> &rects) {
    QVariantList list;
    for (const QRectF &rect : rects)
        list << rect;
    return list;
}

// Their strokes, in their colour: what tells their ink from one's own, and
// one person's from another's. Each id is made theirs, so two people's never meet.
void addTheirs(std::vector<Ink::Stroke> &into, const AnnotationLayers::Person &person, const AnnotationLayers::Annotation &annotation) {
    const QColor color = AnnotationLayers::colorOf(person.person);
    for (Ink::Stroke stroke : annotation.strokes) {
        stroke.id = person.person.left(12) + u':' + stroke.id;
        stroke.color = color;
        into.push_back(std::move(stroke));
    }
}

[[nodiscard]] QString shownText(const QQuickTextDocument *chapter) {
    const QTextDocument *document = chapter != nullptr ? chapter->textDocument() : nullptr;
    return document != nullptr ? document->toPlainText() : QString();
}

} // namespace

AnnotationLayerView::AnnotationLayerView(QObject *parent) : QObject(parent) {}

QObject *AnnotationLayerView::reader() const { return m_reader; }

void AnnotationLayerView::setReader(QObject *reader) {
    if (m_reader == reader)
        return;
    if (m_reader)
        disconnect(m_reader, nullptr, this, nullptr);
    m_reader = qobject_cast<Reader *>(reader);
    if (m_reader) {
        connect(m_reader, &Reader::bookChanged, this, &AnnotationLayerView::refresh);
        // A chapter further on may bring what was past the reader's place before.
        connect(m_reader, &Reader::sectionChanged, this, &AnnotationLayerView::refresh);
    }
    refresh();
}

QObject *AnnotationLayerView::settings() const { return m_settings; }

void AnnotationLayerView::setSettings(QObject *settings) {
    if (m_settings == settings)
        return;
    if (m_settings)
        disconnect(m_settings, nullptr, this, nullptr);
    m_settings = qobject_cast<Settings *>(settings);
    if (m_settings)
        connect(m_settings, &Settings::changed, this, [this] {
            ++m_revision;
            redraw();
            emit changed();
        });
    ++m_revision;
    emit changed();
}

bool AnnotationLayerView::isShown(const QString &person) const {
    return !m_settings || (m_settings->othersAnnotations() && !m_settings->hiddenAnnotators().contains(person));
}

QVariantList AnnotationLayerView::people() const {
    QVariantList list;
    const QStringList hidden = m_settings ? m_settings->hiddenAnnotators() : QStringList();
    for (const AnnotationLayers::Person &person : m_layers.people)
        if (!person.annotations.isEmpty())
            list << QVariantMap{{"person", person.person},
                                {"called", AnnotationLayers::calledOf(person)},
                                {"color", AnnotationLayers::colorOf(person.person)},
                                {"count", person.annotations.size()},
                                {"shown", !hidden.contains(person.person)}};
    return list;
}

bool AnnotationLayerView::shown() const { return !m_settings || m_settings->othersAnnotations(); }

void AnnotationLayerView::setShown(bool shown) {
    if (m_settings)
        m_settings->setOthersAnnotations(shown);
}

int AnnotationLayerView::count() const {
    qsizetype shown = 0;
    for (const AnnotationLayers::Person &person : m_layers.people)
        if (isShown(person.person))
            shown += person.annotations.size();
    return static_cast<int>(shown);
}

int AnnotationLayerView::notes() const {
    qsizetype shown = 0;
    for (const AnnotationLayers::Person &person : m_layers.people)
        if (isShown(person.person))
            shown += std::ranges::count(person.annotations, u"margin-note"_s, &AnnotationLayers::Annotation::kind);
    return static_cast<int>(shown);
}

void AnnotationLayerView::setPersonShown(const QString &person, bool shown) {
    if (m_settings)
        m_settings->setAnnotatorHidden(person, !shown);
}

QList<AnnotationLayerView::Placed> AnnotationLayerView::placedIn(const QString &shown) const {
    QList<Placed> placed;
    if (!m_reader || shown.isEmpty())
        return placed;
    const int section = m_reader->section();
    for (const AnnotationLayers::Person &person : m_layers.people) {
        if (!isShown(person.person))
            continue;
        for (const AnnotationLayers::Annotation &annotation : person.annotations) {
            if (annotation.kind == "ink"_L1 || annotation.place.section != section)
                continue;
            if (const auto span = annotation.passage().spanIn(shown))
                placed << Placed{.person = &person, .annotation = &annotation, .start = span->first, .end = span->second};
        }
    }
    return placed;
}

QVariantList AnnotationLayerView::passagesIn(QQuickTextDocument *chapter) const {
    QVariantList list;
    const QTextDocument *document = chapter != nullptr ? chapter->textDocument() : nullptr;
    if (document == nullptr)
        return list;
    for (const Placed &one : placedIn(document->toPlainText()))
        list << QVariantMap{{"id", one.annotation->id},
                            {"kind", one.annotation->kind},
                            {"person", one.person->person},
                            {"called", AnnotationLayers::calledOf(*one.person)},
                            {"color", AnnotationLayers::colorOf(one.person->person)},
                            {"rects", variants(TextGeometry::rects(*document, static_cast<int>(one.start), static_cast<int>(one.end)))}};
    return list;
}

QVariantList AnnotationLayerView::notesIn(QQuickTextDocument *chapter) const {
    QVariantList list;
    for (const Placed &one : placedIn(shownText(chapter)))
        if (one.annotation->kind == "margin-note"_L1)
            list << QVariantMap{{"id", one.annotation->id},
                                {"start", one.start},
                                {"end", one.end},
                                {"note", one.annotation->note},
                                {"ink", !one.annotation->strokes.empty()},
                                {"person", one.person->person},
                                {"called", AnnotationLayers::calledOf(*one.person)},
                                {"color", AnnotationLayers::colorOf(one.person->person)}};
    return list;
}

QString AnnotationLayerView::makeFact(const QString &id, const QString &thing, const QString &kind) {
    if (!m_reader || m_reader->library() == nullptr)
        return {};
    const Library &library = *m_reader->library();
    const auto source = AnnotationFacts::of(library, id);
    if (!source)
        return source.error().text();
    const auto made = AnnotationFacts::make(library, *source, {.which = thing, .kind = kind});
    return made ? QString() : made.error().text();
}

std::vector<Ink::Stroke> AnnotationLayerView::strokesOf(const Drawn &drawn) const {
    std::vector<Ink::Stroke> strokes;
    for (const AnnotationLayers::Person &person : m_layers.people) {
        if (!isShown(person.person))
            continue;
        for (const AnnotationLayers::Annotation &annotation : person.annotations) {
            const bool wanted =
                drawn.note.isEmpty() ? annotation.kind == "ink"_L1 && annotation.place.section == drawn.page : annotation.id == drawn.note;
            if (wanted)
                addTheirs(strokes, person, annotation);
        }
    }
    std::ranges::stable_sort(strokes, {}, &Ink::Stroke::began);
    return strokes;
}

std::vector<Ink::Stroke> AnnotationLayerView::inkOn(int page) const { return strokesOf({.page = page, .note = {}, .surface = {}}); }

std::vector<Ink::Stroke> AnnotationLayerView::noteInk(const QString &id) const {
    return id.isEmpty() ? std::vector<Ink::Stroke>() : strokesOf({.page = -1, .note = id, .surface = {}});
}

void AnnotationLayerView::showInk(QQuickItem *surface, int page) {
    auto *ink = qobject_cast<InkSurface *>(surface);
    if (ink == nullptr)
        return;
    forgetInk(ink);
    m_drawn << Drawn{.page = page, .note = {}, .surface = ink};
    ink->replace(strokesOf(m_drawn.last()));
}

void AnnotationLayerView::showNoteInk(QQuickItem *surface, const QString &id) {
    auto *ink = qobject_cast<InkSurface *>(surface);
    if (ink == nullptr || id.isEmpty())
        return;
    forgetInk(ink);
    m_drawn << Drawn{.page = -1, .note = id, .surface = ink};
    ink->replace(strokesOf(m_drawn.last()));
}

double AnnotationLayerView::inkBottom(QQuickItem *surface) const {
    const auto *ink = qobject_cast<InkSurface *>(surface);
    double bottom = 0;
    if (ink != nullptr)
        for (const Ink::Stroke &stroke : ink->sheet().strokes())
            bottom = std::max(bottom, Ink::bounds(stroke).bottom());
    return bottom;
}

double AnnotationLayerView::noteInkUnits() const { return MarginNotes::inkWidth; }

void AnnotationLayerView::forgetInk(QQuickItem *surface) {
    m_drawn.removeIf([surface](const Drawn &drawn) { return drawn.surface.isNull() || drawn.surface == surface; });
}

void AnnotationLayerView::redraw() {
    m_drawn.removeIf([](const Drawn &drawn) { return drawn.surface.isNull(); });
    for (const Drawn &drawn : std::as_const(m_drawn))
        drawn.surface->replace(strokesOf(drawn));
}

void AnnotationLayerView::refresh() {
    m_layers = {};
    if (m_reader && m_reader->library() != nullptr && m_reader->bookId() > 0)
        m_layers = AnnotationLayers::read(*m_reader->library(), m_reader->bookId());
    ++m_revision;
    redraw();
    emit changed();
}
