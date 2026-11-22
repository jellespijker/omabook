#include "readinglineink.h"

#include <QAbstractTextDocumentLayout>
#include <QJsonObject>
#include <QQuickTextDocument>
#include <QTextDocument>

#include <algorithm>
#include <cmath>
#include <map>

#include "annotationanchors.h"
#include "annotationlayerview.h"
#include "backend.h"
#include "booktext.h"
#include "inkcodec.h"
#include "inksurface.h"
#include "library.h"
#include "reader.h"
#include "readingink.h"

namespace {

// How much lighter an ink is drawn on a dark page.
constexpr int lighterOnDark = 165;
// How far a stroke drawn again may stand from where a surface has it, in pixels, and be the same.
constexpr double stillThere = 0.5;

[[nodiscard]] Ink::Stroke movedBy(Ink::Stroke stroke, QPointF by) {
    for (Ink::Point &point : stroke.points)
        point.at += by;
    return stroke;
}

[[nodiscard]] bool same(const Ink::Stroke &one, const Ink::Stroke &other) {
    const QRectF a = Ink::bounds(one);
    const QRectF b = Ink::bounds(other);
    return one.points.size() == other.points.size() && one.color == other.color && std::abs(a.left() - b.left()) < stillThere
           && std::abs(a.top() - b.top()) < stillThere && std::abs(a.right() - b.right()) < stillThere
           && std::abs(a.bottom() - b.bottom()) < stillThere;
}

[[nodiscard]] QStringList idsOf(const std::vector<Ink::Stroke> &strokes) {
    QStringList ids;
    for (const Ink::Stroke &stroke : strokes)
        ids << stroke.id;
    return ids;
}

// Whether a stroke anchored to `fresh` joins the strokes already on `there`:
// beside the same line in a margin, at some of the same words among the lines.
[[nodiscard]] bool joins(const InkLines::Anchor &there, const InkLines::Anchor &fresh) {
    if (there.where != fresh.where)
        return false;
    if (there.where == InkLines::Where::Margin)
        return there.start >= fresh.start && there.start < fresh.end;
    return there.start < fresh.end && fresh.start < there.end;
}

[[nodiscard]] const QTextDocument *documentOf(const QQuickItem *text) {
    const auto *chapter = text ? text->property("textDocument").value<QQuickTextDocument *>() : nullptr;
    return chapter ? chapter->textDocument() : nullptr;
}

} // namespace

ReadingLineInk::ReadingLineInk(Reader *reader, ReadingInk *mode) : QObject(reader), m_reader(reader), m_mode(mode) {
    m_relaid.setSingleShot(true);
    m_relaid.setInterval(0);
    connect(&m_relaid, &QTimer::timeout, this, &ReadingLineInk::relay);
    connect(reader, &Reader::bookChanged, this, [this] {
        clear();
        reread();
        redrawAll(true);
        emit changed();
    });
    connect(reader, &Reader::sectionChanged, &m_relaid, qOverload<>(&QTimer::start));
    if (const Backend *backend = reader->backend())
        connect(backend, &Backend::revisionChanged, this, &ReadingLineInk::refresh);
    connect(mode, &ReadingInk::modeChanged, this, [this] {
        if (!m_mode->inking())
            flushAll();
        emit lookChanged();
    });
}

void ReadingLineInk::setForeground(const QColor &color) {
    if (color != m_foreground) {
        m_foreground = color;
        redrawAll(true);
        emit lookChanged();
    }
}

void ReadingLineInk::setBackground(const QColor &color) {
    if (color != m_background) {
        m_background = color;
        redrawAll(true);
        emit lookChanged();
    }
}

QColor ReadingLineInk::penColor() const { return shownColor(m_mode->penColor()); }

QColor ReadingLineInk::shownColor(QColor kept) const {
    const QVariantList inks = ReadingInk::colors();
    const bool dark = m_background.lightnessF() < 0.5;
    for (qsizetype at = 0; at < inks.size(); ++at) {
        const auto ink = inks.at(at).toMap().value("color").value<QColor>();
        if (ink.rgb() != kept.rgb())
            continue;
        // The first ink is the text's own colour, on any page.
        QColor shown = ink;
        if (at == 0)
            shown = m_foreground;
        else if (dark)
            shown = ink.lighter(lighterOnDark);
        shown.setAlphaF(kept.alphaF());
        return shown;
    }
    return kept;
}

