#include "hyphenation.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QLocale>
#include <QMutex>
#include <QStandardPaths>
#include <QStringDecoder>
#include <algorithm>
#include <map>
#include <memory>
#include <numeric>
#include <ranges>

using namespace Qt::StringLiterals;

namespace Hyphenation {

namespace {

// The decoder for a dictionary's first line. The files name their sets as
// the old OpenOffice code did ("ISO8859-1", "microsoft-cp1251"), which
// aren't all names ICU knows.
QStringDecoder decoderFor(QByteArray name) {
    name = name.trimmed();
    if (name.startsWith("ISO8859"))
        name.insert(3, '-');
    if (name.startsWith("microsoft-cp"))
        name = "windows-" + name.mid(12);
    if (name.compare("ISO-8859-1", Qt::CaseInsensitive) == 0)
        return QStringDecoder(QStringConverter::Latin1);
    return QStringDecoder(name.constData());
}

bool wordCharacter(QChar character) { return character.isLetter() || character.isMark(); }

// Whether a run of letters at [start, end) of some text is a word to break:
// long enough, not in capitals, and not part of a number, an address or a
// word the book broke itself.
// Whether a run of letters at [start, end) of some text is part of
// something else than a word: a number, an address, a name with dots in it
// (example.org, or the org of it), or a word the book broke itself.
bool partOfMore(QStringView text, qsizetype start, qsizetype end) {
    const auto joins = [](QChar character) {
        return character.isDigit() || character == softHyphen || character == u'_' || character == u'/' || character == u'@'
               || character == u'\\';
    };
    const QChar before = start > 0 ? text.at(start - 1) : u' ';
    const QChar after = end < text.size() ? text.at(end) : u' ';
    const bool dotAfter = after == u'.' && end + 1 < text.size() && text.at(end + 1).isLetter();
    const bool dotBefore = before == u'.' && start > 1 && text.at(start - 2).isLetter();
    return joins(before) || joins(after) || dotAfter || dotBefore;
}

// Whether a run of letters at [start, end) of some text is a word to break:
// long enough, not in capitals, and not part of more.
bool breakable(QStringView text, qsizetype start, qsizetype end, const Dictionary &dictionary) {
    if (end - start < std::max(5, dictionary.leftMin() + dictionary.rightMin()))
        return false;
    return std::ranges::any_of(text.mid(start, end - start), [](QChar letter) { return letter.isLower(); })
           && !partOfMore(text, start, end);
}

// Where something that isn't text ends: a tag at its '>', an entity at its ';'.
qsizetype skipped(QStringView html, qsizetype at) {
    if (html.at(at) == u'<') {
        const qsizetype end = html.indexOf(u'>', at);
        return end < 0 ? html.size() : end + 1;
    }
    // An entity is short: &amp;, &#8212;, &eacute;.
    const qsizetype semicolon = html.mid(at, 13).indexOf(u';');
    const qsizetype end = semicolon < 0 ? -1 : at + semicolon;
    const bool entity =
        end > at + 1 && std::ranges::all_of(html.mid(at + 1, end - at - 1), [](QChar c) { return c.isLetterOrNumber() || c == u'#'; });
    return entity ? end + 1 : at + 1;
}

} // namespace

std::expected<Dictionary, QString> Dictionary::read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(QString("%1 can't be read: %2").arg(path, file.errorString()));
    return parse(file.readAll());
}

namespace {

// A pattern's letters and the value before, between and after them.
std::pair<std::u16string, std::vector<std::uint8_t>> pattern(QStringView written) {
    std::u16string letters;
    std::vector<std::uint8_t> values{0};
    for (const QChar character : written) {
        if (character.isDigit()) {
            values.back() = static_cast<std::uint8_t>(character.digitValue());
            continue;
        }
        letters += character.toLower().unicode();
        values.push_back(0);
    }
    return {std::move(letters), std::move(values)};
}

// A setting's number, as "LEFTHYPHENMIN 2" gives it; 0 when the line is not that setting.
int setting(QStringView line, QLatin1StringView name) {
    return line.startsWith(name) && line.size() > name.size() && line.at(name.size()).isSpace() ? line.mid(name.size()).trimmed().toInt()
                                                                                                : 0;
}

} // namespace

