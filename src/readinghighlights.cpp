#include "readinghighlights.h"

#include <QTextDocument>
#include <algorithm>
#include <optional>
#include <vector>

#include "annotationanchors.h"
#include "backend.h"
#include "booktext.h"
#include "highlightcolors.h"
#include "highlights.h"
#include "inkgesture.h"
#include "inksurface.h"
#include "log.h"
#include "pagetext.h"
#include "reader.h"

namespace {

[[nodiscard]] QString kindName(Ink::GestureKind kind) {
    switch (kind) {
    case Ink::GestureKind::Underline:
        return QStringLiteral("underline");
    case Ink::GestureKind::Circle:
        return QStringLiteral("circle");
    case Ink::GestureKind::Bracket:
        return QStringLiteral("bracket");
    case Ink::GestureKind::Scribble:
        return QStringLiteral("scribble");
    case Ink::GestureKind::None:
        break;
    }
    return {};
}

[[nodiscard]] QVariantList variants(const QList<QRectF> &rects) {
    QVariantList list;
    for (const QRectF &rect : rects)
        list << rect;
    return list;
}

// The words of a passage as they are read: one line, without the hyphens only the eye needs.
[[nodiscard]] QString shownWords(const QString &words) { return QString(words).remove(QChar(0x00AD)).simplified(); }

} // namespace

ReadingHighlights::ReadingHighlights(Reader *reader) : QObject(reader), m_reader(reader) {
    connect(reader, &Reader::bookChanged, this, [this] {
        m_words.clear();
        refresh();
    });
    if (const Backend *backend = reader->backend())
        connect(backend, &Backend::revisionChanged, this, &ReadingHighlights::refresh);
}

void ReadingHighlights::setForeground(const QColor &color) {
    if (color == m_foreground)
        return;
    m_foreground = color;
    emit lookChanged();
}

void ReadingHighlights::setBackground(const QColor &color) {
    if (color == m_background)
        return;
    m_background = color;
    emit lookChanged();
}

QVariantList ReadingHighlights::colors() const {
    QVariantList colors;
    for (const HighlightColors::Color &color : HighlightColors::all()) {
        QVariantList dashes;
        for (const qreal dash : color.dashes)
            dashes << dash;
        colors << QVariantMap{{"key", color.key},
                              {"color", color.color},
                              {"tint", HighlightColors::tint(color.key, m_background, m_foreground)},
                              {"line", HighlightColors::line(color.key, m_background)},
                              // On a PDF's page, which is paper whatever the theme.
                              {"paperTint", HighlightColors::tint(color.key, Qt::white, Qt::black)},
                              {"paperLine", HighlightColors::line(color.key, Qt::white)},
                              {"dashes", dashes}};
    }
    return colors;
}

void ReadingHighlights::setCurrent(const QString &color) {
    if (color.isEmpty() || color == m_current || !HighlightColors::known(color))
        return;
    m_current = color;
    emit currentChanged();
}

QVariantList ReadingHighlights::list() const {
    const bool pages = m_reader->kind() == "pages";
    QVariantList list;
    for (const Annotation &highlight : m_highlights) {
        QVariantMap row{{"id", highlight.id},
                        {"color", HighlightColors::of(highlight.color).key},
                        {"quote", shownWords(highlight.passage().quote)},
                        {"section", highlight.place.section},
                        {"fraction", highlight.place.fraction},
                        {"title", pages ? QString() : titleOf(highlight.place.section)},
                        {"page", pages ? highlight.place.section + 1 : 0},
                        {"lost", highlight.lost}};
        if (const std::optional<PageRegion> region = highlight.region())
            row.insert("region", region->toJson().toVariantMap());
        list << row;
    }
    return list;
}

int ReadingHighlights::lostCount() const { return static_cast<int>(std::ranges::count_if(m_highlights, &Annotation::lost)); }

void ReadingHighlights::refresh() {
    const Library *library = m_reader->library();
    QList<Annotation> now = library && m_reader->isOpen() ? Highlights::of(*library, m_reader->bookId()) : QList<Annotation>();
    const QVariantList before = list();
    m_highlights = std::move(now);
    if (list() != before)
        emit changed();
}