QColor ReadingLineInk::keptColor(QColor shown) const {
    for (const QVariant &one : ReadingInk::colors()) {
        auto ink = one.toMap().value("color").value<QColor>();
        if (shownColor(ink).rgb() == shown.rgb()) {
            ink.setAlphaF(shown.alphaF());
            return ink;
        }
    }
    return shown;
}

void ReadingLineInk::show(QQuickItem *surface, QQuickItem *text) {
    auto *ink = qobject_cast<InkSurface *>(surface);
    if (!ink || !text)
        return;
    forget(surface);
    m_shown.append({.surface = ink, .text = text, .others = {}});
    // Laid out again: the surface is drawn again once it is, for as long as it is there.
    const auto again = [this] { m_relaid.start(); };
    const QObject *whileShown = ink;
    connect(text, &QQuickItem::widthChanged, whileShown, again);
    connect(ink, &QQuickItem::xChanged, this, again);
    connect(ink, &QQuickItem::widthChanged, this, again);
    if (const QTextDocument *document = documentOf(text)) {
        connect(document, &QTextDocument::contentsChanged, whileShown, again);
        connect(document->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, whileShown, again);
    }
    redraw(m_shown.last(), true);
}

void ReadingLineInk::showTheirs(QQuickItem *surface, QQuickItem *text, QObject *others) {
    auto *layers = qobject_cast<AnnotationLayerView *>(others);
    show(surface, text);
    if (!layers || m_shown.isEmpty() || m_shown.last().surface != surface)
        return;
    m_shown.last().others = layers;
    connect(layers, &AnnotationLayerView::changed, surface, [this] { m_relaid.start(); });
    redraw(m_shown.last(), true);
}

void ReadingLineInk::forget(QQuickItem *surface) {
    m_shown.removeIf([surface](const Shown &shown) { return shown.surface.isNull() || shown.text.isNull() || shown.surface == surface; });
}

void ReadingLineInk::relay() {
    flushAll();
    redrawAll(true);
}

std::optional<ReadingLineInk::View> ReadingLineInk::viewOf(const Shown &shown) const {
    const QTextDocument *document = shown.surface && shown.text ? documentOf(shown.text) : nullptr;
    if (!document)
        return std::nullopt;
    View view{.document = document, .shown = document->toPlainText(), .column = {}, .offset = {}, .lines = {}, .placed = {}};
    // Where the surface stands on the text, which reaches past it into the margins.
    view.offset = shown.surface->mapToItem(shown.text, QPointF(0, 0));
    const double width = shown.text->width();
    view.column = {.left = 0,
                   .right = width,
                   .leftRoom = std::max(0.0, -view.offset.x()),
                   .rightRoom = std::max(0.0, view.offset.x() + shown.surface->width() - width)};
    const int section = m_reader->section();
    if (shown.others) {
        view.lines = shown.others->lineInk(section);
    } else {
        for (const LineInk::Line &line : m_lines)
            if (line.passage.from.section == section)
                view.lines << line;
    }
    view.placed = LineInk::place(*document, view.shown, view.column, view.lines);
    return view;
}

std::vector<Ink::Stroke> ReadingLineInk::drawn(const View &view) const {
    std::vector<Ink::Stroke> all;
    for (const LineInk::Placed &placed : view.placed)
        for (const Ink::Stroke &kept : view.lines.at(placed.index).strokes) {
            Ink::Stroke stroke = movedBy(placed.placing.shown(kept), -view.offset);
            stroke.color = shownColor(stroke.color);
            all.push_back(std::move(stroke));
        }
    return all;
}

void ReadingLineInk::redraw(const Shown &shown, bool whole) {
    const std::optional<View> view = viewOf(shown);
    if (!view)
        return;
    std::vector<Ink::Stroke> wanted = drawn(*view);
    if (whole) {
        shown.surface->replace(std::move(wanted));
        return;
    }
    // Only what moved or came or went, so what the pen holds (a lasso's selection) stays.
    const std::vector<Ink::Stroke> &there = shown.surface->sheet().strokes();
    Ink::Change change;
    for (const Ink::Stroke &stroke : there) {
        const auto found = std::ranges::find(wanted, stroke.id, &Ink::Stroke::id);
        if (found == wanted.cend() || !same(*found, stroke))
            change.removed << stroke.id;
    }
    for (Ink::Stroke &stroke : wanted) {
        const auto found = std::ranges::find(there, stroke.id, &Ink::Stroke::id);
        if (found == there.cend() || !same(*found, stroke))
            change.added.push_back(std::move(stroke));
    }
    if (!change.empty())
        shown.surface->applyChange(change);
}

