#include "pdfsketch.h"

#include <QBuffer>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPainterPath>
#include <QPdfWriter>
#include <QPen>
#include <QTextOption>
#include <algorithm>
#include <map>
#include <optional>
#include <utility>
#include <vector>

using namespace Qt::StringLiterals;

namespace {

using PdfSketch::Problem;

[[nodiscard]] Problem problem(Problem::Kind kind, qsizetype page = 0, qsizetype index = 0) {
    return {.kind = kind, .page = static_cast<int>(page), .index = static_cast<int>(index)};
}

// A colour as a script writes it, with `alpha` over its own.
[[nodiscard]] QColor colorOf(const QJsonObject &item, const QString &key, const QColor &otherwise) {
    QColor color = item.contains(key) ? QColor::fromString(item.value(key).toString()) : otherwise;
    if (!color.isValid())
        color = otherwise;
    if (item.contains("alpha"_L1))
        color.setAlphaF(static_cast<float>(color.alphaF() * std::clamp(item.value("alpha"_L1).toDouble(1), 0.0, 1.0)));
    return color;
}

// A page's layout, its size in points as it was given, edge to edge.
[[nodiscard]] std::optional<QPageLayout> layoutOf(const QJsonObject &page) {
    const double width = page.value("width"_L1).toDouble();
    const double height = page.value("height"_L1).toDouble();
    if (width < 1 || height < 1 || width > PdfSketch::maxSide || height > PdfSketch::maxSide)
        return std::nullopt;
    // A page size is upright; one wider than tall is that turned.
    const QPageSize size(QSizeF(std::min(width, height), std::max(width, height)), QPageSize::Point, {}, QPageSize::ExactMatch);
    return QPageLayout(size, width > height ? QPageLayout::Landscape : QPageLayout::Portrait, QMarginsF(), QPageLayout::Point);
}

// Each page's layout, or what can't be drawn, before anything is.
[[nodiscard]] std::expected<std::vector<QPageLayout>, Problem> layouts(const QJsonArray &pages) {
    if (pages.isEmpty())
        return std::unexpected(problem(Problem::Kind::NoPages));
    if (pages.size() > PdfSketch::maxPages)
        return std::unexpected(problem(Problem::Kind::TooManyPages));
    std::vector<QPageLayout> laid;
    laid.reserve(static_cast<std::size_t>(pages.size()));
    qsizetype items = 0;
    for (qsizetype index = 0; index < pages.size(); ++index) {
        const auto layout = layoutOf(pages.at(index).toObject());
        if (!layout)
            return std::unexpected(problem(Problem::Kind::PageSize, index, index));
        laid.push_back(*layout);
        items += pages.at(index).toObject().value("items"_L1).toArray().size();
    }
    if (items > PdfSketch::maxItems)
        return std::unexpected(problem(Problem::Kind::TooManyItems));
    return laid;
}

void drawPath(QPainter &painter, const QJsonObject &item) {
    const QJsonArray points = item.value("points"_L1).toArray();
    if (points.size() < 2)
        return;
    QPainterPath path(QPointF(points.at(0).toDouble(), points.at(1).toDouble()));
    for (qsizetype at = 2; at + 1 < points.size(); at += 2)
        path.lineTo(points.at(at).toDouble(), points.at(at + 1).toDouble());
    // One point is a dot as wide as the pen.
    if (points.size() < 4)
        path.lineTo(path.currentPosition());
    if (item.value("closed"_L1).toBool())
        path.closeSubpath();
    const double width = item.value("width"_L1).toDouble(1);
    painter.setPen(width > 0 ? QPen(colorOf(item, u"color"_s, Qt::black), width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)
                             : QPen(Qt::NoPen));
    painter.setBrush(item.contains("fill"_L1) ? QBrush(colorOf(item, u"fill"_s, Qt::black)) : QBrush(Qt::NoBrush));
    painter.drawPath(path);
}

[[nodiscard]] QString familyOf(const QString &font, const PdfSketch::Fonts &fonts) {
    if (font.isEmpty() || font == "app"_L1)
        return fonts.app;
    if (font == "reader"_L1)
        return fonts.reader.isEmpty() ? fonts.app : fonts.reader;
    if (font == "mono"_L1)
        return fonts.mono;
    return font;
}

void drawText(QPainter &painter, const QJsonObject &item, const PdfSketch::Fonts &fonts) {
    QFont font(familyOf(item.value("font"_L1).toString(), fonts));
    font.setPointSizeF(std::clamp(item.value("size"_L1).toDouble(12), 1.0, 1000.0));
    font.setBold(item.value("bold"_L1).toBool());
    font.setItalic(item.value("italic"_L1).toBool());
    painter.setFont(font);
    painter.setPen(colorOf(item, u"color"_s, Qt::black));
    const double width = item.value("width"_L1).toDouble();
    const QRectF box(item.value("x"_L1).toDouble(), item.value("y"_L1).toDouble(), width > 0 ? width : PdfSketch::maxSide,
                     PdfSketch::maxSide);
    QTextOption option(Qt::AlignLeft | Qt::AlignTop);
    option.setWrapMode(width > 0 ? QTextOption::WordWrap : QTextOption::NoWrap);
    painter.drawText(box, item.value("text"_L1).toString(), option);
}

// The pictures as they are decoded, each once however often it is drawn.
class Pictures {
public:
    explicit Pictures(std::span<const QByteArray> given) : m_given(given) {}

