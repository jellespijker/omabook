#include "readingink.h"

#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <array>
#include <iterator>
#include <numbers>

#include "annotationanchors.h"
#include "backend.h"
#include "bookink.h"
#include "bookinkwords.h"
#include "inkcodec.h"
#include "inksurface.h"
#include "reader.h"
#include "settings.h"

namespace {

struct InkColor {
    const char *key;
    const char *color;
};

// Inks that read on a white page, which a PDF's and a comic's are.
constexpr std::array<InkColor, 4> inks{{{"black", "#1a1a1a"}, {"blue", "#1d4ed8"}, {"red", "#c62828"}, {"green", "#2e7d32"}}};

// A page whose height isn't known yet is taken for a printed book's (A5, A4: the square root of two).
constexpr double usualAspect = std::numbers::sqrt2;

[[nodiscard]] QSet<QString> idsOf(const std::vector<Ink::Stroke> &strokes) {
    QSet<QString> ids;
    for (const Ink::Stroke &stroke : strokes)
        ids.insert(stroke.id);
    return ids;
}

// What turns `before` into `after`, by the strokes' ids: a stroke never changes, it is erased and its pieces added.
[[nodiscard]] Ink::Change between(const std::vector<Ink::Stroke> &before, const std::vector<Ink::Stroke> &after) {
    const QSet<QString> had = idsOf(before);
    const QSet<QString> has = idsOf(after);
    Ink::Change change;
    for (const Ink::Stroke &stroke : after)
        if (!had.contains(stroke.id))
            change.added.push_back(stroke);
    for (const Ink::Stroke &stroke : before)
        if (!has.contains(stroke.id))
            change.removed << stroke.id;
    return change;
}

} // namespace

ReadingInk::ReadingInk(Reader *reader) : QObject(reader), m_reader(reader) {
    connect(reader, &Reader::bookChanged, this, [this] {
        clear();
        // The pages already shown (the book read anew) show what it has now;
        // another book's go with their pages.
        forget(nullptr);
        for (const auto &[page, surface] : std::as_const(m_surfaces))
            surface->replace(strokesOn(page));
        readList();
        emit changed();
    });
    if (const Backend *backend = reader->backend())
        connect(backend, &Backend::revisionChanged, this, &ReadingInk::refresh);
    if (Backend *backend = reader->backend(); backend && backend->settings()) {
        m_settings = backend->settings();
        m_words = new BookInkWords(*m_settings, [reader] { return reader->library(); }, this);
        connect(m_settings, &Settings::changed, this, &ReadingInk::fingersChanged);
    }
}

void ReadingInk::setInking(bool inking) {
    if (inking == m_inking)
        return;
    m_inking = inking;
    // Writing on the pages shows what is on them.
    if (inking)
        m_shown = true;
    else
        flushAll();
    emit modeChanged();
}

void ReadingInk::setShown(bool shown) {
    if (shown == m_shown)
        return;
    m_shown = shown;
    // Ink put away is no ink being written.
    if (!shown && m_inking) {
        flushAll();
        m_inking = false;
    }
    emit modeChanged();
}

void ReadingInk::setErasing(bool erasing) {
    if (erasing != m_erasing) {
        m_erasing = erasing;
        emit modeChanged();
    }
}

void ReadingInk::setColor(const QString &color) {
    if (color != m_color && std::ranges::any_of(inks, [&color](const InkColor &ink) { return color == QLatin1StringView(ink.key); })) {
        m_color = color;
        emit modeChanged();
    }
}

QColor ReadingInk::penColor() const {
    const auto *const ink = std::ranges::find_if(inks, [this](const InkColor &one) { return m_color == QLatin1StringView(one.key); });
    return {QLatin1StringView(ink != inks.end() ? ink->color : inks.front().color)};
}

QString ReadingInk::fingers() const { return m_settings ? m_settings->inkFingers() : QString("auto"); }