void ReadingLineInk::redrawAll(bool whole) {
    forget(nullptr);
    // A copy: drawing again sends what a surface held back, which is taken at once.
    const QList<Shown> shown = m_shown;
    for (const Shown &one : shown)
        redraw(one, whole);
}

std::optional<ReadingLineInk::Step> ReadingLineInk::partsOf(const View &view, const Ink::Change &change) const {
    Step parts;
    const auto partOf = [&parts, &view](qsizetype line) {
        const LineInk::Line &of = view.lines.at(line);
        const auto found = std::ranges::find(parts, of.id, &Part::id);
        if (found != parts.end())
            return std::distance(parts.begin(), found);
        parts.push_back({.id = of.id, .kind = of.kind, .passage = {.passage = of.passage, .confidence = 1}, .added = {}, .removed = {}});
        return static_cast<std::ptrdiff_t>(parts.size()) - 1;
    };
    for (qsizetype line = 0; line < view.lines.size(); ++line)
        for (const Ink::Stroke &stroke : view.lines.at(line).strokes)
            if (change.removed.contains(stroke.id))
                parts[static_cast<std::size_t>(partOf(line))].removed.push_back(stroke);
    // The lines begun by this change: where they are, and the part that keeps each.
    std::vector<std::pair<LineInk::Placed, std::size_t>> begun;
    const int section = m_reader->section();
    const BookText text(m_reader->count(),
                        [reader = m_reader, section](int asked) { return asked == section ? reader->sectionText() : QString(); });
    for (const Ink::Stroke &added : change.added) {
        Ink::Stroke stroke = movedBy(added, view.offset);
        stroke.color = keptColor(stroke.color);
        const std::optional<InkLines::Anchor> anchor = InkLines::anchorOf(*view.document, view.column, Ink::bounds(stroke));
        if (!anchor)
            continue;
        const auto there = std::ranges::find_if(view.placed, [&anchor](const LineInk::Placed &one) { return joins(one.anchor, *anchor); });
        if (there != view.placed.cend()) {
            parts[static_cast<std::size_t>(partOf(there->index))].added.push_back(there->placing.kept(std::move(stroke)));
            continue;
        }
        const auto fresh = std::ranges::find_if(begun, [&anchor](const auto &one) { return joins(one.first.anchor, *anchor); });
        if (fresh != begun.cend()) {
            parts[fresh->second].added.push_back(fresh->first.placing.kept(std::move(stroke)));
            continue;
        }
        const auto placing = InkLines::Placing::of(*view.document, view.column, *anchor, {});
        const auto passage = Passage::shownAt(view.shown, section, anchor->start, anchor->end, text);
        if (!placing || !passage)
            continue;
        parts.push_back({.id = 0,
                         .kind = LineInk::kindOf(anchor->where),
                         .passage = *passage,
                         .added = {placing->kept(std::move(stroke))},
                         .removed = {}});
        begun.emplace_back(LineInk::Placed{.index = -1, .anchor = *anchor, .placing = *placing}, parts.size() - 1);
    }
    if (parts.empty())
        return std::nullopt;
    return parts;
}

bool ReadingLineInk::apply(Part &part, const Ink::Change &change) {
    Library *library = m_reader->library();
    if (!library || m_reader->bookId() <= 0)
        return false;
    if (change.empty() || (part.id > 0 && LineInk::write(*library, part.id, change)))
        return true;
    // Its line went with its last stroke: what comes back to it is a line again.
    if (change.added.empty())
        return true;
    const std::optional<qint64> made = LineInk::make(*library, m_reader->bookId(), part.kind, part.passage, fileHash(), change.added);
    part.id = made.value_or(0);
    return made.has_value();
}

bool ReadingLineInk::perform(Step &step, bool forward) {
    const Library *library = m_reader->library();
    if (!library)
        return false;
    const Library::Batch batch(*library);
    bool kept = true;
    for (Part &part : step) {
        const Ink::Change change = forward ? Ink::Change{.added = part.added, .removed = idsOf(part.removed)}
                                           : Ink::Change{.added = part.removed, .removed = idsOf(part.added)};
        kept = apply(part, change) && kept;
    }
    return kept;
}

