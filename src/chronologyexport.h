#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <expected>

// The chronology as a file other tools read (#45), from the answer of
// ChronologyOps::read, so it is cut where the reader is as that is:
//
// - json: the answer as it is;
// - csv: one row per placed event on each lane (timeline, rank, the time as
//   the book says it, its extent on the lane's calendar, and where the time
//   was revealed);
// - graphml, dot: the events and timelines, with the order as edges
//   ("before" from each rank to the next, "same time as", "during", and each
//   event "on" its timeline), written as `graph export` writes a graph.
namespace ChronologyExport {

[[nodiscard]] QStringList formats();
[[nodiscard]] std::expected<QByteArray, QString> exported(const QJsonObject &answer, const QString &format);

// A time as the book says it: "1944-06-06", "about 3019 – …", with its own
// calendar where that isn't `calendar`, and the book's words in quotes.
[[nodiscard]] QString said(const QJsonObject &time, const QString &calendar);

} // namespace ChronologyExport
