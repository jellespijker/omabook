#pragma once

#include <QColor>
#include <QPainterPath>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QVariantAnimation>
#include <QVariantMap>

#include "graphmodel.h"

// How a kind of node looks: which of the theme's colors it takes, and the
// small drawing that tells it from the others of that color.
namespace GraphKinds {
// The key in the theme's palette: "blue", "green", ... or "foreground".
QString colorKey(const QString &kind);
// The color itself, in a theme: its palette by name and its two inks. The
// one function every face of a thing is colored by (its disc in the graph,
// its mark on the page, its name in an answer), so it is one color everywhere.
QColor color(const QString &kind, const QVariantMap &colors, const QColor &foreground, const QColor &background);
// The drawing, on a 24 by 24 grid like the app's other icons.
const QPainterPath &glyph(const QString &kind);
// A node: a disc of its kind's color with the drawing in it when it is
// large enough to be told (and `withGlyph` doesn't say to leave it out).
void paint(QPainter *painter, const QPointF &middle, double radius, const QString &kind, const QColor &fill, const QColor &ink,
           bool withGlyph);
// Whichever of two inks reads better on a fill.
QColor inkOn(const QColor &fill, const QColor &dark, const QColor &light);
} // namespace GraphKinds

// The knowledge graph as a picture: nodes as discs, edges as lines, drawn
// by one painter into one item.
//
// It is painted rather than built out of items or scene-graph nodes because
// what makes a graph readable is decided per frame: which of two thousand
// names fit without running into each other at this zoom, which edges say
// what they are, what is dimmed around the selection. That is a few loops
// over plain arrays here, with everything outside the view skipped, and no
// objects to create or keep in step. Panning and zooming only change two
// numbers and paint again.
class GraphCanvas : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(GraphBrowser *browser READ browser WRITE setBrowser NOTIFY browserChanged)
    // The theme: its palette by name, and the three colors everything is drawn in.
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY lookChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY lookChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY lookChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY lookChanged)
    // Screen pixels per unit of the layout.
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
    // The node under the pointer, or 0.
    Q_PROPERTY(qint64 hovered READ hovered WRITE setHovered NOTIFY hoveredChanged)

public:
    explicit GraphCanvas(QQuickItem *parent = nullptr);

    GraphBrowser *browser() const { return m_browser; }
    void setBrowser(GraphBrowser *browser);
    QVariantMap colors() const noexcept { return m_colors; }
    void setColors(const QVariantMap &colors);
    QColor foreground() const noexcept { return m_foreground; }
    void setForeground(const QColor &color);
    QColor background() const noexcept { return m_background; }
    void setBackground(const QColor &color);
    QColor accent() const noexcept { return m_accent; }
    void setAccent(const QColor &color);
    double zoom() const noexcept { return m_zoom; }
    qint64 hovered() const noexcept { return m_hovered; }
    void setHovered(qint64 node);
    // How long the last frame took to paint, in milliseconds.
    Q_INVOKABLE double paintTime() const { return m_paintTime; }

    void paint(QPainter *painter) override;

    // The node at a point of the item, or 0; `reach` widens the nodes for a finger.
    Q_INVOKABLE qint64 nodeAt(double x, double y, double reach = 4) const;
    // Where a node is in the item.
    Q_INVOKABLE QPointF pointOf(qint64 node) const;
    Q_INVOKABLE void panBy(double dx, double dy);
    // Zooms about a point of the item; about its middle when none is given.
    Q_INVOKABLE void zoomBy(double factor, double x = -1, double y = -1);
    // Brings the whole graph into view, and keeps it there while it settles.
    Q_INVOKABLE void fit(bool glide = true);
    // Moves the view just enough to have a node well inside it.
    Q_INVOKABLE void reveal(qint64 node);

signals:
    void browserChanged();
    void lookChanged();
    void viewChanged();
    void hoveredChanged();

protected:
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Name {
        QString text;
        double width = 0;
    };
    QPointF toScreen(const QPointF &world) const;
    double radiusOf(int row) const;
    QColor colorOf(const QString &kind) const;
    void measure();
    void follow();
    void moveTo(const QPointF &centre, double zoom, bool glide);

    QPointer<GraphBrowser> m_browser;
    QVariantMap m_colors;
    QColor m_foreground{Qt::white}, m_background{Qt::black}, m_accent{Qt::yellow};
    // The point of the layout in the middle of the item, and the scale.
    QPointF m_centre;
    double m_zoom = 1;
    // Whether the view keeps the whole graph in sight; until the user moves it.
    bool m_following = true;
    QVariantAnimation m_glide;
    QPointF m_glideFrom, m_glideTo;
    double m_zoomFrom = 1, m_zoomTo = 1;
    qint64 m_hovered = 0;
    QList<Name> m_names, m_books, m_relations;
    double m_paintTime = 0;
};

// One kind's mark on its own, for the legend, the list and the details.
class GraphMark : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString kind READ kind WRITE setKind NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY changed)

public:
    explicit GraphMark(QQuickItem *parent = nullptr);

    QString kind() const noexcept { return m_kind; }
    void setKind(const QString &kind);
    QVariantMap colors() const noexcept { return m_colors; }
    void setColors(const QVariantMap &colors);
    QColor foreground() const noexcept { return m_foreground; }
    void setForeground(const QColor &color);
    QColor background() const noexcept { return m_background; }
    void setBackground(const QColor &color);

    void paint(QPainter *painter) override;

signals:
    void changed();

private:
    QString m_kind;
    QVariantMap m_colors;
    QColor m_foreground{Qt::white}, m_background{Qt::black};
};
