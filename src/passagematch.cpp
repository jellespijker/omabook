#include "passagematch.h"

#include <QChar>
#include <QString>
#include <algorithm>
#include <cmath>
#include <ranges>
#include <utility>
#include <vector>

namespace PassageMatch {
namespace {

constexpr QChar softHyphen{0x00AD};
// The words around a passage that are compared, at most, on each side.
constexpr qsizetype contextWords = 8;
// Of a passage's words (and those around it), the rarest few in the text are
// looked up to say where it may be; a word that occurs this often says
// little, and is looked up only when nothing rarer is there.
constexpr int anchorWords = 6;
constexpr qsizetype commonWord = 400;
// Of the places they point to, the most likely few are compared word by word.
constexpr qsizetype likelyPlaces = 8;

// Writing without spaces between its words, read a character at a time.
[[nodiscard]] bool standsAlone(QChar c) {
    switch (c.script()) {
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
    case QChar::Script_Thai:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool inWord(QChar c) { return c.isLetterOrNumber() || c.isMark() || c.isSurrogate(); }

// The passage as words: its own, and the few around it.
struct Sequence {
    QList<size_t> quote, before, after;
};

[[nodiscard]] QList<size_t> keys(QStringView text) {
    return words(text) | std::views::transform(&Word::key) | std::ranges::to<QList<size_t>>();
}

// The words of the text from `first` to `last`, and how many of the passage's are among them.
struct Span {
    int first = 0;
    int last = 0;
    int matched = 0;
};

// The longest run of the passage's words, in order, among those of the text
// from `from` to `to`, and the stretch of the text it covers, kept as short
// as the run allows.
[[nodiscard]] std::optional<Span> aligned(const QList<Word> &text, const QList<size_t> &quote, int from, int to) {
    const auto n = static_cast<int>(quote.size());
    const int m = to - from;
    if (m <= 0)
        return std::nullopt;
    std::vector<int> table((static_cast<size_t>(n) + 1) * (static_cast<size_t>(m) + 1), 0);
    const size_t width = static_cast<size_t>(m) + 1;
    const auto at = [&table, width](int i, int j) -> int & { return table[(static_cast<size_t>(i) * width) + static_cast<size_t>(j)]; };
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            at(i, j) = quote[i - 1] == text[from + j - 1].key ? at(i - 1, j - 1) + 1 : std::max(at(i - 1, j), at(i, j - 1));
    if (at(n, m) == 0)
        return std::nullopt;
    // Ending at the first word that completes the run, and each word taken as
    // late as it can be, so the stretch starts and ends at the passage's words.
    int j = 1;
    while (at(n, j) < at(n, m))
        ++j;
    Span span{.first = -1, .last = -1, .matched = at(n, m)};
    for (int i = n; i > 0 && j > 0;) {
        if (quote[i - 1] == text[from + j - 1].key && at(i, j) == at(i - 1, j - 1) + 1) {
            span.first = from + j - 1;
            if (span.last < 0)
                span.last = span.first;
            --i;
            --j;
        } else if (at(i - 1, j) >= at(i, j - 1)) {
            --i;
        } else {
            --j;
        }
    }
    return span;
}

[[nodiscard]] bool exactAt(const QList<Word> &text, const QList<size_t> &quote, int start) {
    if (start < 0 || start + quote.size() > text.size())
        return false;
    return std::ranges::equal(quote, text.sliced(start, quote.size()), {}, {}, &Word::key);
}

// How many words of `sought` are, in order, among those of the text from `from` to `to`.
[[nodiscard]] int common(const QList<Word> &text, const QList<size_t> &sought, int from, int to) {
    from = std::clamp(from, 0, static_cast<int>(text.size()));
    to = std::clamp(to, from, static_cast<int>(text.size()));
    if (sought.isEmpty() || to == from)
        return 0;
    const std::optional<Span> span = aligned(text, sought, from, to);
    return span ? span->matched : 0;
}

// How many of the words around the passage are around this stretch too, or,
// when it was kept with none, as sure as its own words are.
[[nodiscard]] double contextAround(const QList<Word> &text, const Sequence &sought, const Span &span, double words) {
    const auto before = static_cast<int>(sought.before.size());
    const auto after = static_cast<int>(sought.after.size());
    if (before + after == 0)
        return words;
    const int found = common(text, sought.before, span.first - before - 2, span.first)
                      + common(text, sought.after, span.last + 1, span.last + 1 + after + 2);
    return static_cast<double>(found) / (before + after);
}

// Whether that is enough of it to say it is the passage: all its words, or
// most of them; of one of a word or two, half and the words around it.
[[nodiscard]] bool enough(double words, double context, qsizetype length) {
    if (words >= 1)
        return true;
    if (words >= 0.5 && context >= 0.8)
        return true;
    return length > 2 && words >= 0.6;
}

[[nodiscard]] double whereIn(const Index &index, int word) {
    const QList<Word> &text = index.words();
    if (text.isEmpty() || index.length() == 0)
        return 0;
    return static_cast<double>(text[std::clamp(word, 0, static_cast<int>(text.size()) - 1)].start) / static_cast<double>(index.length());
}

[[nodiscard]] std::optional<Match> matchAt(const Index &index, const Sequence &sought, int start, double near) {
    const QList<Word> &text = index.words();
    const auto n = static_cast<int>(sought.quote.size());
    std::optional<Span> span;
    if (exactAt(text, sought.quote, start)) {
        span = Span{.first = start, .last = start + n - 1, .matched = n};
    } else {
        const int slack = std::max(4, n / 2);
        span = aligned(text, sought.quote, std::max(0, start - slack), std::min(static_cast<int>(text.size()), start + n + slack));
    }
    if (!span)
        return std::nullopt;
    const double words = static_cast<double>(span->matched) / std::max(n, span->last - span->first + 1);
    const double context = contextAround(text, sought, *span, words);
    if (!enough(words, context, n))
        return std::nullopt;
    return Match{.start = text[span->first].start,
                 .end = text[span->last].end,
                 .words = words,
                 .context = context,
                 .confidence = (0.75 * words) + (0.25 * context),
                 .distance = std::abs(whereIn(index, span->first) - near)};
}

// Where the passage's first word may be: the places its rarest words (and
// those around it) point to, the most of them agreeing first, then the
// nearest to where it was.
[[nodiscard]] QList<int> likelyStarts(const Index &index, const Sequence &sought, double near) {
    QList<std::pair<size_t, int>> anchors; // a word, and how far from the passage's first word it is
    for (int i = 0; i < sought.quote.size(); ++i)
        anchors.append({sought.quote[i], i});
    for (int i = 0; i < sought.before.size(); ++i)
        anchors.append({sought.before[i], i - static_cast<int>(sought.before.size())});
    for (int i = 0; i < sought.after.size(); ++i)
        anchors.append({sought.after[i], static_cast<int>(sought.quote.size()) + i});
    anchors.removeIf([&index](const auto &anchor) { return index.occurrences(anchor.first).isEmpty(); });
    std::ranges::stable_sort(anchors, {}, [&index](const auto &anchor) { return index.occurrences(anchor.first).size(); });
    QHash<int, int> votes;
    int used = 0;
    for (const auto &[key, offset] : std::as_const(anchors)) {
        const QList<int> places = index.occurrences(key);
        if (used == anchorWords || (used > 0 && places.size() > commonWord))
            break;
        ++used;
        for (const int place : places)
            ++votes[place - offset];
    }
    QList<std::pair<int, int>> ranked;
    ranked.reserve(votes.size());
    for (const auto &[start, count] : votes.asKeyValueRange())
        ranked.append({start, count});
    std::ranges::sort(ranked, [&index, near](const auto &one, const auto &other) {
        if (one.second != other.second)
            return one.second > other.second;
        return std::abs(whereIn(index, one.first) - near) < std::abs(whereIn(index, other.first) - near);
    });
    return ranked | std::views::take(likelyPlaces) | std::views::keys | std::ranges::to<QList<int>>();
}

[[nodiscard]] double rank(const Match &match) { return match.confidence - (0.1 * match.distance); }

} // namespace

QList<Word> words(QStringView text) {
    QList<Word> found;
    QString folded;
    qsizetype start = -1;
    const auto close = [&found, &folded, &start](qsizetype end) {
        if (start >= 0)
            found.append(Word{.start = start, .end = end, .key = qHash(folded)});
        start = -1;
        folded.clear();
    };
    for (qsizetype at = 0; at < text.size(); ++at) {
        const QChar c = text[at];
        // A soft hyphen breaks a word only on the page, never in its words.
        if (c == softHyphen && start >= 0)
            continue;
        if (!inWord(c)) {
            close(at);
        } else if (standsAlone(c)) {
            close(at);
            start = at;
            folded = QString(c).toCaseFolded();
            close(at + 1);
        } else {
            if (start < 0)
                start = at;
            folded += QString(c).toCaseFolded();
        }
    }
    close(text.size());
    return found;
}

Index::Index(QStringView text) : m_words(PassageMatch::words(text)), m_length(text.size()) {
    for (int i = 0; i < m_words.size(); ++i)
        m_where[m_words[i].key].append(i);
}

std::optional<Match> find(const Index &index, const Sought &sought) {
    Sequence sequence{.quote = keys(sought.quote), .before = keys(sought.before), .after = keys(sought.after)};
    if (sequence.quote.isEmpty() || index.words().isEmpty())
        return std::nullopt;
    if (sequence.before.size() > contextWords)
        sequence.before = sequence.before.sliced(sequence.before.size() - contextWords);
    if (sequence.after.size() > contextWords)
        sequence.after = sequence.after.first(contextWords);
    std::optional<Match> best;
    for (const int start : likelyStarts(index, sequence, sought.near)) {
        const std::optional<Match> match = matchAt(index, sequence, start, sought.near);
        if (match && (!best || rank(*match) > rank(*best)))
            best = match;
    }
    return best;
}

} // namespace PassageMatch