bool Dictionary::take(QStringView line, std::vector<Patterns> &levels) {
    if (line.isEmpty() || line.startsWith(u'%') || line.startsWith(u'#'))
        return true;
    if (line.startsWith("NEXTLEVEL"_L1)) {
        // libhyphen reads two levels and no more.
        if (levels.size() == 2)
            return false;
        levels.emplace_back();
        return true;
    }
    // Fewer letters than two around a break are never left, as libhyphen doesn't.
    for (const auto &[name, value] :
         {std::pair{"LEFTHYPHENMIN"_L1, &m_left}, std::pair{"RIGHTHYPHENMIN"_L1, &m_right},
          std::pair{"COMPOUNDLEFTHYPHENMIN"_L1, &m_compoundLeft}, std::pair{"COMPOUNDRIGHTHYPHENMIN"_L1, &m_compoundRight}})
        if (const int given = setting(line, name))
            *value = std::max(2, given);
    // The other settings (NOHYPHEN) are for words joined with a hyphen or
    // an apostrophe, which never reach a dictionary here; a pattern with a
    // slash changes the letters around its break, which a soft hyphen can't.
    if (!line.front().isUpper() && !line.contains(u'/')) {
        auto [letters, values] = pattern(line);
        levels.back().insert_or_assign(std::move(letters), std::move(values));
    }
    return true;
}

std::expected<Dictionary, QString> Dictionary::parse(const QByteArray &bytes) {
    const qsizetype firstEnd = bytes.indexOf('\n');
    QStringDecoder decoder = decoderFor(bytes.left(firstEnd < 0 ? bytes.size() : firstEnd));
    if (!decoder.isValid())
        return std::unexpected(QString("The character set %1 is not known.").arg(QString::fromLatin1(bytes.left(firstEnd).trimmed())));
    const QString text = decoder.decode(firstEnd < 0 ? QByteArray() : bytes.mid(firstEnd + 1));
    Dictionary dictionary;
    std::vector<Patterns> levels(1);
    for (const QStringView line : QStringView(text).split(u'\n'))
        if (!dictionary.take(line.trimmed(), levels))
            break;
    std::erase_if(levels, [](const Patterns &level) { return level.empty(); });
    if (levels.empty())
        return std::unexpected(QString("There are no patterns in it."));
    for (const Patterns &level : levels)
        dictionary.m_levels.push_back(built(level));
    return dictionary;
}

namespace {

constexpr std::uint64_t edge(std::int32_t state, char16_t letter) { return (static_cast<std::uint64_t>(state) << 16) | letter; }

} // namespace

Dictionary::Level Dictionary::built(const Patterns &patterns) {
    Level level;
    // The states as a tree first: each one's parent, the letter to it and its depth.
    std::unordered_map<std::uint64_t, std::int32_t> next;
    std::vector<std::int32_t> parent{-1};
    std::vector<char16_t> letter{0};
    std::vector<std::uint32_t> depth{0};
    level.matchAt.push_back(0);
    level.matchSize.push_back(0);
    for (const auto &[letters, values] : patterns) {
        std::int32_t state = 0;
        for (const char16_t one : letters) {
            const auto [found, added] = next.try_emplace(edge(state, one), static_cast<std::int32_t>(parent.size()));
            if (added) {
                parent.push_back(state);
                letter.push_back(one);
                depth.push_back(depth[static_cast<size_t>(state)] + 1);
                level.matchAt.push_back(0);
                level.matchSize.push_back(0);
            }
            state = found->second;
        }
        level.matchAt[static_cast<size_t>(state)] = static_cast<std::uint32_t>(level.values.size());
        level.matchSize[static_cast<size_t>(state)] = static_cast<std::uint8_t>(values.size());
        level.values.insert(level.values.end(), values.begin(), values.end());
    }
    level.next.assign(next.begin(), next.end());
    std::ranges::sort(level.next);
    // Where each falls back to, shallower states first, as in Aho and
    // Corasick's automaton: from the parent's fallback on by the same letter.
    std::vector<std::int32_t> order(parent.size());
    std::iota(order.begin(), order.end(), 0);
    std::ranges::stable_sort(order, {}, [&depth](std::int32_t state) { return depth[static_cast<size_t>(state)]; });
    level.fallback.assign(parent.size(), 0);
    level.fallback[0] = -1;
    for (const std::int32_t state : order | std::views::drop(1)) {
        const std::int32_t from = parent[static_cast<size_t>(state)];
        level.fallback[static_cast<size_t>(state)] =
            from == 0 ? 0 : level.step(level.fallback[static_cast<size_t>(from)], letter[static_cast<size_t>(state)]);
    }
    return level;
}

