#pragma once

#include <QJsonObject>
#include <QString>
#include <QtTypes>
#include <functional>

// The chronology as `omabook chronology` prints it without --json (#45), for
// a terminal: of each timeline its events in order, one to a line, with the
// time as the book says it and where it was said; what is lined up with
// what; the events not placed yet; and each contradiction with where every
// statement in it was said, and which was set aside.
namespace ChronologyPrint {

// The answer of ChronologyOps::read as text. `collection` names a
// collection by its number, for the sides of a split library.
[[nodiscard]] QString text(const QJsonObject &answer, const std::function<QString(qint64)> &collection);

// The answer of one of the writers, in a line.
[[nodiscard]] QString written(const QJsonObject &answer);

} // namespace ChronologyPrint