void ReadingInk::setFingers(const QString &fingers) {
    if (m_settings)
        m_settings->setInkFingers(fingers);
}

InkSurface::FingerUse ReadingInk::fingerUse() const {
    const QString mode = fingers();
    if (mode == "on")
        return InkSurface::FingerUse::Draw;
    if (mode == "off")
        return InkSurface::FingerUse::Scroll;
    return InkSurface::FingerUse::Auto;
}

bool ReadingInk::touched(qulonglong at) const {
    return std::ranges::any_of(m_surfaces, [at](const auto &one) { return one.second && one.second->touched(at); });
}

QVariantList ReadingInk::colors() {
    QVariantList list;
    for (const InkColor &ink : inks)
        list << QVariantMap{{"key", QString::fromLatin1(ink.key)}, {"color", QColor(QLatin1StringView(ink.color))}};
    return list;
}

double ReadingInk::pageUnits() { return BookInk::pageUnits; }

QVariantList ReadingInk::pages() const { return m_list; }

void ReadingInk::show(QQuickItem *surface, int page) {
    auto *ink = qobject_cast<InkSurface *>(surface);
    if (!ink || page < 0)
        return;
    forget(surface);
    m_surfaces.append({page, ink});
    ink->replace(strokesOn(page));
}

void ReadingInk::forget(QQuickItem *surface) {
    m_surfaces.removeIf([surface](const auto &shown) { return shown.second.isNull() || shown.second == surface; });
}

bool ReadingInk::take(QQuickItem *surface, const QVariantMap &batch) {
    const auto shown = std::ranges::find_if(m_surfaces, [surface](const auto &one) { return one.second == surface; });
    if (shown == m_surfaces.cend()) {
        qWarning("Book ink: a batch from a surface that shows no page, not kept");
        return false;
    }
    const auto change = Ink::fromBatch(QJsonObject::fromVariantMap(batch));
    if (!change) {
        qWarning("Book ink: a batch not taken: %s", qPrintable(change.error()));
        return false;
    }
    return record(shown->first, *change, shown->second.data());
}

bool ReadingInk::take(int page, const Ink::Change &change) { return record(page, change, nullptr); }

bool ReadingInk::record(int page, const Ink::Change &change, const InkSurface *from) {
    Step step{.page = page, .added = change.added, .removed = {}};
    for (const Ink::Stroke &stroke : strokesOn(page))
        if (change.removed.contains(stroke.id))
            step.removed.push_back(stroke);
    if (!keep(page, change, from))
        return false;
    m_undo.push_back(std::move(step));
    m_redo.clear();
    emit changed();
    return true;
}

bool ReadingInk::undo() {
    // What the pen did and hasn't sent yet is the step to undo.
    flushAll();
    if (m_undo.empty())
        return false;
    Step step = std::move(m_undo.back());
    m_undo.pop_back();
    const Step undone = inverse(step);
    const bool kept = keep(undone.page, changeOf(undone), nullptr);
    m_redo.push_back(std::move(step));
    emit changed();
    return kept;
}

bool ReadingInk::redo() {
    flushAll();
    if (m_redo.empty())
        return false;
    Step step = std::move(m_redo.back());
    m_redo.pop_back();
    const bool kept = keep(step.page, changeOf(step), nullptr);
    m_undo.push_back(std::move(step));
    emit changed();
    return kept;
}

bool ReadingInk::erasePage(int page) {
    flushAll();
    Ink::Change all;
    for (const Ink::Stroke &stroke : strokesOn(page))
        all.removed << stroke.id;
    return !all.empty() && take(page, all);
}

void ReadingInk::refresh() {
    for (auto cached = m_pages.begin(); cached != m_pages.end(); ++cached) {
        const Library *library = m_reader->library();
        std::vector<Ink::Stroke> now = library ? BookInk::strokes(*library, m_reader->bookId(), cached.key()) : std::vector<Ink::Stroke>{};
        const Ink::Change came = between(cached.value(), now);
        if (came.empty())
            continue;
        cached.value() = std::move(now);
        for (const auto &[page, surface] : std::as_const(m_surfaces))
            if (page == cached.key() && surface)
                surface->applyChange(came);
    }
    readList();
    emit changed();
}

