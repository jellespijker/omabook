#include "documenttext.h"

#include <QVariantMap>

#include "document.h"

QVariantList DocumentText::find(const Document &document, const QString &words, int most) {
    QVariantList results;
    const auto room = [&results, most] { return most <= 0 || results.size() < most; };
    const QString needle = words.simplified();
    if (needle.size() < 2)
        return results;
    const QList<TocEntry> toc = document.toc();
    for (int section = 0; section < document.count() && room(); ++section) {
        const QString text = document.text(section).simplified();
        QString title;
        for (const TocEntry &entry : toc)
            if (entry.section <= section)
                title = entry.title;
        for (qsizetype at = text.indexOf(needle, 0, Qt::CaseInsensitive); at >= 0 && room();
             at = text.indexOf(needle, at + needle.size(), Qt::CaseInsensitive)) {
            const qsizetype from = qMax<qsizetype>(0, at - 40);
            results << QVariantMap{{"section", section},
                                   {"title", title},
                                   {"fraction", text.isEmpty() ? 0.0 : static_cast<double>(at) / static_cast<double>(text.size())},
                                   {"snippet", (from > 0 ? "…" : "") + text.mid(from, needle.size() + 100) + "…"}};
        }
    }
    return results;
}

QString DocumentText::between(const Document &document, const Span &span, bool withFirstPage) {
    if (span.fromSection > span.toSection)
        return {};
    const bool flows = document.kind() == Document::Flow;
    QString text;
    for (int section = span.fromSection; section <= span.toSection; ++section) {
        if (!flows && section == span.fromSection && !withFirstPage)
            continue;
        const QString whole = document.text(section);
        const qsizetype from =
            flows && section == span.fromSection ? qRound(static_cast<double>(whole.size()) * qBound(0.0, span.fromFraction, 1.0)) : 0;
        const qsizetype to = flows && section == span.toSection
                                 ? qMin(whole.size(), qRound(static_cast<double>(whole.size()) * span.toFraction) + 1)
                                 : whole.size();
        if (to > from)
            text += whole.mid(from, to - from) + "\n\n";
    }
    return text.trimmed();
}