QString ReadingHighlights::titleOf(int section) const {
    const std::shared_ptr<const Document> document = m_reader->document();
    QString title;
    int best = -1;
    for (const TocEntry &entry : document ? document->toc() : QList<TocEntry>())
        if (entry.section <= section && entry.section > best) {
            best = entry.section;
            title = entry.title;
        }
    return title;
}

ReadingHighlights::Shown ReadingHighlights::chapterShown(QQuickTextDocument *chapter) const {
    const QTextDocument *document = chapter ? chapter->textDocument() : nullptr;
    if (!document)
        return {};
    return {
        .section = m_reader->section(),
        .text = document->toPlainText(),
        .linesIn = [document](const QRectF &area) { return TextGeometry::linesIn(*document, area); },
        .rects = [document](int start, int end) { return TextGeometry::rects(*document, start, end); },
        .positionOn = [document](const TextGeometry::Line &line, double x, bool) { return TextGeometry::positionOn(*document, line, x); }};
}

ReadingHighlights::Shown ReadingHighlights::pageShown(int page, QSizeF shown) const {
    const std::shared_ptr<const Document> document = m_reader->document();
    if (!document || page < 0 || page >= document->count())
        return {.section = page};
    if (!m_words.contains(page))
        m_words.insert(page, document->words(page));
    const QList<PageWord> words = m_words.value(page);
    return {.section = page,
            .text = document->text(page),
            .linesIn = [words, shown](const QRectF &) { return PageText::lines(words, shown); },
            .rects = [words, shown](int start, int end) { return PageText::rects(words, start, end, shown); },
            .positionOn = [words, shown](const TextGeometry::Line &line, double x,
                                         bool end) { return PageText::positionOn(words, line, x, end, shown); }};
}

QList<ReadingHighlights::Span> ReadingHighlights::placed(const Shown &shown) const {
    QList<Span> found;
    if (shown.text.isEmpty())
        return found;
    for (const Annotation &highlight : m_highlights) {
        if (highlight.lost || highlight.place.section != shown.section)
            continue;
        if (const auto span = highlight.passage().spanIn(shown.text))
            found << Span{.id = highlight.id, .start = span->first, .end = span->second};
    }
    return found;
}

QVariantList ReadingHighlights::spans(const Shown &shown) const {
    QVariantList spans;
    for (const Span &span : placed(shown)) {
        const auto highlight = std::ranges::find(m_highlights, span.id, &Annotation::id);
        spans << QVariantMap{{"id", span.id},
                             {"color", HighlightColors::of(highlight->color).key},
                             {"rects", variants(shown.rects(static_cast<int>(span.start), static_cast<int>(span.end)))}};
    }
    return spans;
}

QVariantMap ReadingHighlights::under(const Shown &shown, int start, int end) const {
    for (const Span &span : placed(shown)) {
        if (span.start >= std::max(end, start + 1) || span.end <= start)
            continue;
        const auto highlight = std::ranges::find(m_highlights, span.id, &Annotation::id);
        return {{"id", span.id}, {"color", HighlightColors::of(highlight->color).key}};
    }
    return {};
}

qint64 ReadingHighlights::make(const Shown &shown, int start, int end, const QString &color) {
    Library *library = m_reader->library();
    const qint64 bookId = m_reader->bookId();
    const auto [from, to] = TextGeometry::wholeWords(shown.text, start, end);
    if (!library || bookId <= 0 || to <= from)
        return 0;
    const QString chosen = color.isEmpty() ? m_current : color;
    const Passage passage = Passage::of(shown.text, shown.section, from, to);
    // Placed in the book's own text, in the file the reader reads.
    const std::optional<FormatFile> file = AnnotationAnchors::readableFile(*library, bookId);
    const std::shared_ptr<const Document> document = m_reader->document();
    std::optional<BookText> text;
    if (document && file)
        text.emplace(*document);
    const auto made = Highlights::make(*library, bookId, passage, chosen, text ? &*text : nullptr, file ? file->hash : QString());
    if (!made) {
        qCWarning(lcLibrary).noquote() << "a highlight of book" << bookId << "couldn't be made:" << made.error();
        return 0;
    }
    setCurrent(chosen);
    refresh();
    return *made;
}