std::vector<Ink::Stroke> ReadingInk::strokesOn(int page) {
    if (const auto cached = m_pages.constFind(page); cached != m_pages.cend())
        return cached.value();
    const Library *library = m_reader->library();
    std::vector<Ink::Stroke> strokes =
        library && m_reader->bookId() > 0 ? BookInk::strokes(*library, m_reader->bookId(), page) : std::vector<Ink::Stroke>{};
    m_pages.insert(page, strokes);
    return strokes;
}

Ink::Change ReadingInk::changeOf(const Step &step) {
    Ink::Change change{.added = step.added, .removed = {}};
    for (const Ink::Stroke &stroke : step.removed)
        change.removed << stroke.id;
    return change;
}

ReadingInk::Step ReadingInk::inverse(Step step) {
    return {.page = step.page, .added = std::move(step.removed), .removed = std::move(step.added)};
}

bool ReadingInk::keep(int page, const Ink::Change &change, const InkSurface *from) {
    Library *library = m_reader->library();
    if (!library || m_reader->bookId() <= 0 || change.empty())
        return false;
    if (const auto written = BookInk::write(*library, m_reader->bookId(), page, change, fileHash()); !written) {
        qWarning("Book ink: not kept: %s", qPrintable(written.error()));
        return false;
    }
    std::vector<Ink::Stroke> strokes = strokesOn(page);
    std::erase_if(strokes, [&change](const Ink::Stroke &stroke) { return change.removed.contains(stroke.id); });
    std::ranges::copy(change.added, std::back_inserter(strokes));
    BookInk::inOrder(strokes);
    m_pages.insert(page, std::move(strokes));
    for (const auto &[shownPage, surface] : std::as_const(m_surfaces))
        if (shownPage == page && surface && surface != from)
            surface->applyChange(change);
    readList();
    // Its words read again once the pen rests, where recognition is set up (#115).
    if (m_words)
        m_words->changed(m_reader->bookId(), page);
    return true;
}

void ReadingInk::flushAll() {
    // A copy: a batch sent is taken at once, and the list may change meanwhile.
    const auto surfaces = m_surfaces;
    for (const auto &[page, surface] : surfaces)
        if (surface)
            surface->flush();
}

void ReadingInk::readList() {
    m_list.clear();
    const Library *library = m_reader->library();
    if (library && m_reader->bookId() > 0 && m_reader->kind() == QLatin1StringView("pages")) {
        for (const BookInk::Page &page : BookInk::pages(*library, m_reader->bookId())) {
            const PageRegion region = BookInk::regionOf(page.page, page.strokes, 12);
            const double aspect = m_reader->pageAspect(page.page);
            m_list << QVariantMap{{"id", page.id},
                                  {"page", page.page},
                                  {"strokes", static_cast<int>(page.strokes.size())},
                                  {"x", region.area.x()},
                                  {"y", region.area.y()},
                                  {"width", region.area.width()},
                                  {"height", region.area.height()},
                                  {"fraction", region.place(aspect > 0 ? aspect : usualAspect).fraction}};
        }
    }
}

void ReadingInk::clear() {
    m_pages.clear();
    m_undo.clear();
    m_redo.clear();
    m_fileHash.clear();
    m_hashOf = 0;
    if (m_inking) {
        m_inking = false;
        emit modeChanged();
    }
}

QString ReadingInk::fileHash() {
    if (m_hashOf != m_reader->bookId()) {
        m_hashOf = m_reader->bookId();
        const Library *library = m_reader->library();
        const auto file = library ? AnnotationAnchors::readableFile(*library, m_hashOf) : std::nullopt;
        m_fileHash = file ? file->hash : QString();
    }
    return m_fileHash;
}