    [[nodiscard]] const QImage *at(qsizetype index) {
        if (index < 0 || std::cmp_greater_equal(index, m_given.size()))
            return nullptr;
        auto [found, fresh] = m_decoded.try_emplace(index);
        if (fresh)
            found->second = QImage::fromData(m_given[static_cast<std::size_t>(index)]);
        return found->second.isNull() ? nullptr : &found->second;
    }

private:
    std::span<const QByteArray> m_given;
    std::map<qsizetype, QImage> m_decoded;
};

[[nodiscard]] bool drawImage(QPainter &painter, const QJsonObject &item, Pictures &pictures) {
    const QImage *picture = pictures.at(item.value("image"_L1).toInteger(-1));
    if (picture == nullptr)
        return false;
    // A side not given keeps the picture's shape; neither is its size in points.
    double width = item.value("width"_L1).toDouble();
    double height = item.value("height"_L1).toDouble();
    if (width <= 0 && height <= 0) {
        width = picture->width();
        height = picture->height();
    } else if (width <= 0) {
        width = height * picture->width() / picture->height();
    } else if (height <= 0) {
        height = width * picture->height() / picture->width();
    }
    painter.drawImage(QRectF(item.value("x"_L1).toDouble(), item.value("y"_L1).toDouble(), width, height), *picture);
    return true;
}

// The items of a page; which one is no picture, when one isn't.
[[nodiscard]] std::optional<qsizetype> drawPage(QPainter &painter, const QJsonArray &items, Pictures &pictures,
                                                const PdfSketch::Fonts &fonts) {
    for (qsizetype index = 0; index < items.size(); ++index) {
        const QJsonObject item = items.at(index).toObject();
        const QString type = item.value("type"_L1).toString();
        bool drawn = true;
        painter.save();
        if (type == "path"_L1)
            drawPath(painter, item);
        else if (type == "text"_L1)
            drawText(painter, item, fonts);
        else if (type == "image"_L1)
            drawn = drawImage(painter, item, pictures);
        painter.restore();
        if (!drawn)
            return index;
    }
    return std::nullopt;
}

} // namespace

std::expected<QByteArray, Problem> PdfSketch::render(const QJsonObject &sketch, std::span<const QByteArray> pictures, const Fonts &fonts) {
    const QJsonArray pages = sketch.value("pages"_L1).toArray();
    const auto laid = layouts(pages);
    if (!laid)
        return std::unexpected(laid.error());
    QByteArray written;
    QBuffer buffer(&written);
    if (!buffer.open(QIODevice::WriteOnly))
        return std::unexpected(problem(Problem::Kind::Unwritable));
    QPdfWriter writer(&buffer);
    writer.setResolution(72); // a unit of the painter is a point
    writer.setTitle(sketch.value("title"_L1).toString());
    writer.setCreator(u"Omabook"_s);
    writer.setPageLayout(laid->front());
    QPainter painter;
    if (!painter.begin(&writer))
        return std::unexpected(problem(Problem::Kind::Unwritable));
    painter.setRenderHint(QPainter::Antialiasing);
    Pictures decoded(pictures);
    for (qsizetype index = 0; index < pages.size(); ++index) {
        const QJsonObject page = pages.at(index).toObject();
        if (index > 0) {
            writer.setPageLayout(laid->at(static_cast<std::size_t>(index)));
            writer.newPage();
        }
        if (const auto unread = drawPage(painter, page.value("items"_L1).toArray(), decoded, fonts)) {
            painter.end();
            return std::unexpected(problem(Problem::Kind::Picture, index, *unread));
        }
    }
    painter.end();
    if (written.isEmpty())
        return std::unexpected(problem(Problem::Kind::Unwritable));
    return written;
}
