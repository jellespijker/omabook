#pragma once

#include <QColor>
#include <QHash>
#include <QPointer>
#include <QQuickItem>
#include <QVariantMap>
#include <array>
#include <memory>

#include "chronologymodel.h"

class QSGRectangleNode;
class QSGTextNode;
class QTextLayout;

// The chronology as a picture (#44): each band a ruler of its calendar's
// years, months or days (or none, for a band in order alone), each lane a
// row of marks under it, a moment a square and a span a bar, an event
// during another on a row under it; what someone shared hollow, what is in
// a contradiction ringed, and "you are here" where the reader is.
//
// Drawn with Qt Quick's scene graph, not a painter: the shapes of a frame
// are rectangle nodes kept from frame to frame, the words a few text nodes,
// so that thousands of events pan and zoom smoothly. Only what is in view
// is put in a frame. Panning and zooming change two numbers and draw again.
class ChronologyCanvas : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(ChronologyBrowser *browser READ browser WRITE setBrowser NOTIFY browserChanged)
    // The theme: its palette by name, and the colours everything is drawn in.
    Q_PROPERTY(QVariantMap colors READ colors WRITE setColors NOTIFY lookChanged)
    Q_PROPERTY(QColor foreground READ foreground WRITE setForeground NOTIFY lookChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY lookChanged)
    Q_PROPERTY(QColor accent READ accent WRITE setAccent NOTIFY lookChanged)
    // Time runs the other way, with the lanes' names on the right, in a
    // right-to-left language.
    Q_PROPERTY(bool mirrored READ mirrored WRITE setMirrored NOTIFY lookChanged)
    // The words for "you are here", in the window's language.
    Q_PROPERTY(QString hereText READ hereText WRITE setHereText NOTIFY lookChanged)
    // How much of a band is in view: 1 for all of it.
    Q_PROPERTY(double zoom READ zoom NOTIFY viewChanged)
    // The event under the pointer, or 0.
    Q_PROPERTY(qint64 hovered READ hovered WRITE setHovered NOTIFY hoveredChanged)

public:
    explicit ChronologyCanvas(QQuickItem *parent = nullptr);

    ChronologyBrowser *browser() const { return m_browser; }
    void setBrowser(ChronologyBrowser *browser);
    QVariantMap colors() const { return m_colors; }
    void setColors(const QVariantMap &colors);
    QColor foreground() const { return m_foreground; }
    void setForeground(const QColor &color);
    QColor background() const { return m_background; }
    void setBackground(const QColor &color);
    QColor accent() const { return m_accent; }
    void setAccent(const QColor &color);
    bool mirrored() const noexcept { return m_mirrored; }
    void setMirrored(bool mirrored);
    QString hereText() const { return m_hereText; }
    void setHereText(const QString &text);
    double zoom() const noexcept { return m_zoom; }
    qint64 hovered() const noexcept { return m_hovered; }
    void setHovered(qint64 event);

    // The event at a point of the item, or 0; `reach` widens the marks for a finger.
    Q_INVOKABLE qint64 eventAt(double x, double y, double reach = 4) const;
    // Where an event's mark is in the item (the selected mark, for the selected event).
    Q_INVOKABLE QPointF pointOf(qint64 event) const;
    Q_INVOKABLE void panBy(double dx, double dy);
    // Zooms about a point across the item; about its middle when none is given.
    Q_INVOKABLE void zoomBy(double factor, double x = -1);
    // Every band whole in view, from its start.
    Q_INVOKABLE void fit();
    // Moves the view just enough to have the selected mark well inside it.
    Q_INVOKABLE void reveal();
    // What the last frame took to put together, in milliseconds of its thread's work (ThreadClock), and how many marks it drew.
    Q_INVOKABLE double frameTime() const noexcept { return m_frameTime; }
    Q_INVOKABLE int drawn() const noexcept { return m_drawn; }

signals:
    void browserChanged();
    void lookChanged();
    void viewChanged();
    void hoveredChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *data) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;

private:
    struct Shapes;
    struct Words;
    void relayout();
    void addRulers(Shapes &shapes, Words &words);
    void addMarks(Shapes &shapes, Words &words);
    void addName(Words &words, const Chronology::Chart::Mark &mark, double to, double y, double &room);
    // "You are here", at a mark.
    void addHere(Shapes &shapes, Words &words, const Chronology::Chart::Mark &mark, double x);
    // Across the item: where a place along a band is, and the other way round.
    [[nodiscard]] double xOf(double along) const;
    [[nodiscard]] double alongAt(double x) const;
    [[nodiscard]] double rowY(int lane, int row) const;
    [[nodiscard]] QTextLayout &layoutOf(const QString &text, int pixels, bool bold = false);
    void clampView();

    QPointer<ChronologyBrowser> m_browser;
    QVariantMap m_colors;
    QColor m_foreground{Qt::white}, m_background{Qt::black}, m_accent{Qt::yellow};
    bool m_mirrored = false;
    QString m_hereText;
    // The place along every band in the middle of the view, how much of a
    // band the view holds, and how far down the lanes it is scrolled.
    double m_centre = 0.5, m_zoom = 1, m_scroll = 0;
    double m_mostZoom = 1;
    qint64 m_hovered = 0;
    // Where each band's ruler and each lane begin, down the picture.
    QList<double> m_bandTop, m_laneTop;
    QList<int> m_laneFirst; // each lane's first mark, and one past the last after them
    double m_height = 0;
    QHash<QString, std::shared_ptr<QTextLayout>> m_layouts;
    double m_frameTime = 0;
    int m_drawn = 0;
    // The nodes of the last frame, which the scene graph owns: the shapes,
    // and the words in the foreground, muted and accent colours.
    QSGNode *m_rectsNode = nullptr;
    QList<QSGRectangleNode *> m_rects;
    std::array<QSGTextNode *, 3> m_textNodes{};
};