std::int32_t Dictionary::Level::step(std::int32_t state, char16_t letter) const {
    while (state >= 0) {
        const auto found = std::ranges::lower_bound(next, edge(state, letter), {}, &std::pair<std::uint64_t, std::int32_t>::first);
        if (found != next.end() && found->first == edge(state, letter))
            return found->second;
        state = fallback[static_cast<size_t>(state)];
    }
    return 0;
}

std::vector<bool> Dictionary::Level::breaks(std::u16string_view word) const {
    std::u16string dotted;
    dotted.reserve(word.size() + 2);
    dotted.append(u".").append(word).append(u".");
    std::vector<std::uint8_t> found(dotted.size() + 1, 0);
    std::int32_t state = 0;
    for (size_t at = 0; at < dotted.size(); ++at) {
        state = step(state, dotted[at]);
        const size_t size = matchSize[static_cast<size_t>(state)];
        if (size == 0)
            continue;
        // The pattern's letters end here; its values start before its first.
        const size_t start = at + 2 - size;
        for (size_t i = 0; i < size; ++i)
            found[start + i] = std::max(found[start + i], values[matchAt[static_cast<size_t>(state)] + i]);
    }
    // Between word[at - 1] and word[at] is between dotted[at] and dotted[at + 1].
    std::vector<bool> breaks(word.size() + 1, false);
    for (size_t at = 1; at < word.size(); ++at)
        breaks[at] = found[at + 1] % 2 == 1;
    return breaks;
}

std::vector<bool> Dictionary::breaks(std::u16string_view word, bool start, bool end, int depth) const {
    std::vector<bool> found = m_levels.front().breaks(word);
    if (m_levels.size() == 1)
        return found;
    // A compound: each of its words is broken as a word of its own, which
    // may be a compound again (as deep as libhyphen goes).
    constexpr int deepest = 32;
    if (depth < deepest && std::ranges::contains(found, true)) {
        std::vector<bool> joined = found;
        size_t begin = 0;
        for (size_t at = 1; at <= word.size(); ++at) {
            if (at < word.size() && !found[at])
                continue;
            if (at - begin > 1) {
                const std::vector<bool> part =
                    breaks(word.substr(begin, at - begin), start && begin == 0, end && at == word.size(), depth + 1);
                std::copy(part.begin() + 1, part.end() - 1, joined.begin() + static_cast<std::ptrdiff_t>(begin) + 1);
            }
            begin = at;
        }
        return joined;
    }
    std::vector<bool> syllables = m_levels.back().breaks(word);
    for (size_t at = 1; at < word.size(); ++at)
        if ((!start && at < static_cast<size_t>(m_compoundLeft)) || (!end && word.size() - at < static_cast<size_t>(m_compoundRight)))
            syllables[at] = false;
    return syllables;
}

QList<qsizetype> Dictionary::points(QStringView word) const {
    std::u16string letters;
    letters.reserve(static_cast<size_t>(word.size()));
    for (const QChar letter : word)
        letters += letter.toLower().unicode();
    const std::vector<bool> breaks = this->breaks(letters, true, true, 0);
    QList<qsizetype> points;
    for (qsizetype at = m_left; at <= word.size() - m_right; ++at)
        if (breaks[static_cast<size_t>(at)])
            points << at;
    return points;
}

QString Dictionary::hyphenated(QStringView word) const {
    const QList<qsizetype> points = this->points(word);
    QString broken;
    broken.reserve(word.size() + points.size());
    qsizetype from = 0;
    for (const qsizetype point : points) {
        broken += word.mid(from, point - from);
        broken += softHyphen;
        from = point;
    }
    broken += word.mid(from);
    return broken;
}

