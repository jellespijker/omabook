#include "readingformat.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <ranges>

namespace ReadingFormat {

QStringList keys() {
    QStringList keys = ranges | std::views::transform([](const Range &range) { return QString(range.key); })
                       | std::ranges::to<QStringList>();
    return keys << "font_family" << "align";
}

const Range *range(QAnyStringView key) {
    const auto found = std::ranges::find_if(ranges, [key](const Range &range) { return QAnyStringView::equal(key, range.key); });
    return found == ranges.end() ? nullptr : &*found;
}

QString problem(const QString &key, const QJsonValue &value) {
    if (const Range *numeric = range(key)) {
        const double number = value.toDouble(-1e9);
        if (!value.isDouble() || number != std::floor(number) || number < numeric->low || number > numeric->high)
            return QString("%1 is %2 to %3 (%4); %5 is not.")
                .arg(key)
                .arg(numeric->low)
                .arg(numeric->high)
                .arg(numeric->unit == "%"_L1 ? QString("percent") : numeric->unit == "px"_L1 ? QString("pixels") : QString(numeric->unit))
                .arg(value.isDouble() ? QString::number(number) : value.toVariant().toString());
        return {};
    }
    if (key == "font_family")
        return value.isString() && value.toString().size() <= 200 ? QString() : QString("font_family is the name of a font, or \"\" for the desktop's.");
    if (key == "align")
        return std::ranges::contains(alignments, value.toString()) ? QString()
                                                                   : QString("align is start, justify, center or end; %1 is not.").arg(value.toVariant().toString());
    return QString("%1 isn't a reading format option; they are %2.").arg(key, keys().join(", "));
}

QJsonObject fitting(const QJsonObject &options) {
    QJsonObject kept;
    for (const QString &key : keys())
        if (options.contains(key) && problem(key, options.value(key)).isEmpty())
            kept.insert(key, options.value(key));
    return kept;
}

QJsonObject over(const QJsonObject &defaults, const QJsonObject &own) {
    QJsonObject options = defaults;
    for (auto it = own.begin(); it != own.end(); ++it)
        options.insert(it.key(), it.value());
    return options;
}

QJsonObject bounds() {
    QJsonObject bounds;
    for (const Range &range : ranges)
        bounds.insert(range.key, QJsonObject{{"low", range.low}, {"high", range.high}, {"default", range.fallback},
                                             {"step", range.step}, {"unit", QString(range.unit)}});
    bounds.insert("align", QJsonObject{{"choices", QJsonArray{"start", "justify", "center", "end"}}, {"default", "start"}});
    bounds.insert("font_family", QJsonObject{{"default", ""}});
    return bounds;
}

QString setChapter(const QString &html, const QJsonObject &options, const QColor &links) {
    const auto number = [&options](QLatin1StringView key) { return options.value(key).toInt(range(key)->fallback); };
    const QString align = options.value("align").toString("start");
    // Qt reads "left" and "right" against the paragraph's own direction,
    // which is what start and end mean; justify it only takes from the
    // paragraph (below).
    const QString textAlign = align == "end" ? " text-align: right;" : align == "center" ? " text-align: center;" : "";
    QString styled = QString("<style>a { color: %1; text-decoration: none; } "
                             "p, li, blockquote, dd { line-height: %2%; } "
                             "p { margin-top: 0; margin-bottom: %3px;%4 } "
                             "h1, h2, h3 { margin-top: 24px; margin-bottom: 16px; }</style>")
                         .arg(links.name())
                         .arg(number("line_height"_L1))
                         .arg(number("paragraph_spacing"_L1))
                         .arg(textAlign);
    // An indent under centred lines would only push them off the middle.
    const int indent = align == "center" ? 0 : number("indent"_L1);
    const QString justified = align == "justify" ? QStringLiteral(" align=\"justify\"") : QString();
    if (indent == 0 && justified.isEmpty())
        return styled + html;
    const QString plain = "<p" + justified + ">";
    const QString indented = "<p" + justified + QString(" style=\"text-indent: %1px\">").arg(indent);
    // Cleaned HTML writes a paragraph the book didn't style as a bare <p>.
    static const QRegularExpression afterHeading("</h[1-6]>\\s*(?:<a name=\"[^\"]*\"></a>\\s*)*$");
    styled.reserve(styled.size() + html.size() + html.size() / 20);
    qsizetype from = 0;
    for (qsizetype at = html.indexOf("<p>"_L1); at >= 0; at = html.indexOf("<p>"_L1, from)) {
        const QStringView before = QStringView(html).mid(from, at - from);
        styled += before;
        const bool first = indent > 0 && afterHeading.matchView(QStringView(html).mid(std::max<qsizetype>(0, at - 200), std::min<qsizetype>(at, 200))).hasMatch();
        styled += indent == 0 || first ? plain : indented;
        from = at + 3;
    }
    styled += QStringView(html).mid(from);
    return styled;
}

} // namespace ReadingFormat
