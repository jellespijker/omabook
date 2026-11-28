#pragma once

#include <QList>
#include <QString>
#include <QStringView>
#include <initializer_list>
#include <limits>
#include <optional>

class QCommandLineParser;

// Numbers as a person or an agent writes them (#230): the whole of the text,
// finite and in range, or none. Never a 0 nobody asked for.
namespace Numbers {

// The highest a number can be where nothing else limits it.
inline constexpr qint64 anyHigh = std::numeric_limits<int>::max();
// The highest a time in seconds since 1970 can be.
inline constexpr qint64 anyTime = 1'000'000'000'000;

[[nodiscard]] std::optional<qint64> whole(QStringView text, qint64 least = 0, qint64 most = anyHigh);
[[nodiscard]] std::optional<double> decimal(QStringView text, double least = 0, double most = std::numeric_limits<double>::max());
// "1,3,4", each a whole number from 1; none when one of them isn't.
[[nodiscard]] std::optional<QList<int>> list(QStringView text);

// What one of a command's options takes.
struct Option {
    QString name;
    double least = 0;
    double most = static_cast<double>(anyHigh);
    bool isWhole = true;
};

// A place in a book, as every command takes one.
[[nodiscard]] Option section(const QString &name = QStringLiteral("section"));
[[nodiscard]] Option fraction();
// The command's first word, a book's number (#263): the option with no name.
[[nodiscard]] Option book();

// Whether the text is a day there is, written as 2026-03-01 (#286).
[[nodiscard]] bool day(const QString &text);

// Why one of these options, among those given, isn't the number it takes; none
// when all of them are.
[[nodiscard]] std::optional<QString> problem(const QCommandLineParser &parser, std::initializer_list<Option> options);
// Why one of these words isn't a number from 0, such as a book's or a node's.
[[nodiscard]] std::optional<QString> notNumbers(const QStringList &words);

} // namespace Numbers