QStringList folders() {
    QStringList folders;
    for (const QString &folder : qEnvironmentVariable("OMABOOK_HYPHENATION_PATH").split(u':', Qt::SkipEmptyParts))
        folders << folder;
    folders << QStandardPaths::locateAll(QStandardPaths::GenericDataLocation, "hyphen", QStandardPaths::LocateDirectory);
    return folders;
}

namespace {

// The file for a language, in the folders: the names tried in order, in each folder in turn.
QString fileFor(const QString &language) {
    const QString given = QString(language).replace(u'-', u'_').trimmed();
    const qsizetype cut = given.indexOf(u'_');
    const QLocale::Language known = QLocale::codeToLanguage(given.left(cut < 0 ? given.size() : cut));
    if (known == QLocale::AnyLanguage || known == QLocale::C)
        return {};
    const QString code = QLocale::languageToCode(known, QLocale::ISO639Part1 | QLocale::ISO639Part2);
    QStringList names;
    if (cut > 0)
        names << QString("hyph_%1_%2.dic").arg(code, given.mid(cut + 1).toUpper());
    names << QString("hyph_%1.dic").arg(QLocale(known).name()) << QString("hyph_%1.dic").arg(code);
    const QStringList folders = Hyphenation::folders();
    for (const QString &name : names)
        for (const QString &folder : folders)
            if (const QString path = QDir(folder).filePath(name); QFile::exists(path))
                return path;
    for (const QString &folder : folders)
        if (const QStringList any = QDir(folder).entryList({QString("hyph_%1_*.dic").arg(code)}, QDir::Files, QDir::Name); !any.isEmpty())
            return QDir(folder).filePath(any.first());
    return {};
}

} // namespace

const Dictionary *forLanguage(const QString &language) {
    static QMutex lock;
    // Kept for the life of the program, so a pointer to one stays good.
    static std::map<QString, std::unique_ptr<const Dictionary>> read;
    static QHash<QString, QString> files;
    const QMutexLocker locked(&lock);
    const auto known = files.constFind(language);
    const QString path = known != files.cend() ? *known : files.insert(language, fileFor(language)).value();
    if (path.isEmpty())
        return nullptr;
    auto found = read.find(path);
    if (found == read.end()) {
        auto dictionary = Dictionary::read(path);
        found = read.emplace(path, dictionary ? std::make_unique<const Dictionary>(std::move(*dictionary)) : nullptr).first;
    }
    return found->second.get();
}

QString hyphenateHtml(QStringView html, const Dictionary &dictionary) {
    QString hyphenated;
    hyphenated.reserve(html.size() + html.size() / 8);
    qsizetype at = 0;
    while (at < html.size()) {
        const QChar character = html.at(at);
        if (character == u'<' || character == u'&') {
            const qsizetype end = skipped(html, at);
            hyphenated += html.mid(at, end - at);
            at = end;
            continue;
        }
        if (!wordCharacter(character)) {
            hyphenated += character;
            ++at;
            continue;
        }
        qsizetype end = at;
        while (end < html.size() && wordCharacter(html.at(end)))
            ++end;
        const QStringView word = html.mid(at, end - at);
        hyphenated += breakable(html, at, end, dictionary) ? dictionary.hyphenated(word) : word.toString();
        at = end;
    }
    return hyphenated;
}

Plain::Plain(QStringView shown) {
    m_text.reserve(shown.size());
    for (qsizetype at = 0; at < shown.size(); ++at) {
        if (shown.at(at) == softHyphen)
            m_hyphens << at;
        else
            m_text += shown.at(at);
    }
}

qsizetype Plain::toPlain(qsizetype shown) const { return shown - (std::ranges::lower_bound(m_hyphens, shown) - m_hyphens.begin()); }

qsizetype Plain::toShown(qsizetype plain) const {
    // The i-th soft hyphen stands before the plain character at its shown position less i.
    qsizetype before = 0;
    for (qsizetype low = 0, high = m_hyphens.size(); low < high;) {
        const qsizetype middle = (low + high) / 2;
        if (m_hyphens.at(middle) - middle <= plain)
            low = before = middle + 1;
        else
            high = middle;
    }
    return plain + before;
}

qsizetype Plain::toShownEnd(qsizetype end) const { return end <= 0 ? 0 : toShown(end - 1) + 1; }

} // namespace Hyphenation
