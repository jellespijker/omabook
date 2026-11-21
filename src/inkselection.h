#pragma once

#include <QColor>
#include <QObject>
#include <QRectF>

#include <functional>

#include "inklasso.h"

class QKeyEvent;
class QQuickItem;

// What an ink surface's lasso has selected (#129), for QML as its
// `selection`: how many strokes and where, in item pixels, and what can be
// done with them, each one step of undo. The keys work while the surface
// has the keyboard, which it takes when the lasso selects: the arrows move
// the selection a pixel (with Shift ten), Delete and Backspace take it away,
// Ctrl+C, Ctrl+X and Ctrl+V copy, cut and paste it (between surfaces too),
// Ctrl+A selects every stroke and Escape none.
class InkSelection : public QObject {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(QRectF box READ box NOTIFY changed)
    // What the lasso and the box are drawn in: the theme's, given by whoever shows the surface.
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)

public:
    // `edit` takes each change made to the sheet as the surface's own edit.
    InkSelection(Ink::Lasso &lasso, QQuickItem &surface, std::function<void(Ink::Change)> edit);

    [[nodiscard]] int count() const { return static_cast<int>(m_lasso->selection().size()); }
    [[nodiscard]] QRectF box() const;
    [[nodiscard]] QColor color() const { return m_color; }
    void setColor(const QColor &color);
    // Item pixels to an ink unit, and whether the selection may be changed: the surface's own.
    void setScale(double scale) { m_scale = scale; }
    void setReadOnly(bool readOnly);

    Q_INVOKABLE void selectAll();
    // Nothing selected.
    Q_INVOKABLE void clear();
    // By so many item pixels.
    Q_INVOKABLE void moveBy(double dx, double dy);
    Q_INVOKABLE void recolor(const QColor &color);
    Q_INVOKABLE void remove();
    // To the clipboard, as ink (with its ids, so that pasting here again sets the copy beside it).
    Q_INVOKABLE void copy();
    Q_INVOKABLE void cut();
    // What the clipboard holds of ink, on top, selected. False when it holds none.
    Q_INVOKABLE bool paste();

    // The lasso's path and box as triangles, in item pixels.
    void marks(Ink::Mesh &mesh) const;
    [[nodiscard]] Ink::Lasso &lasso() { return *m_lasso; }

signals:
    void changed();
    void colorChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // What a key does to the selection, none when it is not the selection's.
    std::function<void()> actionOf(const QKeyEvent &event);
    void edit(Ink::Change change);
    void told();

    Ink::Lasso *m_lasso;   // never null: the surface's
    QQuickItem *m_surface; // never null: the surface, which owns this
    std::function<void(Ink::Change)> m_edit;
    QColor m_color{0x3f, 0x73, 0xc9};
    double m_scale = 1.0;
    bool m_readOnly = false;
    int m_shownCount = 0;
    QRectF m_shownBox;
};
