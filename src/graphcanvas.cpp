#include "graphcanvas.h"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QHash>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace {

constexpr double smallest = 0.04, largest = 6;

// A path from a few letters: M, L, H, V, C and Z as in SVG, all absolute,
// and O for a whole circle (its middle and its radius).
[[nodiscard]] QPainterPath drawn(const char *text) {
    QPainterPath path;
    const QStringList words = QString::fromLatin1(text).split(' ', Qt::SkipEmptyParts);
    qsizetype at = 0;
    const auto number = [&] { return at < words.size() ? words.at(at++).toDouble() : 0.0; };
    while (at < words.size()) {
        const QChar letter = words.at(at++).at(0);
        if (letter == 'M') {
            const double x = number(), y = number();
            path.moveTo(x, y);
        } else if (letter == 'L') {
            const double x = number(), y = number();
            path.lineTo(x, y);
        } else if (letter == 'H') {
            path.lineTo(number(), path.currentPosition().y());
        } else if (letter == 'V') {
            path.lineTo(path.currentPosition().x(), number());
        } else if (letter == 'C') {
            const double a = number(), b = number(), c = number(), d = number(), x = number(), y = number();
            path.cubicTo(a, b, c, d, x, y);
        } else if (letter == 'O') {
            const double x = number(), y = number(), radius = number();
            path.addEllipse(QPointF(x, y), radius, radius);
        } else if (letter == 'Z') {
            path.closeSubpath();
        }
    }
    return path;
}

[[nodiscard]] QColor mixed(const QColor &a, const QColor &b, double share) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * share, a.greenF() + (b.greenF() - a.greenF()) * share,
                            a.blueF() + (b.blueF() - a.blueF()) * share);
}

[[nodiscard]] QColor faint(QColor color, double alpha) {
    color.setAlphaF(alpha);
    return color;
}

[[nodiscard]] double luminance(const QColor &color) noexcept { return .2126 * color.redF() + .7152 * color.greenF() + .0722 * color.blueF(); }

[[nodiscard]] QString shortened(const QString &text, int most) { return text.size() > most ? text.left(most - 1).trimmed() + QChar(0x2026) : text; }

// What part of the view is taken, so that no name is written over a node
// or over another name: boxes kept by the cells of a coarse grid they
// touch, which makes asking about one box a matter of a few others.
class Taken {
public:
    Taken(double width, double height)
        : m_across(int(width / cell) + 1), m_down(int(height / cell) + 1), m_cells(m_across * m_down) {}

    [[nodiscard]] bool free(const QRectF &box) const {
        bool free = true;
        each(box, [&free, &box](const QList<QRectF> &boxes) {
            free = free && std::ranges::none_of(boxes, [&box](const QRectF &other) { return other.intersects(box); });
        });
        return free;
    }
    void take(const QRectF &box) {
        each(box, [&box](QList<QRectF> &boxes) { boxes << box; });
        ++m_count;
    }
    [[nodiscard]] int count() const noexcept { return m_count; }

private:
    static constexpr double cell = 72;
    template <typename Cells, typename Visit> static void walk(Cells &cells, int across, int down, const QRectF &box, Visit visit) {
        const int left = std::clamp(int(box.left() / cell), 0, across - 1), right = std::clamp(int(box.right() / cell), 0, across - 1);
        const int top = std::clamp(int(box.top() / cell), 0, down - 1), bottom = std::clamp(int(box.bottom() / cell), 0, down - 1);
        for (int y = top; y <= bottom; ++y)
            for (int x = left; x <= right; ++x)
                visit(cells[y * across + x]);
    }
    template <typename Visit> void each(const QRectF &box, Visit visit) const { walk(m_cells, m_across, m_down, box, visit); }
    template <typename Visit> void each(const QRectF &box, Visit visit) { walk(m_cells, m_across, m_down, box, visit); }

    int m_across, m_down;
    QList<QList<QRectF>> m_cells;
    int m_count = 0;
};

[[nodiscard]] QFont fontOf(int pixels) {
    QFont font = QGuiApplication::font();
    font.setPixelSize(pixels);
    return font;
}

} // namespace

