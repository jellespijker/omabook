#include "chronologycanvas.h"

#include <QGuiApplication>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QTextLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

#include "graphcanvas.h"
#include "threadclock.h"

using Chronology::Chart::Mark;

namespace {

// The picture's measures, in pixels.
constexpr double side = 16;          // between the window's edge and either end of a band
constexpr double rulerHeight = 38;   // a band's name, its ruler's words and the ruler
constexpr double captionHeight = 20; // a lane's name, over its rows
constexpr double rowHeight = 22;
constexpr double laneFoot = 6;
constexpr double bandGap = 14;
constexpr double markSize = 5; // half a moment's mark
constexpr double barHeight = 10;
constexpr double tickRoom = 110; // the least room between two of a ruler's words
constexpr int longestName = 28;  // what an event's name is cut to beside its mark

// Words kept laid out: past this many, they are laid out afresh.
constexpr qsizetype mostLayouts = 20000;

[[nodiscard]] QString shortened(const QString &text, int most) {
    return text.size() > most ? text.left(most - 1).trimmed() + QChar(0x2026) : text;
}

[[nodiscard]] QColor faded(QColor color, double alpha) {
    color.setAlphaF(static_cast<float>(color.alphaF() * alpha));
    return color;
}

// How far the ring around a mark reaches: the selected one's, the one pointed at, or none.
[[nodiscard]] double grownBy(bool selected, bool hovered) {
    if (selected)
        return 4;
    return hovered ? 2 : 0;
}

[[nodiscard]] QColor mixed(const QColor &a, const QColor &b, double share) {
    return QColor::fromRgbF(static_cast<float>(a.redF() * (1 - share) + b.redF() * share),
                            static_cast<float>(a.greenF() * (1 - share) + b.greenF() * share),
                            static_cast<float>(a.blueF() * (1 - share) + b.blueF() * share));
}

} // namespace

// The frame's shapes: rectangles, which every renderer of the scene graph
// draws (the software one too, which draws no geometry of one's own).
struct ChronologyCanvas::Shapes {
    QList<std::pair<QRectF, QColor>> rects;
    bool mirrored = false;
    double width = 0;

    // A rectangle whose left edge is at `left` as time runs.
    void rect(double left, double top, double w, double h, const QColor &color) {
        if (w > 0 && h > 0 && color.alpha() > 0)
            rects.append({QRectF(mirrored ? width - left - w : left, top, w, h), color});
    }
    // How a mark looks: its colour, the ring of the selected one or the one
    // pointed at (`grow` wide, none when 0), the ring of one in a
    // contradiction, and the inside of one of someone else's graph.
    struct Look {
        QColor fill, ring, trouble, hollow;
        double grow = 0;
    };
    // A mark from `from` to `to`, at `y` down: a bar for a span, a square about `from` for a moment.
    void mark(const Mark &mark, double from, double to, double y, const Look &look) {
        const double half = mark.span ? barHeight / 2 : markSize;
        const double left = mark.span ? from : from - markSize;
        const double wide = mark.span ? to - from : 2 * markSize;
        const auto box = [&](double out, const QColor &color) {
            rect(left - out, y - half - out, wide + 2 * out, 2 * half + 2 * out, color);
        };
        if (look.grow > 0)
            box(look.grow, look.ring);
        if (mark.contradicted)
            box(2, look.trouble);
        box(0, look.fill);
        if (look.hollow.isValid())
            box(-2, look.hollow);
        if (mark.openFrom)
            runsOn(from, y, -1, look.fill);
        if (mark.openTo)
            runsOn(to, y, 1, look.fill);
    }
    // An end of a span that runs on: the bar fades out past it, `way` -1 before it and 1 after.
    void runsOn(double at, double y, int way, const QColor &color) {
        for (const auto [step, alpha] : std::array<std::pair<int, double>, 3>{{{0, 0.6}, {1, 0.35}, {2, 0.15}}})
            rect(way < 0 ? at - (step + 1) * 5.0 : at + step * 5.0, y - barHeight / 2, 5, barHeight, faded(color, alpha));
    }
};

