#include "qrview.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

QrView::QrView(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setAntialiasing(false);
    setImplicitSize(160, 160);
}

void QrView::setText(const QString &text) {
    if (text == m_text)
        return;
    m_text = text;
    m_code = text.isEmpty() ? std::nullopt : QrCode::of(text.toUtf8());
    emit changed();
    update();
}

void QrView::paint(QPainter *painter) {
    if (!m_code)
        return;
    const int side = modules();
    const qreal room = std::min(width(), height());
    // Whole pixels a module where there is room for at least one.
    const qreal module = room >= side ? std::floor(room / side) : room / side;
    const qreal across = module * side;
    const QPointF corner((width() - across) / 2, (height() - across) / 2);
    painter->fillRect(QRectF(corner, QSizeF(across, across)), Qt::white);
    for (int y = 0; y < m_code->size(); ++y) {
        for (int x = 0; x < m_code->size(); ++x) {
            if (m_code->dark(x, y))
                painter->fillRect(QRectF(corner.x() + (x + margin) * module, corner.y() + (y + margin) * module, module, module),
                                  Qt::black);
        }
    }
}
