#include "inkpicture.h"

#include <QBuffer>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {

// Room around the writing, in pixels, so a letter at the edge is whole.
constexpr int margin = 24;
// Small writing is drawn larger, up to this, so a model sees the letters.
constexpr double mostEnlarged = 2.0;

// Every stroke's, each with room for its width; none when there are no points.
[[nodiscard]] std::optional<QRectF> boundsOf(std::span<const Ink::Stroke> strokes) {
    std::optional<QRectF> all;
    for (const Ink::Stroke &stroke : strokes)
        if (!stroke.points.empty())
            all = all ? all->united(Ink::bounds(stroke)) : Ink::bounds(stroke);
    return all;
}

void draw(QPainter &painter, const Ink::Stroke &stroke, double scale) {
    if (stroke.points.size() == 1) {
        const double width = Ink::widthAt(stroke, stroke.points.front()) * scale;
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::black);
        painter.drawEllipse(stroke.points.front().at * scale, width / 2, width / 2);
        return;
    }
    // Each piece as wide as the pen was pressed where it begins.
    for (std::size_t i = 1; i < stroke.points.size(); ++i) {
        const Ink::Point &from = stroke.points[i - 1];
        QPen pen(Qt::black, std::max(1.0, Ink::widthAt(stroke, from) * scale), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawLine(from.at * scale, stroke.points[i].at * scale);
    }
}

} // namespace

QImage Ink::picture(std::span<const Stroke> strokes, int longest) {
    const auto bounded = boundsOf(strokes);
    if (!bounded)
        return {};
    const QRectF area = *bounded;
    const double side = std::max({area.width(), area.height(), 1.0});
    const double scale = std::min(mostEnlarged, (longest - 2.0 * margin) / side);
    const QSize size(static_cast<int>(std::ceil(area.width() * scale)) + 2 * margin,
                     static_cast<int>(std::ceil(area.height() * scale)) + 2 * margin);
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(margin - area.left() * scale, margin - area.top() * scale);
    for (const Stroke &stroke : strokes)
        if (!stroke.points.empty())
            draw(painter, stroke, scale);
    return image;
}

QByteArray Ink::png(std::span<const Stroke> strokes, int longest) {
    const QImage image = picture(strokes, longest);
    if (image.isNull())
        return {};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return bytes;
}