// The frame's words, each laid out once and kept, by the colour they are written in.
struct ChronologyCanvas::Words {
    struct Placed {
        QPointF at;
        QTextLayout *layout = nullptr;
    };
    QList<Placed> ink, muted, lit;
    bool mirrored = false;
    double width = 0;

    // A box of words, its left edge at `left` as time runs.
    void add(QList<Placed> &to, double left, double top, QTextLayout &layout) const {
        const double wide = layout.lineAt(0).naturalTextWidth();
        to.append({{mirrored ? width - left - wide : left, top}, &layout});
    }
};

ChronologyCanvas::ChronologyCanvas(QQuickItem *parent) : QQuickItem(parent) { setFlag(ItemHasContents, true); }

void ChronologyCanvas::setBrowser(ChronologyBrowser *browser) {
    if (browser == m_browser)
        return;
    if (m_browser)
        m_browser->disconnect(this);
    m_browser = browser;
    if (m_browser) {
        connect(m_browser, &ChronologyBrowser::changed, this, [this] {
            relayout();
            update();
        });
        connect(m_browser, &ChronologyBrowser::selectedChanged, this, [this] {
            reveal();
            update();
        });
    }
    relayout();
    fit();
    emit browserChanged();
}

void ChronologyCanvas::setColors(const QVariantMap &colors) {
    m_colors = colors;
    emit lookChanged();
    update();
}
void ChronologyCanvas::setForeground(const QColor &color) {
    m_foreground = color;
    emit lookChanged();
    update();
}
void ChronologyCanvas::setBackground(const QColor &color) {
    m_background = color;
    emit lookChanged();
    update();
}
void ChronologyCanvas::setAccent(const QColor &color) {
    m_accent = color;
    emit lookChanged();
    update();
}
void ChronologyCanvas::setMirrored(bool mirrored) {
    m_mirrored = mirrored;
    emit lookChanged();
    update();
}
void ChronologyCanvas::setHereText(const QString &text) {
    m_hereText = text;
    emit lookChanged();
    update();
}
void ChronologyCanvas::setHovered(qint64 event) {
    if (event == m_hovered)
        return;
    m_hovered = event;
    emit hoveredChanged();
    update();
}

void ChronologyCanvas::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    clampView();
    update();
}

void ChronologyCanvas::relayout() {
    m_bandTop.clear();
    m_laneTop.clear();
    m_laneFirst.clear();
    m_mostZoom = 4;
    double y = 0;
    if (m_browser) {
        const auto &chart = m_browser->chart();
        m_laneTop.resize(chart.lanes.size());
        for (const Chronology::Chart::Band &band : chart.bands) {
            m_bandTop.append(y);
            y += rulerHeight;
            for (const int lane : band.lanes) {
                m_laneTop[lane] = y;
                y += captionHeight + chart.lanes.at(lane).rows * rowHeight + laneFoot;
            }
            y += bandGap;
            // As near as half an hour across the view, or three events of an order.
            m_mostZoom = std::max(m_mostZoom, band.dated() ? (band.last - band.first) / 30 : (band.last - band.first) / 3);
        }
        int at = 0;
        for (const auto [index, lane] : std::views::enumerate(chart.lanes)) {
            while (at < chart.marks.size() && chart.marks.at(at).lane < index)
                ++at;
            m_laneFirst.append(at);
        }
        m_laneFirst.append(static_cast<int>(chart.marks.size()));
    }
    m_height = y;
    clampView();
}

double ChronologyCanvas::xOf(double along) const {
    const double room = std::max(1.0, width() - 2 * side);
    return side + room * (0.5 + (along - m_centre) * m_zoom);
}

double ChronologyCanvas::alongAt(double x) const {
    const double room = std::max(1.0, width() - 2 * side);
    return m_centre + ((x - side) / room - 0.5) / m_zoom;
}

double ChronologyCanvas::rowY(int lane, int row) const {
    return m_laneTop.value(lane) + captionHeight + row * rowHeight + rowHeight / 2 - m_scroll;
}

