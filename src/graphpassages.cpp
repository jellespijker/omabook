#include "graphpassages.h"

#include <QRegularExpression>

namespace {


// Words as the text may hold them: any case, and any run of white space
// (a line break inside a quote) between them.
QRegularExpression phraseIn(const QString &words) {
    QStringList parts;
    for (const QString &word : words.simplified().split(' ', Qt::SkipEmptyParts))
        parts << QRegularExpression::escape(word);
    return QRegularExpression(parts.join("\\s+"),
                              QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
}

// The passages a model's section number points into: that section's, or
// all of them when it named one that isn't there.
QList<const GraphBuild::Passage *> partsOf(const QList<GraphBuild::Passage> &passages, int section) {
    QList<const GraphBuild::Passage *> within;
    for (const GraphBuild::Passage &passage : passages)
        if (passage.section == section)
            within << &passage;
    if (within.isEmpty())
        for (const GraphBuild::Passage &passage : passages)
            within << &passage;
    return within;
}

} // namespace

QList<GraphBuild::Passage> GraphBuild::passages(const Document &document, const std::optional<GraphPlace> &after, const GraphPlace &upTo) {
    QList<GraphBuild::Passage> found;
    const bool flows = document.kind() == Document::Flow;
    // A flowing section read partly goes on where it was left; a page read is done.
    int first = 0;
    if (after)
        first = flows ? after->section : after->section + 1;
    const int last = qMin(upTo.section, document.count() - 1);
    for (int section = qMax(0, first); section <= last; ++section) {
        const QString whole = document.text(section);
        if (!flows) {
            if (!whole.trimmed().isEmpty())
                found.append({section, 0, 0, whole});
            continue;
        }
        const double from = after && section == after->section ? qBound(0.0, after->fraction, 1.0) : 0.0;
        const double to = section == upTo.section ? qBound(0.0, upTo.fraction, 1.0) : 1.0;
        const qsizetype start = qRound64(static_cast<double>(whole.size()) * from);
        // The line the reader is on has been read, as it is for a catch-up.
        const qsizetype end =
            section == upTo.section ? qMin(whole.size(), qRound64(static_cast<double>(whole.size()) * to) + 1) : whole.size();
        if (end > start && !whole.mid(start, end - start).trimmed().isEmpty())
            found.append({section, from, to, whole.mid(start, end - start)});
    }
    return found;
}

qsizetype GraphBuild::size(const QList<Passage> &passages) {
    qsizetype total = 0;
    for (const GraphBuild::Passage &passage : passages)
        total += passage.text.size();
    return total;
}

int GraphBuild::sectionOf(const QJsonValue &value) {
    if (value.isDouble())
        return value.toInt(-1);
    bool number = false;
    const int section = value.toString().toInt(&number);
    return number ? section : -1;
}

std::optional<GraphMoment> GraphBuild::momentOf(qint64 bookId, const QList<Passage> &passages, int section, const QStringList &phrases) {
    for (const GraphBuild::Passage *passage : partsOf(passages, section)) {
        qsizetype first = -1;
        qsizetype length = 0;
        for (const QString &phrase : phrases) {
            if (phrase.simplified().size() < 2)
                continue;
            const QRegularExpressionMatch found = phraseIn(phrase).match(passage->text);
            if (found.hasMatch() && (first < 0 || found.capturedStart() < first)) {
                first = found.capturedStart();
                length = found.capturedLength();
            }
        }
        if (first >= 0)
            return GraphMoment{
                {bookId, passage->section,
                 passage->from + (passage->to - passage->from) * static_cast<double>(first) / static_cast<double>(passage->text.size())},
                passage->text.mid(first, length).simplified()};
    }
    return std::nullopt;
}

// Never after the passages' end, so a fact is never placed ahead of the reader.
GraphMoment GraphBuild::placeOf(qint64 bookId, const QList<Passage> &passages, int section, const QStringList &names) {
    if (const auto found = momentOf(bookId, passages, section, names))
        return *found;
    const QList<const GraphBuild::Passage *> within = partsOf(passages, section);
    const GraphBuild::Passage *last = within.isEmpty() ? nullptr : within.last();
    return {last ? GraphPlace{bookId, last->section, last->to} : GraphPlace{bookId, 0, 0}, {}};
}

GraphMoment GraphBuild::revealedAt(qint64 bookId, const QList<Passage> &passages, int section, const QJsonValue &quote,
                                   const GraphMoment &fallback) {
    const QString said = quote.toString().simplified().left(200);
    if (said.size() >= 3)
        if (const auto found = momentOf(bookId, passages, section, {said}))
            return *found;
    return fallback;
}
