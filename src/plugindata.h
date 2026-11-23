#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtTypes>
#include <expected>
#include <span>

#include "spacereplay.h"

class Library;

// A plugin's data as operations in the change log (ADR 0002, #76): entities
// and fields under the plugin's id, signed by the person, replicated between
// one's own computers and compacted like the rest, and never the library's
// rows.
//
// Each plugin writes a space of its own, "plugin:<id>", which the rows'
// reading of the log (live_ops) leaves out, as it does a published or shared
// space; its entities are named "<id>:<entity>" in it, so no plugin's can be
// taken for the library's or another plugin's when compaction keeps the
// latest of each field. A thing is
//
//   - its fields, each the latest written (a tie to the larger device);
//   - its sets ("set:<name>"), of which every operation counts: one adds
//     items by key and removes others (a batch, such as the strokes of one
//     pen pause, or a partial erasing: one removed, two added), and of each
//     item the latest word holds, an addition winning a removal at the same
//     moment, so what two computers added is all kept;
//   - a removal ("removed"), which a later write brings back.
//
// What another computer sends is untrusted: an operation over the limits
// below is refused (whyRefused), never stored.
namespace PluginData {

// The most a value of one operation may hold, in bytes of its JSON, and the
// most items one change of a set may add and remove together.
inline constexpr qsizetype maxValueBytes = qsizetype{64} * 1024;
inline constexpr qsizetype maxItems = 1000;
// The longest name of an entity, field or set, and the longest id of a thing
// or key of an item.
inline constexpr qsizetype maxNameLength = 64;
inline constexpr qsizetype maxIdLength = 256;

// The space a plugin's data is kept in, and whether a space is one.
[[nodiscard]] QString spaceOf(QStringView plugin);
[[nodiscard]] bool isPluginSpace(QStringView space);

// One thing of a plugin's.
struct Thing {
    QString plugin;
    QString entity;
    QString id;
};

// What one change of a set does: items added (by key, each its value) and
// keys removed.
struct Change {
    QJsonObject added;
    QStringList removed;
};

// Each writes in one transaction and answers why nothing was written: a name
// or id out of bounds, or a value over the limits.
std::expected<void, QString> set(const Library &library, const Thing &thing, const QJsonObject &fields);
std::expected<void, QString> change(const Library &library, const Thing &thing, const QString &set, const Change &change);
std::expected<void, QString> remove(const Library &library, const Thing &thing);

// A thing as the log says it now, `{id, fields, sets, updated}` (`updated` in
// seconds), or null when there is none or it was removed.
[[nodiscard]] QJsonValue read(const Library &library, const Thing &thing);
// Every thing of an entity there is, ordered by id.
[[nodiscard]] QJsonArray list(const Library &library, const QString &plugin, const QString &entity);
// The entities a plugin keeps things of, ordered by name.
[[nodiscard]] QStringList entities(const Library &library, const QString &plugin);

// The things `ops` (of one plugin's entity) make, as read() answers each.
[[nodiscard]] QJsonArray reduce(std::span<const SpaceOperation> ops);

// Why an operation of a plugin's space may not be taken from elsewhere:
// empty when it may (and for one of any other space).
[[nodiscard]] QString whyRefused(const SpaceOperation &op);

} // namespace PluginData