QTextLayout &ChronologyCanvas::layoutOf(const QString &text, int pixels, bool bold) {
    const QString key = QString::number(pixels) + (bold ? u'b' : u'n') + text;
    if (m_layouts.size() > mostLayouts && !m_layouts.contains(key))
        m_layouts.clear();
    std::shared_ptr<QTextLayout> &kept = m_layouts[key];
    if (!kept) {
        QFont font = QGuiApplication::font();
        font.setPixelSize(pixels);
        font.setBold(bold);
        kept = std::make_shared<QTextLayout>(text, font);
        // On one line from its left edge, whichever way its words run.
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        option.setAlignment(Qt::AlignLeft | Qt::AlignAbsolute);
        kept->setTextOption(option);
        kept->beginLayout();
        kept->createLine().setLineWidth(100000);
        kept->endLayout();
    }
    return *kept;
}

void ChronologyCanvas::clampView() {
    m_zoom = std::clamp(m_zoom, 1.0, std::max(1.0, m_mostZoom));
    const double half = 0.5 / m_zoom;
    m_centre = std::clamp(m_centre, half, 1 - half);
    m_scroll = std::clamp(m_scroll, 0.0, std::max(0.0, m_height - height()));
}

void ChronologyCanvas::panBy(double dx, double dy) {
    const double room = std::max(1.0, width() - 2 * side);
    m_centre -= (m_mirrored ? -dx : dx) / (room * m_zoom);
    m_scroll -= dy;
    clampView();
    emit viewChanged();
    update();
}

void ChronologyCanvas::zoomBy(double factor, double x) {
    double logical = m_mirrored ? width() - x : x;
    if (x < 0)
        logical = width() / 2;
    const double along = alongAt(logical);
    const double room = std::max(1.0, width() - 2 * side);
    m_zoom = std::clamp(m_zoom * factor, 1.0, std::max(1.0, m_mostZoom));
    m_centre = along - ((logical - side) / room - 0.5) / m_zoom;
    clampView();
    emit viewChanged();
    update();
}

void ChronologyCanvas::fit() {
    m_zoom = 1;
    m_centre = 0.5;
    m_scroll = 0;
    clampView();
    emit viewChanged();
    update();
}

void ChronologyCanvas::reveal() {
    if (!m_browser || m_browser->selectedMark() < 0 || m_browser->selectedMark() >= m_browser->chart().marks.size())
        return;
    const Mark &mark = m_browser->chart().marks.at(m_browser->selectedMark());
    const double x = xOf((mark.from + mark.to) / 2);
    if (x < side * 3 || x > width() - side * 3)
        m_centre = (mark.from + mark.to) / 2;
    const double y = rowY(mark.lane, mark.row);
    if (y < rowHeight * 2)
        m_scroll -= rowHeight * 2 - y;
    else if (y > height() - rowHeight * 2)
        m_scroll += y - (height() - rowHeight * 2);
    clampView();
    emit viewChanged();
    update();
}

qint64 ChronologyCanvas::eventAt(double x, double y, double reach) const {
    if (!m_browser)
        return 0;
    const auto &chart = m_browser->chart();
    const double logical = m_mirrored ? width() - x : x;
    qint64 best = 0;
    double nearest = reach + markSize;
    for (const auto [lane, top] : std::views::enumerate(m_laneTop)) {
        const double first = rowY(static_cast<int>(lane), 0) - rowHeight / 2;
        if (y < first - reach || y > first + chart.lanes.at(lane).rows * rowHeight + reach)
            continue;
        for (int at = m_laneFirst.value(lane); at < m_laneFirst.value(lane + 1); ++at) {
            const Mark &mark = chart.marks.at(at);
            if (!m_browser->passes().value(at) || std::abs(rowY(mark.lane, mark.row) - y) > rowHeight / 2 + reach)
                continue;
            // How far the point is beside the mark: none when it is on it.
            const double off = std::max({xOf(mark.from) - logical, logical - xOf(mark.to), 0.0});
            if (off <= nearest) {
                nearest = off;
                best = mark.event;
            }
        }
    }
    return best;
}