namespace GraphKinds {

QString colorKey(const QString &kind) {
    // People in blue and magenta, the world in green, yellow and cyan, the
    // plot in red, what was asked in orange; the library's own in the
    // window's ink, so the story stands out from the shelf it is on.
    static const QHash<QString, QString> keys{
        {"character", "blue"}, {"group", "magenta"}, {"place", "green"},  {"item", "yellow"},     {"concept", "cyan"},
        {"event", "red"},      {"thread", "red"},    {"theme", "magenta"}, {"topic", "cyan"},     {"question", "orange"},
        {"book", "foreground"}, {"author", "muted"}, {"series", "muted"}, {"tag", "muted"}};
    return keys.value(kind, "muted");
}

QColor color(const QString &kind, const QVariantMap &colors, const QColor &foreground, const QColor &background) {
    const QString key = colorKey(kind);
    if (key == "foreground")
        return foreground;
    const QColor color = colors.value(key).value<QColor>();
    return color.isValid() ? color : mixed(foreground, background, 0.45);
}

const QPainterPath &glyph(const QString &kind) {
    static const QHash<QString, QPainterPath> glyphs{
        {"character", drawn("O 12 8 3.6 M 5 20 C 5 15.5 8 13.5 12 13.5 C 16 13.5 19 15.5 19 20")},
        {"group", drawn("O 8.5 9 3 O 16.5 10 2.6 M 3 20 C 3 16 5.5 14.5 8.5 14.5 C 11.5 14.5 14 16 14 20 M 16.5 15 C 19 15 21 16.5 21 20")},
        {"place", drawn("M 12 21 C 8 16 6 13 6 10 C 6 6.5 8.7 4 12 4 C 15.3 4 18 6.5 18 10 C 18 13 16 16 12 21 Z O 12 10 2")},
        {"item", drawn("M 7 4.5 H 17 L 21 10 L 12 20.5 L 3 10 Z M 3 10 H 21")},
        {"concept", drawn("M 12 3 L 14 10 L 21 12 L 14 14 L 12 21 L 10 14 L 3 12 L 10 10 Z")},
        {"event", drawn("M 6 21 V 4 M 6 5 H 18 L 15 9 L 18 13 H 6")},
        {"thread", drawn("M 3 9 C 6 5 9 5 12 9 C 15 13 18 13 21 9 M 3 16 C 6 12 9 12 12 16 C 15 20 18 20 21 16")},
        {"theme", drawn("O 9 12 5.5 O 15 12 5.5")},
        {"topic", drawn("O 12 12 8.5 M 3.5 12 H 20.5 M 12 3.5 C 8 7 8 17 12 20.5 M 12 3.5 C 16 7 16 17 12 20.5")},
        {"question", drawn("M 8 9 C 8 6 10 4.5 12 4.5 C 14.5 4.5 16 6 16 8.5 C 16 11.5 12 11.5 12 15 M 12 19.2 V 19.4")},
        {"book", drawn("M 5 5 H 19 V 19 H 5 Z M 9 5 V 19")},
        {"author", drawn("M 4 20 L 5.5 14.5 L 16 4 L 20 8 L 9.5 18.5 Z M 13.5 6.5 L 17.5 10.5")},
        {"series", drawn("M 4 16.5 H 20 V 20 H 4 Z M 6 11.5 H 19 V 15 H 6 Z M 5 6.5 H 18 V 10 H 5 Z")},
        {"tag", drawn("M 4 4 H 12 L 20 12 L 12 20 L 4 12 Z O 8.5 8.5 1.1")}};
    static const QPainterPath other = drawn("O 12 12 3");
    const auto found = glyphs.constFind(kind);
    return found == glyphs.constEnd() ? other : *found;
}

QColor inkOn(const QColor &fill, const QColor &dark, const QColor &light) {
    return std::abs(luminance(fill) - luminance(dark)) > std::abs(luminance(fill) - luminance(light)) ? dark : light;
}

void paint(QPainter *painter, const QPointF &middle, double radius, const QString &kind, const QColor &fill, const QColor &ink,
           bool withGlyph) {
    painter->setPen(Qt::NoPen);
    painter->setBrush(fill);
    painter->drawEllipse(middle, radius, radius);
    // Below this the drawing is a smudge; the color alone says the kind.
    if (!withGlyph || radius < 6.5)
        return;
    const double scale = radius * 1.24 / 24;
    painter->save();
    painter->translate(middle);
    painter->scale(scale, scale);
    painter->translate(-12, -12);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(ink, std::max(1.9, 1.25 / scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->drawPath(glyph(kind));
    painter->restore();
}

} // namespace GraphKinds

GraphCanvas::GraphCanvas(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    // Where a glide was headed may be out of date by the time it gets
    // there: the graph went on settling, the view changed size.
    connect(&m_glide, &QVariantAnimation::finished, this, &GraphCanvas::follow);
    m_glide.setDuration(220);
    m_glide.setEasingCurve(QEasingCurve::OutCubic);
    m_glide.setStartValue(0.0);
    m_glide.setEndValue(1.0);
    connect(&m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const double done = value.toDouble();
        m_centre = m_glideFrom + (m_glideTo - m_glideFrom) * done;
        // Zoom is felt in ratios, so it is eased as one.
        m_zoom = m_zoomFrom * std::pow(m_zoomTo / m_zoomFrom, done);
        emit viewChanged();
        update();
    });
}

void GraphCanvas::setBrowser(GraphBrowser *browser) {
    if (browser == m_browser)
        return;
    if (m_browser)
        m_browser->disconnect(this);
    m_browser = browser;
    if (m_browser) {
        connect(m_browser, &GraphBrowser::changed, this, [this] {
            measure();
            update();
        });
        connect(m_browser, &GraphBrowser::moved, this, [this] {
            follow();
            update();
        });
        connect(m_browser, &GraphBrowser::selectedChanged, this, [this] { update(); });
        connect(m_browser, &GraphBrowser::reframed, this, [this] { fit(m_zoom != 1 || !m_centre.isNull()); });
    }
    measure();
    fit(false);
    emit browserChanged();
}

void GraphCanvas::setColors(const QVariantMap &colors) {
    if (colors == m_colors)
        return;
    m_colors = colors;
    emit lookChanged();
    update();
}

void GraphCanvas::setForeground(const QColor &color) {
    if (color == m_foreground)
        return;
    m_foreground = color;
    emit lookChanged();
    update();
}

void GraphCanvas::setBackground(const QColor &color) {
    if (color == m_background)
        return;
    m_background = color;
    emit lookChanged();
    update();
}

void GraphCanvas::setAccent(const QColor &color) {
    if (color == m_accent)
        return;
    m_accent = color;
    emit lookChanged();
    update();
}

void GraphCanvas::setHovered(qint64 node) {
    if (node == m_hovered)
        return;
    m_hovered = node;
    emit hoveredChanged();
    update();
}

void GraphCanvas::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) {
    QQuickPaintedItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        follow();
}

