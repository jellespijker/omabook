#include "numberoptions.h"

#include <QCommandLineParser>
#include <QDate>
#include <QStringList>
#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

std::optional<qint64> Numbers::whole(QStringView text, qint64 least, qint64 most) {
    bool number = false;
    const qint64 value = text.toLongLong(&number);
    return number && value >= least && value <= most ? std::optional(value) : std::nullopt;
}

std::optional<double> Numbers::decimal(QStringView text, double least, double most) {
    bool number = false;
    const double value = text.toDouble(&number);
    return number && std::isfinite(value) && value >= least && value <= most ? std::optional(value) : std::nullopt;
}

std::optional<QList<int>> Numbers::list(QStringView text) {
    QList<int> numbers;
    for (const QStringView item : text.split(u',', Qt::SkipEmptyParts)) {
        const std::optional<qint64> number = whole(item.trimmed(), 1);
        if (!number)
            return std::nullopt;
        numbers << static_cast<int>(*number);
    }
    return numbers;
}

bool Numbers::day(const QString &text) { return text.size() == 10 && QDate::fromString(text, Qt::ISODate).isValid(); }

Numbers::Option Numbers::section(const QString &name) { return {.name = name}; }

Numbers::Option Numbers::fraction() { return {.name = u"fraction"_s, .least = 0, .most = 1, .isWhole = false}; }

Numbers::Option Numbers::book() { return {.name = {}}; }

namespace {

// "a whole number, 0 or more", "a number from 0 to 100".
[[nodiscard]] QString wanted(const Numbers::Option &option) {
    const QString kind = option.isWhole ? u"a whole number"_s : u"a number"_s;
    const QString least = QString::number(option.least, 'g', 15);
    if (option.most >= static_cast<double>(Numbers::anyHigh))
        return u"%1, %2 or more"_s.arg(kind, least);
    return u"%1 from %2 to %3"_s.arg(kind, least, QString::number(option.most, 'g', 15));
}

[[nodiscard]] bool fits(const Numbers::Option &option, const QString &text) {
    if (option.isWhole)
        return Numbers::whole(text, static_cast<qint64>(option.least), static_cast<qint64>(option.most)).has_value();
    return Numbers::decimal(text, option.least, option.most).has_value();
}

} // namespace

std::optional<QString> Numbers::problem(const QCommandLineParser &parser, std::initializer_list<Option> options) {
    for (const Option &option : options) {
        if (option.name.isEmpty()) {
            if (auto why = notNumbers(parser.positionalArguments().mid(0, 1)))
                return why;
        } else if (parser.isSet(option.name) && !fits(option, parser.value(option.name))) {
            return u"--%1 takes %2, not \"%3\"."_s.arg(option.name, wanted(option), parser.value(option.name));
        }
    }
    return std::nullopt;
}

std::optional<QString> Numbers::notNumbers(const QStringList &words) {
    const auto other = std::ranges::find_if(words, [](const QString &word) { return !whole(word); });
    return other == words.end() ? std::nullopt : std::optional(u"\"%1\" is not a number: give it as 0 or more, in digits."_s.arg(*other));
}
