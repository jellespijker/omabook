#include "koreaderdocument.h"

#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <numeric>

using namespace Qt::StringLiterals;

namespace {

constexpr quint64 sample = 1024;

[[nodiscard]] double sizeBefore(std::span<const qsizetype> sizes, int section) {
    const auto before = sizes.first(std::min(static_cast<std::size_t>(std::max(section, 0)), sizes.size()));
    return static_cast<double>(std::accumulate(before.begin(), before.end(), qsizetype{0}));
}

// The place `percentage` of the way through sections of these sizes.
[[nodiscard]] BookPlace byPercentage(double percentage, std::span<const qsizetype> sizes) {
    const double total = sizeBefore(sizes, static_cast<int>(sizes.size()));
    if (total <= 0)
        return {};
    const double wanted = std::clamp(percentage, 0.0, 1.0) * total;
    double passed = 0;
    for (std::size_t section = 0; section < sizes.size(); ++section) {
        const auto size = static_cast<double>(sizes[section]);
        if (passed + size >= wanted && size > 0)
            return {static_cast<int>(section), std::clamp((wanted - passed) / size, 0.0, 1.0)};
        passed += size;
    }
    return {static_cast<int>(sizes.size()) - 1, 1};
}

// How far into `section` the percentage of the whole book is, held to the section.
[[nodiscard]] double fractionIn(int section, double percentage, std::span<const qsizetype> sizes) {
    if (section < 0 || static_cast<std::size_t>(section) >= sizes.size() || sizes[section] <= 0)
        return 0;
    const double total = sizeBefore(sizes, static_cast<int>(sizes.size()));
    return std::clamp((percentage * total - sizeBefore(sizes, section)) / static_cast<double>(sizes[section]), 0.0, 1.0);
}

// The number in the first [n] after `step` in an XPointer, from 1; none without one.
[[nodiscard]] std::optional<int> stepNumber(QStringView progress, const QString &step) {
    const QRegularExpression pattern(QRegularExpression::escape(step) + uR"(\[(\d+)\])"_s);
    const auto match = pattern.matchView(progress);
    if (!match.hasMatch())
        return std::nullopt;
    return match.captured(1).toInt();
}

} // namespace

QString KoreaderDocument::hash(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash md5(QCryptographicHash::Md5);
    // KOReader's loop runs i from -1 to 10 and seeks to 1024 << 2i, which for
    // -1 its bit library takes as a shift that leaves nothing: offset 0.
    for (int i = -1; i <= 10; ++i) {
        const auto offset = static_cast<qint64>(i < 0 ? 0 : sample << (2U * static_cast<unsigned>(i)));
        if (offset >= file.size() || !file.seek(offset))
            break;
        md5.addData(file.read(static_cast<qint64>(sample)));
    }
    return QString::fromLatin1(md5.result().toHex());
}

bool KoreaderDocument::paged(const QString &format) {
    static const QStringList pagedFormats{"pdf", "cbz", "cbr", "cb7", "cbt", "djvu", "xps"};
    return pagedFormats.contains(format.toLower());
}

BookPlace KoreaderDocument::placeOf(QStringView progress, double percentage, const QString &format,
                                    std::span<const qsizetype> sectionSizes) {
    const QString kind = format.toLower();
    if (paged(kind)) {
        bool number = false;
        const int page = progress.toInt(&number);
        if (number && page > 0)
            return {page - 1, 0};
        return byPercentage(percentage, sectionSizes);
    }
    std::optional<int> step;
    if (kind == "epub")
        step = stepNumber(progress, u"DocFragment"_s);
    else if (kind == "fb2")
        step = stepNumber(progress, u"/FictionBook/body/section"_s);
    if (!step || *step < 1)
        return byPercentage(percentage, sectionSizes);
    const int section = sectionSizes.empty() ? *step - 1 : std::min(*step - 1, static_cast<int>(sectionSizes.size()) - 1);
    return {section, fractionIn(section, percentage, sectionSizes)};
}

std::optional<QString> KoreaderDocument::progressOf(const BookPlace &place, const QString &format) {
    const QString kind = format.toLower();
    if (paged(kind))
        return QString::number(place.section + 1);
    if (kind == "epub")
        return u"/body/DocFragment[%1]/body"_s.arg(place.section + 1);
    if (kind == "fb2")
        return u"/FictionBook/body/section[%1]"_s.arg(place.section + 1);
    return std::nullopt;
}