QPointF GraphCanvas::toScreen(const QPointF &world) const {
    return (world - m_centre) * m_zoom + QPointF(width() / 2, height() / 2);
}

// A node is as large as it is connected, so the hubs of a graph are the
// first thing seen; on screen it never shrinks to nothing nor grows past
// a button.
double GraphCanvas::radiusOf(int row) const {
    const GraphBrowser::Shown &shown = m_browser->shown().at(row);
    const double world = 7 + 2.4 * std::sqrt(double(shown.links));
    // What is from another book is smaller: it is around the picture, not of it.
    return std::clamp(std::min(world, 20.0) * m_zoom * (shown.outer ? 0.72 : 1.0), 3.0, 30.0);
}

QColor GraphCanvas::colorOf(const QString &kind) const { return GraphKinds::color(kind, m_colors, m_foreground, m_background); }

void GraphCanvas::measure() {
    m_names.clear();
    m_books.clear();
    m_relations.clear();
    if (!m_browser)
        return;
    const QFontMetricsF names(fontOf(12)), relations(fontOf(11)), books(fontOf(10));
    for (const GraphBrowser::Shown &shown : m_browser->shown()) {
        const QString text = shortened(shown.node.name, 26);
        m_names.append({text, names.horizontalAdvance(text)});
        // Under the name of a thing from another book, the book.
        const QString book = shortened(shown.from, 30);
        m_books.append({book, books.horizontalAdvance(book)});
    }
    for (const GraphBrowser::Link &link : m_browser->links()) {
        const QString text = shortened(link.edge.relation(), 30);
        m_relations.append({text, relations.horizontalAdvance(text)});
    }
}

