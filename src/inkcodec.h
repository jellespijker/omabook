#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <expected>
#include <span>
#include <vector>

#include "inkstroke.h"

// Strokes as bytes (#78): compact enough to keep thousands in the change log.
// A stroke is a small header (its colour, width, flags (pressure, tilt, a
// highlighter's) and when it began)
// and its points, the first whole and each after it as the difference from
// the one before, every number a varint (zig-zag where it may be negative):
// a point of handwriting takes four to six bytes, against several dozen as
// JSON. Positions are kept to an eighth of an ink unit, pressure to 1/255,
// tilt to a degree, moments to the millisecond.
//
// What is read may come from another computer: anything malformed is an
// error, never a stroke made of it.
namespace Ink {

// A stroke's bytes, without its id (which is its key where it is kept).
[[nodiscard]] QByteArray encode(const Stroke &stroke);
[[nodiscard]] std::expected<Stroke, QString> decode(QByteArrayView bytes, const QString &id);

// Several strokes with their ids, one after the other: a snapshot of a sheet.
[[nodiscard]] QByteArray encodeAll(std::span<const Stroke> strokes);
[[nodiscard]] std::expected<std::vector<Stroke>, QString> decodeAll(QByteArrayView bytes);

// Strokes as the items of a set in the change log, {id: base64 of its bytes},
// and back: a stroke that can't be read is left out, and said why.
[[nodiscard]] QJsonObject items(std::span<const Stroke> strokes);
struct Read {
    std::vector<Stroke> strokes;
    QStringList problems;
};
[[nodiscard]] Read fromItems(const QJsonObject &items);

// The most one batch may hold, as the change log takes one change of a set:
// at most 1000 items, and well inside 64 KiB of JSON (PluginData's limits).
inline constexpr qsizetype maxBatchItems = 1000;
inline constexpr qsizetype maxBatchBytes = qsizetype{56} * 1024;

// A change as ink batches, each `{"add": {id: base64 of the stroke}, "remove": [id]}`,
// the change of a set `omabook.log.add` writes: one, unless it is over a batch's limits.
[[nodiscard]] std::vector<QJsonObject> batches(const Change &change);
// A batch read back. A stroke that can't be read is an error, and nothing of the batch is taken.
[[nodiscard]] std::expected<Change, QString> fromBatch(const QJsonObject &batch);

} // namespace Ink