QPointF ChronologyCanvas::pointOf(qint64 event) const {
    if (!m_browser)
        return {};
    const auto &chart = m_browser->chart();
    const QList<int> marks = chart.marksOf.value(event);
    if (marks.isEmpty())
        return {};
    const int at = marks.contains(m_browser->selectedMark()) ? m_browser->selectedMark() : marks.first();
    const Mark &mark = chart.marks.at(at);
    const double x = xOf((mark.from + mark.to) / 2);
    return {m_mirrored ? width() - x : x, rowY(mark.lane, mark.row)};
}

void ChronologyCanvas::addRulers(Shapes &shapes, Words &words) {
    const auto &chart = m_browser->chart();
    const QColor line = faded(m_foreground, 0.22);
    const QColor faint = faded(m_foreground, 0.07);
    const QColor band = faded(m_foreground, 0.04);
    for (const auto [index, of] : std::views::enumerate(chart.bands)) {
        const double top = m_bandTop.at(index) - m_scroll;
        const double bottom = (index + 1 < m_bandTop.size() ? m_bandTop.at(index + 1) : m_height) - bandGap - m_scroll;
        if (bottom < 0 || top > height())
            continue;
        shapes.rect(0, top, width(), rulerHeight - 2, band);
        shapes.rect(side, top + rulerHeight - 3, width() - 2 * side, 1, line);
        words.add(words.ink, side, top + 2, layoutOf(m_browser->bandName(static_cast<int>(index)), 12, true));
        if (!of.scale)
            continue;
        double clear = 0; // where the ruler has room for words again
        const auto ticks = of.scale->ticks(static_cast<qint64>(of.value(alongAt(0))), static_cast<qint64>(of.value(alongAt(width()))),
                                           std::max(1, static_cast<int>(width() / tickRoom)));
        for (const auto &tick : ticks) {
            const double x = xOf(of.at(static_cast<double>(tick.minute)));
            shapes.rect(x, top + rulerHeight - 8, 1, 6, line);
            shapes.rect(x, top + rulerHeight, 1, bottom - top - rulerHeight, faint);
            // Its words, where they are whole in view and clear of the last.
            QTextLayout &said = layoutOf(ChronologyBrowser::tickText(of.calendar, tick.point), 11);
            const double wide = said.lineAt(0).naturalTextWidth();
            if (x + 3 >= std::max(0.0, clear) && x + 3 + wide <= width()) {
                words.add(words.muted, x + 3, top + 19, said);
                clear = x + 3 + wide + 8;
            }
        }
    }
    for (const auto [lane, top] : std::views::enumerate(m_laneTop)) {
        const double y = top - m_scroll;
        if (y + captionHeight < 0 || y > height())
            continue;
        shapes.rect(side, y, width() - 2 * side, 1, faint);
        words.add(words.muted, side, y + 3, layoutOf(shortened(m_browser->laneName(static_cast<int>(lane)), 60), 11, true));
    }
}

void ChronologyCanvas::addHere(Shapes &shapes, Words &words, const Mark &mark, double x) {
    const double top = m_laneTop.value(mark.lane) - m_scroll;
    shapes.rect(x - 1, top + 2, 2, captionHeight + m_browser->chart().lanes.at(mark.lane).rows * rowHeight, m_accent);
    QTextLayout &said = layoutOf(m_hereText, 11, true);
    const double wide = said.lineAt(0).naturalTextWidth();
    words.add(words.lit, x + 6 + wide <= width() ? x + 6 : x - 6 - wide, top + 3, said);
}