void GraphCanvas::moveTo(const QPointF &centre, double zoom, bool glide) {
    m_glide.stop();
    if (glide && width() > 0) {
        m_glideFrom = m_centre;
        m_glideTo = centre;
        m_zoomFrom = m_zoom;
        m_zoomTo = zoom;
        m_glide.start();
        return;
    }
    m_centre = centre;
    m_zoom = zoom;
    emit viewChanged();
    update();
}

void GraphCanvas::follow() {
    if (m_following && m_glide.state() != QAbstractAnimation::Running)
        fit(false);
}

void GraphCanvas::fit(bool glide) {
    m_following = true;
    if (!m_browser || m_browser->positions().isEmpty() || width() <= 0 || height() <= 0)
        return;
    QRectF box(m_browser->positions().first(), QSizeF(0, 0));
    for (const QPointF &at : m_browser->positions())
        box |= QRectF(at, QSizeF(0, 0)).adjusted(-1, -1, 1, 1);
    // Room around it for the outermost nodes and the names under them.
    const double across = std::max(1.0, width() - 120), down = std::max(1.0, height() - 84);
    const double zoom = std::clamp(std::min(across / std::max(box.width(), 1.0), down / std::max(box.height(), 1.0)), smallest, 1.5);
    moveTo(box.center() + QPointF(0, 8 / zoom), zoom, glide);
}

void GraphCanvas::panBy(double dx, double dy) {
    m_following = false;
    moveTo(m_centre - QPointF(dx, dy) / m_zoom, m_zoom, false);
}

void GraphCanvas::zoomBy(double factor, double x, double y) {
    m_following = false;
    const QPointF from = (x < 0 || y < 0 ? QPointF(width() / 2, height() / 2) : QPointF(x, y)) - QPointF(width() / 2, height() / 2);
    const double zoom = std::clamp(m_zoom * factor, smallest, largest);
    // The point under the pointer stays under it.
    moveTo(m_centre + from / m_zoom - from / zoom, zoom, x < 0 || y < 0);
}

void GraphCanvas::reveal(qint64 node) {
    const int row = m_browser ? m_browser->rowOf(node) : -1;
    if (row < 0 || row >= m_browser->positions().size())
        return;
    // While the whole graph is kept in view, so is every node of it.
    if (m_following) {
        follow();
        return;
    }
    const QPointF centre = m_glide.state() == QAbstractAnimation::Running ? m_glideTo : m_centre;
    const double zoom = m_glide.state() == QAbstractAnimation::Running ? m_zoomTo : m_zoom;
    const QPointF at = (m_browser->positions().at(row) - centre) * zoom + QPointF(width() / 2, height() / 2);
    const double edge = std::min({64.0, width() / 4, height() / 4});
    QPointF shift;
    if (at.x() < edge) shift.rx() = at.x() - edge;
    if (at.x() > width() - edge) shift.rx() = at.x() - (width() - edge);
    if (at.y() < edge) shift.ry() = at.y() - edge;
    if (at.y() > height() - edge) shift.ry() = at.y() - (height() - edge);
    if (shift.isNull())
        return;
    m_following = false;
    moveTo(centre + shift / zoom, zoom, true);
}