bool ReadingHighlights::place(qint64 id, const Shown &shown, int start, int end) {
    Library *library = m_reader->library();
    const auto [from, to] = TextGeometry::wholeWords(shown.text, start, end);
    const std::optional<FormatFile> file = library ? AnnotationAnchors::readableFile(*library, m_reader->bookId()) : std::nullopt;
    const std::shared_ptr<const Document> document = m_reader->document();
    if (!library || !file || !document || to <= from)
        return false;
    Passage passage = Passage::of(shown.text, shown.section, from, to);
    if (const std::optional<FoundPassage> found = passage.findAgain(BookText(*document)))
        passage = found->passage;
    const bool done = AnnotationAnchors::placeAgain(*library, id, passage, file->hash);
    refresh();
    return done;
}

QVariantMap ReadingHighlights::pen(const Shown &shown, const QVariantList &points) {
    std::vector<QPointF> at;
    QRectF area;
    for (const QVariant &point : points) {
        at.push_back(point.toPointF());
        area |= QRectF(at.back(), QSizeF(1, 1));
    }
    const QList<TextGeometry::Line> lines = shown.linesIn ? shown.linesIn(area.adjusted(0, -64, 0, 64)) : QList<TextGeometry::Line>();
    std::vector<Ink::TextLine> boxes;
    boxes.reserve(static_cast<size_t>(lines.size()));
    for (const TextGeometry::Line &line : lines)
        boxes.push_back({.box = line.box});
    const Ink::Gesture gesture = Ink::gestureOf(at, boxes);
    if (gesture.kind == Ink::GestureKind::None)
        return {{"kind", QString()}};
    const Ink::LineSpan &first = gesture.spans.first();
    const Ink::LineSpan &last = gesture.spans.last();
    const int start = shown.positionOn(lines.at(first.line), first.from, false);
    const int end = shown.positionOn(lines.at(last.line), last.to, true);
    if (gesture.kind != Ink::GestureKind::Scribble)
        return {{"kind", kindName(gesture.kind)}, {"id", make(shown, start, end, m_current)}};
    int removed = 0;
    for (const Span &span : placed(shown))
        if (span.end > start && span.start < end && m_reader->library()) {
            m_reader->library()->removeAnnotation(span.id);
            ++removed;
        }
    refresh();
    return {{"kind", kindName(gesture.kind)}, {"removed", removed}};
}

QVariantList ReadingHighlights::spansIn(QQuickTextDocument *chapter) const { return spans(chapterShown(chapter)); }
QVariantMap ReadingHighlights::at(QQuickTextDocument *chapter, int start, int end) const {
    return under(chapterShown(chapter), start, end);
}
qint64 ReadingHighlights::highlight(QQuickTextDocument *chapter, int start, int end, const QString &color) {
    return make(chapterShown(chapter), start, end, color);
}
bool ReadingHighlights::placeAgain(qint64 id, QQuickTextDocument *chapter, int start, int end) {
    return place(id, chapterShown(chapter), start, end);
}
QVariantMap ReadingHighlights::penned(QQuickTextDocument *chapter, const QVariantList &points) {
    return pen(chapterShown(chapter), points);
}

QVariantList ReadingHighlights::pageSpans(int page, QSizeF shown) const { return spans(pageShown(page, shown)); }
QVariantMap ReadingHighlights::pageAt(int page, int start, int end) const { return under(pageShown(page, {1, 1}), start, end); }
qint64 ReadingHighlights::highlightPage(int page, int start, int end, const QString &color) {
    return make(pageShown(page, {1, 1}), start, end, color);
}
bool ReadingHighlights::placeAgainOnPage(qint64 id, int page, int start, int end) { return place(id, pageShown(page, {1, 1}), start, end); }
QVariantMap ReadingHighlights::pagePenned(int page, QSizeF shown, const QVariantList &points) {
    return pen(pageShown(page, shown), points);
}

