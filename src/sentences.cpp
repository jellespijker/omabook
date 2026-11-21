#include "sentences.h"

#include <QString>
#include <QTextBoundaryFinder>
#include <algorithm>

namespace {

[[nodiscard]] bool hasWords(QStringView text) {
    return std::ranges::any_of(text, [](QChar c) { return c.isLetterOrNumber(); });
}

// A piece without the space around it; empty when it has no words.
[[nodiscard]] Sentences::Sentence trimmed(QStringView text, qsizetype start, qsizetype end) {
    while (start < end && text[start].isSpace())
        ++start;
    while (end > start && text[end - 1].isSpace())
        --end;
    return hasWords(text.sliced(start, end - start)) ? Sentences::Sentence{start, end} : Sentences::Sentence{};
}

// Where a long sentence is cut: after the last comma, semicolon or colon
// before the longest, else the last space, else at the longest itself.
[[nodiscard]] qsizetype cutOf(QStringView text, qsizetype start, qsizetype end) {
    const qsizetype limit = std::min(end, start + Sentences::longest);
    qsizetype space = -1;
    for (qsizetype at = limit - 1; at > start + Sentences::longest / 4; --at) {
        const QChar c = text[at];
        if ((c == u',' || c == u';' || c == u':') && at + 1 < end && text[at + 1].isSpace())
            return at + 1;
        if (space < 0 && c.isSpace())
            space = at;
    }
    return space > 0 ? space : limit;
}

void add(QList<Sentences::Sentence> &into, QStringView text, qsizetype start, qsizetype end) {
    while (end - start > Sentences::longest) {
        const qsizetype cut = cutOf(text, start, end);
        if (const auto piece = trimmed(text, start, cut); piece.end > piece.start)
            into << piece;
        start = cut;
    }
    if (const auto piece = trimmed(text, start, end); piece.end > piece.start)
        into << piece;
}

} // namespace

QList<Sentences::Sentence> Sentences::of(QStringView text) {
    QList<Sentence> found;
    const QString whole = text.toString();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, whole);
    qsizetype start = 0;
    for (qsizetype end = finder.toNextBoundary(); end > 0; end = finder.toNextBoundary()) {
        // A line break inside what the finder takes for one sentence (a
        // heading over a paragraph, lines of verse) ends it too.
        qsizetype from = start;
        for (qsizetype at = start; at < end; ++at) {
            if (text[at] != u'\n' && text[at] != QChar::ParagraphSeparator)
                continue;
            add(found, text, from, at);
            from = at + 1;
        }
        add(found, text, from, end);
        start = end;
    }
    return found;
}

qsizetype Sentences::at(const QList<Sentence> &sentences, qsizetype offset) {
    const auto found = std::ranges::find_if(sentences, [offset](const Sentence &one) { return one.end > offset; });
    return found == sentences.end() ? -1 : found - sentences.begin();
}