qint64 GraphCanvas::nodeAt(double x, double y, double reach) const {
    if (!m_browser || m_browser->positions().size() != m_browser->shown().size())
        return 0;
    qint64 found = 0;
    double nearest = 0;
    for (int row = 0; row < m_browser->shown().size(); ++row) {
        const QPointF at = toScreen(m_browser->positions().at(row));
        const double distance = std::hypot(at.x() - x, at.y() - y);
        if (distance <= radiusOf(row) + reach && (found == 0 || distance < nearest)) {
            found = m_browser->shown().at(row).node.id;
            nearest = distance;
        }
    }
    return found;
}

QPointF GraphCanvas::pointOf(qint64 node) const {
    const int row = m_browser ? m_browser->rowOf(node) : -1;
    return row < 0 || row >= m_browser->positions().size() ? QPointF(-1, -1) : toScreen(m_browser->positions().at(row));
}

void GraphCanvas::paint(QPainter *painter) {
    QElapsedTimer clock;
    clock.start();
    if (!m_browser)
        return;
    const QList<GraphBrowser::Shown> &shown = m_browser->shown();
    const QList<GraphBrowser::Link> &links = m_browser->links();
    const QList<QPointF> &places = m_browser->positions();
    const int count = int(shown.size());
    if (places.size() != count || m_names.size() != count || m_books.size() != count || m_relations.size() != links.size())
        return;
    painter->setRenderHint(QPainter::Antialiasing);
    const QRectF view(0, 0, width(), height());
    const int selected = m_browser->rowOf(m_browser->selected());
    const int hovered = m_browser->rowOf(m_hovered);
    const int centre = m_browser->depth() > 0 ? m_browser->rowOf(m_browser->centre()) : -1;

    QList<QPointF> at(count);
    QList<double> radius(count);
    QList<bool> inView(count), lit(count, selected < 0);
    for (int row = 0; row < count; ++row) {
        at[row] = toScreen(places.at(row));
        radius[row] = radiusOf(row);
        inView[row] = view.adjusted(-40, -40, 40, 40).contains(at.at(row));
    }
    if (selected >= 0) {
        lit[selected] = true;
        for (const int row : m_browser->beside(selected))
            lit[row] = true;
    }

    // The edges, under everything: those of the selected node stand out
    // and the others step back; with nothing selected all are quiet.
    // A connection between books is no fact of either: it is drawn dashed,
    // in the accent, wherever it shows.
    QList<QLineF> plain, bright, bridges, brightBridges;
    QList<int> brightLinks;
    const double left = view.left(), right = view.right(), top = view.top(), bottom = view.bottom();
    for (int index = 0; index < links.size(); ++index) {
        const QPointF a = at.at(links.at(index).from), b = at.at(links.at(index).to);
        if ((a.x() < left && b.x() < left) || (a.x() > right && b.x() > right) || (a.y() < top && b.y() < top)
            || (a.y() > bottom && b.y() > bottom))
            continue;
        const bool ofSelected = selected >= 0 && (links.at(index).from == selected || links.at(index).to == selected);
        const bool bridge = links.at(index).edge.across() || shown.at(links.at(index).from).outer || shown.at(links.at(index).to).outer;
        if (ofSelected) {
            (bridge ? brightBridges : bright) << QLineF(a, b);
            brightLinks << index;
        } else {
            (bridge ? bridges : plain) << QLineF(a, b);
        }
    }
    painter->setPen(QPen(faint(m_foreground, selected < 0 ? 0.24 : 0.09), 1));
    painter->drawLines(plain);
    QPen dashed(faint(m_accent, selected < 0 ? 0.6 : 0.22), 1.2, Qt::CustomDashLine);
    dashed.setDashPattern({4, 4});
    painter->setPen(dashed);
    painter->drawLines(bridges);
    const QColor strong = faint(m_foreground, 0.78);
    painter->setPen(QPen(strong, 1.6));
    painter->drawLines(bright);
    dashed.setColor(m_accent);
    dashed.setWidthF(1.6);
    painter->setPen(dashed);
    painter->drawLines(brightBridges);
    // Which way a relation runs, for those of the selected node.
    painter->setPen(Qt::NoPen);
    painter->setBrush(strong);
    for (const int index : std::as_const(brightLinks)) {
        const QPointF a = at.at(links.at(index).from), b = at.at(links.at(index).to);
        const double length = std::hypot(b.x() - a.x(), b.y() - a.y());
        if (length < radius.at(links.at(index).to) + 14)
            continue;
        const QPointF along = (b - a) / length, across(-along.y(), along.x());
        const QPointF tip = b - along * (radius.at(links.at(index).to) + 3);
        painter->drawPolygon(QPolygonF{tip, tip - along * 8 + across * 4, tip - along * 8 - across * 4});
    }

    // The nodes: the dimmed ones first, so what matters lies on top. With
    // hundreds in view their small drawings are only noise (and the slow
    // part of a frame), so then just those around the selection keep them.
    const QColor dark = luminance(m_background) < luminance(m_foreground) ? m_background : m_foreground;
    const QColor light = luminance(m_background) < luminance(m_foreground) ? m_foreground : m_background;
    const bool crowded = std::ranges::count(inView, true) > 220;
    for (const bool pass : {false, true})
        for (int row = 0; row < count; ++row) {
            if (!inView.at(row) || lit.at(row) != pass)
                continue;
            const QColor own = shown.at(row).outer ? mixed(colorOf(shown.at(row).node.kind), m_background, 0.45)
                                                   : colorOf(shown.at(row).node.kind);
            const QColor fill = pass ? own : mixed(own, m_background, 0.72);
            GraphKinds::paint(painter, at.at(row), radius.at(row), shown.at(row).node.kind, fill,
                              pass ? GraphKinds::inkOn(fill, dark, light) : mixed(GraphKinds::inkOn(own, dark, light), fill, 0.6),
                              !crowded || (selected >= 0 && pass));
        }
    painter->setBrush(Qt::NoBrush);
    if (centre >= 0 && centre != selected) {
        painter->setPen(QPen(faint(m_foreground, 0.5), 1.5));
        painter->drawEllipse(at.at(centre), radius.at(centre) + 4, radius.at(centre) + 4);
    }
    if (hovered >= 0 && hovered != selected) {
        painter->setPen(QPen(faint(m_foreground, 0.7), 1.5));
        painter->drawEllipse(at.at(hovered), radius.at(hovered) + 3, radius.at(hovered) + 3);
    }
    if (selected >= 0) {
        painter->setPen(QPen(m_accent, 2.5));
        painter->drawEllipse(at.at(selected), radius.at(selected) + 4, radius.at(selected) + 4);
    }

    // The names: the selected node's and its neighbours' first, then the
    // best connected, each only where no name stands already. That alone
    // makes a far view name its hubs and a near view name everything.
    auto order = std::views::iota(0, count) | std::views::filter([&inView](int row) { return inView.at(row); })
                 | std::ranges::to<QList<int>>();
    const auto weight = [&](int row) {
        return (row == selected ? 4000000 : 0) + (row == hovered ? 2000000 : 0) + (selected >= 0 && lit.at(row) ? 1000000 : 0)
               + shown.at(row).links;
    };
    std::ranges::stable_sort(order, std::ranges::greater{}, weight);
    Taken taken(width(), height());
    for (const int row : std::as_const(order))
        taken.take(QRectF(at.at(row).x() - radius.at(row), at.at(row).y() - radius.at(row), radius.at(row) * 2, radius.at(row) * 2));
    const int nodes = taken.count();
    // The names alone: one that must be shown may cover a node, not another name.
    Taken names(width(), height());
    if (selected >= 0)
        names.take(QRectF(at.at(selected).x() - radius.at(selected) - 4, at.at(selected).y() - radius.at(selected) - 4,
                          radius.at(selected) * 2 + 8, radius.at(selected) * 2 + 8));
    painter->setFont(fontOf(12));
    const QColor plate = faint(m_background, 0.8);
    for (const int row : std::as_const(order)) {
        if (taken.count() - nodes >= 260)
            break;
        // The selected node's neighbours are what a selection is for, so
        // they are named whatever is in the way, unless there are crowds of them.
        const bool must = row == selected || row == hovered
                          || (selected >= 0 && lit.at(row) && m_browser->beside(selected).size() <= 14);
        // A speck has no room for a name, unless it is the one asked about.
        if (!must && radius.at(row) < 4.5 && !(selected >= 0 && lit.at(row)))
            continue;
        const bool booked = !m_books.at(row).text.isEmpty();
        const double wide = std::max(m_names.at(row).width, m_books.at(row).width);
        const QRectF box(at.at(row).x() - wide / 2 - 5, at.at(row).y() + radius.at(row) + (row == selected || row == hovered ? 7 : 4),
                         wide + 10, booked ? 31 : 18);
        if (row != selected && !(must ? names.free(box) : taken.free(box)))
            continue;
        taken.take(box);
        names.take(box);
        painter->setPen(Qt::NoPen);
        painter->setBrush(plate);
        painter->drawRoundedRect(box, 3, 3);
        const double ink = (lit.at(row) ? 1.0 : 0.38) * (shown.at(row).outer && row != selected ? 0.8 : 1.0);
        painter->setPen(faint(m_foreground, ink));
        painter->drawText(QRectF(box.left(), box.top(), box.width(), 18), Qt::AlignCenter, m_names.at(row).text);
        if (booked) {
            painter->setFont(fontOf(10));
            painter->setPen(faint(m_foreground, ink * 0.6));
            painter->drawText(QRectF(box.left(), box.top() + 16, box.width(), 13), Qt::AlignCenter, m_books.at(row).text);
            painter->setFont(fontOf(12));
        }
    }

    // What the edges say: those of the selected node always, the others
    // once the view is near enough for them to be read.
    painter->setFont(fontOf(11));
    const bool near = m_zoom >= 1.35;
    for (int index = 0; index < links.size() && taken.count() - nodes < 420; ++index) {
        const bool ofSelected = selected >= 0 && (links.at(index).from == selected || links.at(index).to == selected);
        if (!ofSelected && (!near || selected >= 0))
            continue;
        const QPointF middle = (at.at(links.at(index).from) + at.at(links.at(index).to)) / 2;
        if (!view.contains(middle))
            continue;
        const QRectF box(middle.x() - m_relations.at(index).width / 2 - 4, middle.y() - 8, m_relations.at(index).width + 8, 16);
        if (!taken.free(box))
            continue;
        taken.take(box);
        painter->setPen(Qt::NoPen);
        painter->setBrush(faint(m_background, 0.9));
        painter->drawRoundedRect(box, 3, 3);
        painter->setPen(faint(m_foreground, ofSelected ? 0.85 : 0.6));
        painter->drawText(box, Qt::AlignCenter, m_relations.at(index).text);
    }
    m_paintTime = clock.nsecsElapsed() / 1e6;
}

GraphMark::GraphMark(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(true);
    setImplicitSize(18, 18);
}

void GraphMark::setKind(const QString &kind) {
    if (kind == m_kind)
        return;
    m_kind = kind;
    emit changed();
    update();
}

void GraphMark::setColors(const QVariantMap &colors) {
    if (colors == m_colors)
        return;
    m_colors = colors;
    emit changed();
    update();
}

void GraphMark::setForeground(const QColor &color) {
    if (color == m_foreground)
        return;
    m_foreground = color;
    emit changed();
    update();
}

void GraphMark::setBackground(const QColor &color) {
    if (color == m_background)
        return;
    m_background = color;
    emit changed();
    update();
}

void GraphMark::paint(QPainter *painter) {
    painter->setRenderHint(QPainter::Antialiasing);
    const QColor fill = GraphKinds::color(m_kind, m_colors, m_foreground, m_background);
    const bool darkTheme = luminance(m_background) < luminance(m_foreground);
    GraphKinds::paint(painter, QPointF(width() / 2, height() / 2), std::min(width(), height()) / 2 - 0.5, m_kind, fill,
                      GraphKinds::inkOn(fill, darkTheme ? m_background : m_foreground, darkTheme ? m_foreground : m_background), true);
}
