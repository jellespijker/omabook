#include "textstretches.h"

#include <QRegularExpression>
#include <algorithm>
#include <utility>

#include "booktext.h"

namespace {

// Where each word of a text starts and ends, as characters.
[[nodiscard]] std::vector<std::pair<qsizetype, qsizetype>> wordsOf(const QString &text) {
    static const QRegularExpression word(QStringLiteral("\\S+"));
    std::vector<std::pair<qsizetype, qsizetype>> words;
    for (auto match = word.globalMatch(text); match.hasNext();) {
        const auto found = match.next();
        words.emplace_back(found.capturedStart(), found.capturedEnd());
    }
    return words;
}

} // namespace

std::vector<TextStretches::Stretch> TextStretches::of(const BookText &text, int length, int shared) {
    const auto size = static_cast<size_t>(std::max(1, length));
    const size_t step = std::max<size_t>(1, size - static_cast<size_t>(std::clamp(shared, 0, length - 1)));
    std::vector<Stretch> stretches;
    for (int section = 0; section < text.sections(); ++section) {
        const QString &words = text.text(section);
        const auto at = wordsOf(words);
        for (size_t first = 0; first < at.size(); first += step) {
            const size_t last = std::min(at.size(), first + size) - 1;
            const qsizetype from = at[first].first;
            const qsizetype to = at[last].second;
            stretches.push_back(
                {.from = text.placeOf(section, from), .to = text.placeOf(section, to), .text = words.mid(from, to - from).simplified()});
            if (last + 1 == at.size())
                break;
        }
    }
    return stretches;
}
