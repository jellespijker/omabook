#include "sentences.h"

#include <QString>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <ranges>

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

[[nodiscard]] qsizetype lengthOf(QStringView line) { return line.trimmed().size(); }

// A page whose lines wrap, with each line break inside a sentence made a
// space, so that only those that end something are left. As long as the
// text, so that a place in one is the same place in the other.
[[nodiscard]] QString unwrapped(QStringView text) {
    QString whole = text.toString();
    const QList<QStringView> lines = text.split(u'\n');
    const qsizetype longestLine = std::ranges::max(lines | std::views::transform(lengthOf));
    qsizetype at = 0;
    for (qsizetype line = 0; line + 1 < lines.size(); ++line) {
        at += lines.at(line).size();
        const QStringView next = lines.at(line + 1).trimmed();
        const bool goesOn = !next.isEmpty() && next.front().isLower();
        const bool full = 3 * lengthOf(lines.at(line)) >= 2 * longestLine;
        if (!next.isEmpty() && (goesOn || full))
            whole[at] = u' ';
        ++at;
    }
    return whole;
}

[[nodiscard]] bool isHyphen(QChar c) { return c == u'-' || c == QChar(0x2010) || c == QChar(0x00AD); }

} // namespace

QList<Sentences::Sentence> Sentences::of(QStringView text, Lines lines) {
    QList<Sentence> found;
    const QString whole = lines == Lines::Wrap ? unwrapped(text) : text.toString();
    QTextBoundaryFinder finder(QTextBoundaryFinder::Sentence, whole);
    qsizetype start = 0;
    for (qsizetype end = finder.toNextBoundary(); end > 0; end = finder.toNextBoundary()) {
        // A line break inside what the finder takes for one sentence (a
        // heading over a paragraph, lines of verse) ends it too.
        qsizetype from = start;
        for (qsizetype at = start; at < end; ++at) {
            if (whole[at] != u'\n' && whole[at] != QChar::ParagraphSeparator)
                continue;
            add(found, whole, from, at);
            from = at + 1;
        }
        add(found, whole, from, end);
        start = end;
    }
    return found;
}

qsizetype Sentences::at(const QList<Sentence> &sentences, qsizetype offset) {
    const auto found = std::ranges::find_if(sentences, [offset](const Sentence &one) { return one.end > offset; });
    return found == sentences.end() ? -1 : found - sentences.begin();
}

QString Sentences::words(QStringView text, const Sentence &sentence, Lines lines) {
    const QStringView said = text.sliced(sentence.start, sentence.end - sentence.start);
    if (lines == Lines::End)
        return said.toString();
    QString joined;
    joined.reserve(said.size());
    for (qsizetype at = 0; at < said.size(); ++at) {
        if (said[at] != u'\n') {
            joined += said[at];
            continue;
        }
        // A word hyphenated at the line's end, going on in small letters on
        // the next, is one word (and a compound broken at its own hyphen
        // loses that hyphen, which a voice does not say anyway).
        const bool broken = joined.size() > 1 && isHyphen(joined.back()) && joined.at(joined.size() - 2).isLetter() && at + 1 < said.size()
                            && said[at + 1].isLower();
        if (broken)
            joined.chop(1);
        else
            joined += u' ';
    }
    return joined;
}