QVariantList ReadingHighlights::pageWords(int page, QSizeF shown) const {
    QVariantList words;
    const Shown on = pageShown(page, shown);
    for (const PageWord &word : m_words.value(page))
        words << QVariantMap{{"x", word.box.x() * shown.width()},
                             {"y", word.box.y() * shown.height()},
                             {"width", word.box.width() * shown.width()},
                             {"height", word.box.height() * shown.height()},
                             {"start", word.start},
                             {"end", word.end}};
    return on.text.isEmpty() ? QVariantList() : words;
}

QString ReadingHighlights::pageText(int page, int start, int end) const {
    const std::shared_ptr<const Document> document = m_reader->document();
    return document ? shownWords(document->text(page).mid(start, std::max(0, end - start))) : QString();
}

QVariantList ReadingHighlights::pageParts(int page, double width) const {
    QVariantList parts;
    for (const Annotation &highlight : m_highlights)
        if (const std::optional<PageRegion> region = highlight.region(); region && region->page == page)
            parts << QVariantMap{{"id", highlight.id},
                                 {"color", HighlightColors::of(highlight.color).key},
                                 {"x", region->area.x() * width},
                                 {"y", region->area.y() * width},
                                 {"width", region->area.width() * width},
                                 {"height", region->area.height() * width}};
    return parts;
}

qint64 ReadingHighlights::highlightPart(int page, const QRectF &area, double aspect, const QString &color) {
    Library *library = m_reader->library();
    const qint64 bookId = m_reader->bookId();
    if (!library || bookId <= 0)
        return 0;
    const QString chosen = color.isEmpty() ? m_current : color;
    const std::optional<FormatFile> file = AnnotationAnchors::readableFile(*library, bookId);
    const auto made = Highlights::makeOn(*library, bookId, {.page = page, .area = area}, aspect, chosen, file ? file->hash : QString());
    if (!made) {
        qCWarning(lcLibrary).noquote() << "a highlight on page" << page + 1 << "of book" << bookId << "couldn't be made in" << chosen;
        return 0;
    }
    setCurrent(chosen);
    refresh();
    return *made;
}

QVariantList ReadingHighlights::takeStrokes(QQuickItem *surface) {
    auto *ink = qobject_cast<InkSurface *>(surface);
    if (!ink)
        return {};
    QVariantList strokes;
    for (const Ink::Stroke &stroke : ink->sheet().strokes()) {
        QVariantList points;
        for (const Ink::Point &point : stroke.points)
            points << point.at * ink->inkScale();
        strokes << QVariant(points);
    }
    ink->replace({});
    return strokes;
}

bool ReadingHighlights::recolor(qint64 id, const QString &color) {
    Library *library = m_reader->library();
    const bool done = library && Highlights::recolor(*library, id, color);
    if (done)
        setCurrent(color);
    refresh();
    return done;
}

void ReadingHighlights::remove(qint64 id) {
    if (Library *library = m_reader->library())
        library->removeAnnotation(id);
    refresh();
}

QString ReadingHighlights::exported(const QString &color, const QVariantMap &names) const {
    QStringList lines{"# " + m_reader->title(), QString()};
    QString chapter;
    for (const QVariant &row : list()) {
        const QVariantMap highlight = row.toMap();
        if (!color.isEmpty() && highlight.value("color").toString() != color)
            continue;
        const QString title =
            highlight.value("page").toInt() > 0 ? tr("Page %1").arg(highlight.value("page").toInt()) : highlight.value("title").toString();
        if (title != chapter && !title.isEmpty())
            lines << "## " + title << QString();
        chapter = title;
        const QString quote = highlight.contains("region") ? tr("A part of the page") : highlight.value("quote").toString();
        lines << "> " + quote << QString() << "— " + names.value(highlight.value("color").toString(), highlight.value("color")).toString()
              << QString();
    }
    return lines.join('\n').trimmed() + '\n';
}
