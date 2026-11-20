#include "bookplace.h"

#include <QtGlobal>
#include <cstdlib>
#include <utility>

#include "booktext.h"
#include "passagematch.h"

namespace {

// The words kept on each side of a passage, and how far back or on they are looked for.
constexpr qsizetype contextWords = 8;
constexpr qsizetype contextReach = 160;
// How much less likely it is, each section away from its own, and at most.
constexpr double sectionAway = 0.02;
constexpr double farthestAway = 0.2;

[[nodiscard]] QString contextBefore(QStringView text, qsizetype start) {
    const qsizetype from = qMax(qsizetype{0}, start - contextReach);
    const QList<PassageMatch::Word> words = PassageMatch::words(text.sliced(from, start - from));
    if (words.isEmpty())
        return {};
    // A word cut at the start of the reach is no word of the text.
    qsizetype first = from > 0 ? 1 : 0;
    if (words.size() > contextWords)
        first = words.size() - contextWords;
    return first < words.size() ? text.sliced(from + words[first].start, start - from - words[first].start).toString() : QString();
}

[[nodiscard]] QString contextAfter(QStringView text, qsizetype end) {
    const qsizetype to = qMin(text.size(), end + contextReach);
    const QList<PassageMatch::Word> words = PassageMatch::words(text.sliced(end, to - end));
    if (words.isEmpty())
        return {};
    const qsizetype last = words.size() > contextWords ? contextWords - 1 : words.size() - (to < text.size() ? 2 : 1);
    return last >= 0 ? text.sliced(end, words[last].end).toString() : QString();
}

} // namespace

QJsonObject BookPlace::toJson() const { return toJson({}); }

QJsonObject BookPlace::toJson(QJsonObject alongside) const {
    alongside.insert("section", section);
    alongside.insert("fraction", fraction);
    return alongside;
}

BookPlace BookPlace::fromJson(const QJsonObject &object) { return fromJson(object, {}); }

BookPlace BookPlace::fromJson(const QJsonObject &object, const BookPlace &whenMissing) {
    return {.section = object.contains("section") ? object.value("section").toInt() : whenMissing.section,
            .fraction = object.contains("fraction") ? object.value("fraction").toDouble() : whenMissing.fraction};
}

BookPlace PageRegion::place(double aspect) const {
    return {.section = page, .fraction = aspect > 0 ? qBound(0.0, area.top() / aspect, 1.0) : 0.0};
}

QJsonObject PageRegion::toJson() const {
    return {{"page", page}, {"x", area.x()}, {"y", area.y()}, {"width", area.width()}, {"height", area.height()}};
}

PageRegion PageRegion::fromJson(const QJsonObject &object) {
    return {.page = object.value("page").toInt(),
            .area = QRectF(object.value("x").toDouble(), object.value("y").toDouble(), object.value("width").toDouble(),
                           object.value("height").toDouble())};
}

QJsonObject Passage::toJson() const {
    QJsonObject object{{"from", from.toJson()}, {"to", to.toJson()}};
    for (const auto &[key, words] : {std::pair{"quote", quote}, std::pair{"before", before}, std::pair{"after", after}})
        if (!words.isEmpty())
            object.insert(key, words);
    return object;
}

Passage Passage::fromJson(const QJsonObject &object) {
    const BookPlace from = BookPlace::fromJson(object.value("from").toObject());
    // A passage that says only where it starts is the one place.
    return {.from = from,
            .to = object.contains("to") ? BookPlace::fromJson(object.value("to").toObject()) : from,
            .quote = object.value("quote").toString(),
            .before = object.value("before").toString(),
            .after = object.value("after").toString()};
}

Passage Passage::of(QStringView sectionText, int section, qsizetype start, qsizetype end) {
    const qsizetype length = sectionText.size();
    start = qBound(qsizetype{0}, start, length);
    end = qBound(start, end, length);
    const auto at = [section, length](qsizetype offset) {
        return BookPlace{.section = section, .fraction = length > 0 ? static_cast<double>(offset) / static_cast<double>(length) : 0};
    };
    return {.from = at(start),
            .to = at(end),
            .quote = sectionText.sliced(start, end - start).toString(),
            .before = contextBefore(sectionText, start),
            .after = contextAfter(sectionText, end)};
}

std::optional<FoundPassage> Passage::findAgain(const BookText &text) const {
    if (text.sections() == 0 || PassageMatch::words(quote).isEmpty())
        return std::nullopt;
    const int home = qBound(0, from.section, text.sections() - 1);
    std::optional<std::pair<int, PassageMatch::Match>> best;
    double bestRank = 0;
    // Its own section, then those beside it, one further each time.
    for (int step = 0; step < 2 * text.sections(); ++step) {
        const int away = (step + 1) / 2;
        const int section = step % 2 == 1 ? home + away : home - away;
        if (section < 0 || section >= text.sections())
            continue;
        const auto match =
            PassageMatch::find(text.index(section), {.quote = quote, .before = before, .after = after, .near = from.fraction});
        if (!match)
            continue;
        const double rank = match->confidence - (0.1 * match->distance) - qMin(farthestAway, sectionAway * away);
        if (!best || rank > bestRank) {
            best = std::pair{section, *match};
            bestRank = rank;
        }
        // The passage itself, the nearest it can be: nothing further is likelier.
        if (best->second.sure())
            break;
    }
    if (!best)
        return std::nullopt;
    const auto &[section, match] = *best;
    return FoundPassage{.passage = of(text.text(section), section, match.start, match.end), .confidence = match.confidence};
}

std::optional<std::pair<qsizetype, qsizetype>> Passage::spanIn(QStringView shown) const {
    const auto match =
        PassageMatch::find(PassageMatch::Index(shown), {.quote = quote, .before = before, .after = after, .near = from.fraction});
    return match ? std::optional(std::pair{match->start, match->end}) : std::nullopt;
}