bool ReadingLineInk::take(QQuickItem *surface, const QVariantMap &batch) {
    const auto *ink = qobject_cast<const InkSurface *>(surface);
    const auto change = Ink::fromBatch(QJsonObject::fromVariantMap(batch));
    if (!change) {
        qWarning("Free ink: a batch not taken: %s", qPrintable(change.error()));
        return false;
    }
    return ink && take(*ink, *change);
}

bool ReadingLineInk::take(const InkSurface &surface, const Ink::Change &change) {
    const auto shown = std::ranges::find(m_shown, &surface, [](const Shown &one) { return one.surface.data(); });
    const std::optional<View> view = shown != m_shown.cend() && !shown->others ? viewOf(*shown) : std::nullopt;
    std::optional<Step> step = view ? partsOf(*view, change) : std::nullopt;
    const bool kept = step && perform(*step, true);
    if (step) {
        m_undo.push_back(std::move(*step));
        m_redo.clear();
    }
    reread();
    // What the pen did stands where its lines now have it; what couldn't be kept goes.
    redrawAll(false);
    emit changed();
    return kept;
}

bool ReadingLineInk::undo() {
    // What the pen did and hasn't sent yet is the step to undo.
    flushAll();
    if (m_undo.empty())
        return false;
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    const bool done = perform(step, false);
    m_redo.push_back(std::move(step));
    reread();
    redrawAll(false);
    emit changed();
    return done;
}

bool ReadingLineInk::redo() {
    flushAll();
    if (m_redo.empty())
        return false;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    const bool done = perform(step, true);
    m_undo.push_back(std::move(step));
    reread();
    redrawAll(false);
    emit changed();
    return done;
}

bool ReadingLineInk::eraseSection(int section) {
    flushAll();
    Step step;
    for (const LineInk::Line &line : std::as_const(m_lines))
        if (!line.lost && line.passage.from.section == section)
            step.push_back({.id = line.id,
                            .kind = line.kind,
                            .passage = {.passage = line.passage, .confidence = 1},
                            .added = {},
                            .removed = line.strokes});
    if (step.empty())
        return false;
    const bool done = perform(step, true);
    m_undo.push_back(std::move(step));
    m_redo.clear();
    reread();
    redrawAll(false);
    emit changed();
    return done;
}

void ReadingLineInk::refresh() {
    reread();
    redrawAll(false);
    emit changed();
}

bool ReadingLineInk::touched(qulonglong at) const {
    return std::ranges::any_of(m_shown, [at](const Shown &one) { return one.surface && !one.others && one.surface->touched(at); });
}

void ReadingLineInk::reread() {
    m_lines.clear();
    m_list.clear();
    const Library *library = m_reader->library();
    if (!library || m_reader->bookId() <= 0 || m_reader->kind() != QLatin1StringView("flow"))
        return;
    m_lines = LineInk::lines(*library, m_reader->bookId());
    std::map<int, QList<const LineInk::Line *>> bySection;
    for (const LineInk::Line &line : std::as_const(m_lines))
        if (!line.lost)
            bySection[line.passage.from.section] << &line;
    for (const auto &[section, lines] : bySection) {
        const auto *first = *std::ranges::min_element(lines, {}, [](const LineInk::Line *line) { return line->passage.from.fraction; });
        qsizetype strokes = 0;
        for (const LineInk::Line *line : lines)
            strokes += static_cast<qsizetype>(line->strokes.size());
        m_list << QVariantMap{{"id", section},
                              {"section", section},
                              {"fraction", first->passage.from.fraction},
                              {"quote", first->passage.quote.simplified()},
                              {"lines", lines.size()},
                              {"strokes", strokes}};
    }
}

void ReadingLineInk::flushAll() {
    // A copy: a batch sent is taken at once, and the list may change meanwhile.
    const QList<Shown> shown = m_shown;
    for (const Shown &one : shown)
        if (one.surface)
            one.surface->flush();
}

void ReadingLineInk::clear() {
    m_undo.clear();
    m_redo.clear();
}

QString ReadingLineInk::fileHash() const {
    const Library *library = m_reader->library();
    const auto file = library ? AnnotationAnchors::readableFile(*library, m_reader->bookId()) : std::nullopt;
    return file ? file->hash : QString();
}