void ChronologyCanvas::addMarks(Shapes &shapes, Words &words) {
    const auto &chart = m_browser->chart();
    const QColor own = GraphKinds::color(QStringLiteral("event"), m_colors, m_foreground, m_background);
    const auto warn = m_colors.value(QStringLiteral("yellow"), m_accent).value<QColor>();
    const qint64 selected = m_browser->selected();
    const qint64 here = m_browser->here();
    QHash<std::pair<int, int>, double> free; // where each row of each lane has room for a name again
    m_drawn = 0;
    for (const auto [at, mark] : std::views::enumerate(chart.marks)) {
        const double from = xOf(mark.from);
        const double to = mark.span ? std::max(xOf(mark.to), from + 4) : from;
        const double y = rowY(mark.lane, mark.row);
        const bool inView = to >= -markSize * 3 && from <= width() + markSize * 3 && y >= -rowHeight && y <= height() + rowHeight;
        if (!m_browser->passes().value(at) || !inView)
            continue;
        ++m_drawn;
        // What someone shared is hollow, a time "around" fainter.
        const bool layered = mark.event < 0;
        const bool chosen = mark.event == selected;
        const Shapes::Look look{.fill = faded(layered ? mixed(own, m_background, 0.45) : own, mark.approximate ? 0.6 : 1),
                                .ring = chosen ? m_accent : faded(m_foreground, 0.5),
                                .trouble = warn,
                                .hollow = layered ? m_background : QColor(),
                                .grow = grownBy(chosen, mark.event == m_hovered)};
        if (mark.event == here)
            addHere(shapes, words, mark, from);
        shapes.mark(mark, from, to, y, look);
        addName(words, mark, to, y, free[{mark.lane, mark.row}]);
    }
}

void ChronologyCanvas::addName(Words &words, const Mark &mark, double to, double y, double &room) {
    // Beside its mark, where that runs into no other name on its row (`room`
    // is where the row has room again).
    QTextLayout &name = layoutOf(shortened(m_browser->nameOf(mark.event), longestName), 12);
    const double wide = name.lineAt(0).naturalTextWidth();
    const double from = mark.span ? std::max(xOf(mark.from), 0.0) : to;
    // After it, else before it, else on a bar long enough to hold it.
    for (const double left : {to + markSize + (mark.openTo ? 15 : 0) + 4, from - markSize - (mark.openFrom ? 15 : 0) - 4 - wide,
                              mark.span && to - from > wide + 12 ? from + 6 : -1.0})
        if (left >= std::max(room, 0.0) && left + wide <= width()) {
            words.add(words.ink, left, y - 8, name);
            room = left + wide + 12;
            return;
        }
}

QSGNode *ChronologyCanvas::updatePaintNode(QSGNode *old, [[maybe_unused]] UpdatePaintNodeData *data) {
    const ThreadClock clock;
    QSGNode *root = old;
    // The scene graph owns the nodes; they are made again whenever it let them go.
    if (!root) {
        root = new QSGNode;
        m_rectsNode = new QSGNode;
        m_rects.clear();
        root->appendChildNode(m_rectsNode);
        for (QSGTextNode *&text : m_textNodes) {
            text = window()->createTextNode();
            root->appendChildNode(text);
        }
    }
    Shapes shapes{.rects = {}, .mirrored = m_mirrored, .width = width()};
    Words words{.ink = {}, .muted = {}, .lit = {}, .mirrored = m_mirrored, .width = width()};
    // With nothing the filters let through, the view says so in words alone.
    if (m_browser && m_browser->shownCount() > 0) {
        addRulers(shapes, words);
        addMarks(shapes, words);
    }
    // The rectangles of earlier frames are used again; those left over are empty.
    while (m_rects.size() < shapes.rects.size()) {
        m_rects.append(window()->createRectangleNode());
        m_rectsNode->appendChildNode(m_rects.last());
    }
    for (const auto [at, node] : std::views::enumerate(m_rects)) {
        node->setRect(at < shapes.rects.size() ? shapes.rects.at(at).first : QRectF());
        if (at < shapes.rects.size())
            node->setColor(shapes.rects.at(at).second);
    }
    const auto muted = m_colors.value(QStringLiteral("muted"), m_foreground).value<QColor>();
    const std::array<std::pair<const QList<Words::Placed> *, QColor>, 3> texts{
        {{&words.ink, m_foreground}, {&words.muted, muted}, {&words.lit, m_accent}}};
    for (const auto [text, written] : std::views::zip(m_textNodes, texts)) {
        text->clear();
        text->setColor(written.second);
        for (const Words::Placed &word : *written.first)
            text->addTextLayout(word.at, word.layout);
    }
    m_frameTime = clock.milliseconds();
    return root;
}
