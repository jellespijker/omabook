#include "plugindata.h"

#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <optional>
#include <utility>

#include "changelog.h"
#include "hlc.h"
#include "library.h"
#include "spacereducer.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto spacePrefix = "plugin:"_L1;
constexpr auto setPrefix = "set:"_L1;
constexpr auto removedField = "removed"_L1;

[[nodiscard]] bool named(QStringView name, const QRegularExpression &pattern) {
    return !name.isEmpty() && name.size() <= PluginData::maxNameLength && pattern.matchView(name).hasMatch();
}

[[nodiscard]] bool isPlugin(QStringView name) {
    static const QRegularExpression pattern(u"^[A-Za-z0-9][A-Za-z0-9._-]*$"_s);
    return named(name, pattern);
}

[[nodiscard]] bool isEntity(QStringView name) {
    static const QRegularExpression pattern(u"^[a-z][a-z0-9_-]*$"_s);
    return named(name, pattern);
}

// A field's or a set's name: never the removal's, and with no colon, so
// none is taken for a set's ("set:<name>").
[[nodiscard]] bool isField(QStringView name) {
    static const QRegularExpression pattern(u"^[A-Za-z][A-Za-z0-9_.-]*$"_s);
    return name != removedField && named(name, pattern);
}

[[nodiscard]] bool isKey(QStringView key) { return !key.isEmpty() && key.size() <= PluginData::maxIdLength; }

// A value as an operation keeps it, its JSON, and back: a bare string or
// number is no JSON document of its own, so it goes in a list to be read.
[[nodiscard]] QString textOf(const QJsonValue &value) {
    const QString list = QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    return list.mid(1, list.size() - 2);
}

[[nodiscard]] std::optional<QJsonValue> valueOf(const QString &text) {
    QJsonParseError error{};
    const QJsonDocument list = QJsonDocument::fromJson((u'[' + text + u']').toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || list.array().size() != 1)
        return std::nullopt;
    return list.array().at(0);
}

[[nodiscard]] QString whyNotThing(const PluginData::Thing &thing) {
    if (!isPlugin(thing.plugin))
        return u"\"%1\" is no plugin's id."_s.arg(thing.plugin);
    if (!isEntity(thing.entity))
        return u"An entity is lower-case letters, digits, - and _, starting with a letter, at most %1 long; this one is \"%2\"."_s
            .arg(PluginData::maxNameLength)
            .arg(thing.entity);
    if (!isKey(thing.id))
        return u"A thing's id is some text, at most %1 long."_s.arg(PluginData::maxIdLength);
    return {};
}

[[nodiscard]] QString whyTooLarge(const QString &value) {
    const qsizetype bytes = value.toUtf8().size();
    return bytes > PluginData::maxValueBytes
               ? u"A value of %1 bytes is over the %2 one write may hold."_s.arg(bytes).arg(PluginData::maxValueBytes)
               : QString();
}

[[nodiscard]] QString whyNotChange(const QJsonObject &change) {
    const QJsonObject added = change.value("add").toObject();
    const QJsonArray removed = change.value("remove").toArray();
    if (added.size() + removed.size() > PluginData::maxItems)
        return u"A change of %1 items is over the %2 one write may make."_s.arg(added.size() + removed.size()).arg(PluginData::maxItems);
    if (!std::ranges::all_of(added.keys(), [](const QString &key) { return isKey(key); })
        || !std::ranges::all_of(removed, [](const QJsonValue &key) { return key.isString() && isKey(key.toString()); }))
        return u"An item's key is some text, at most %1 long."_s.arg(PluginData::maxIdLength);
    return {};
}

void ensureSpace(const Library &library, const QString &space) {
    const QString device = Spaces::deviceId(library);
    const qint64 created = Spaces::lastHlc(library);
    ChangeLog::exec(library, "INSERT OR IGNORE INTO spaces (id, kind, name, owner, created) VALUES (?, 'plugin', ?, ?, ?)",
                    {space, space, device, created});
    ChangeLog::exec(library, "INSERT OR IGNORE INTO space_members (space, person, role, added) VALUES (?, ?, 'writer', ?)",
                    {space, device, created});
}

// Appends each of `written` (a field and its value) to the thing, in one transaction.
std::expected<void, QString> write(const Library &library, const PluginData::Thing &thing,
                                   const QList<std::pair<QString, QString>> &written) {
    if (const QString why = whyNotThing(thing); !why.isEmpty())
        return std::unexpected(why);
    for (const auto &[field, value] : written)
        if (const QString why = whyTooLarge(value); !why.isEmpty())
            return std::unexpected(why);
    if (written.isEmpty())
        return {};
    const Library::Batch batch(library);
    const QString space = PluginData::spaceOf(thing.plugin);
    ensureSpace(library, space);
    for (const auto &[field, value] : written)
        ChangeLog::append(
            library,
            {.space = space, .entity = thing.plugin + u':' + thing.entity, .id = thing.id, .field = field, .value = value, .hlc = {}});
    return {};
}

void takeMax(SpaceStamp &target, const SpaceStamp &candidate) {
    if (candidate > target)
        target = candidate;
}

// A thing as its operations make it. Of a set's items, an addition has
// priority 2 and a removal 1, so an addition wins one at the same moment.
struct Reduced {
    QMap<QString, SpaceStamp> fields;
    QMap<QString, QMap<QString, SpaceStamp>> sets;
    SpaceStamp alive;
    SpaceStamp removed;
    qint64 latest = 0;
};
constexpr int addedItem = 2;
constexpr int removedItem = 1;

void fold(Reduced &thing, const SpaceOperation &op) {
    thing.latest = std::max(thing.latest, op.hlc);
    if (op.field == removedField) {
        takeMax(op.value == "1"_L1 ? thing.removed : thing.alive, {op.hlc, op.device, op.value == "1"_L1 ? 0 : 1, op.value});
        return;
    }
    takeMax(thing.alive, {op.hlc, op.device, 1, {}});
    if (!op.field.startsWith(setPrefix)) {
        takeMax(thing.fields[op.field], {op.hlc, op.device, 1, op.value});
        return;
    }
    QMap<QString, SpaceStamp> &items = thing.sets[op.field.mid(setPrefix.size())];
    const QJsonObject change = QJsonDocument::fromJson(op.value.toUtf8()).object();
    for (const auto &key : change.value("remove").toArray())
        takeMax(items[key.toString()], {op.hlc, op.device, removedItem, {}});
    const QJsonObject added = change.value("add").toObject();
    for (auto item = added.begin(); item != added.end(); ++item)
        takeMax(items[item.key()], {op.hlc, op.device, addedItem, textOf(item.value())});
}

[[nodiscard]] QJsonObject toJson(const QString &id, const Reduced &thing) {
    QJsonObject fields;
    for (auto field = thing.fields.cbegin(); field != thing.fields.cend(); ++field)
        fields.insert(field.key(), valueOf(field->value).value_or(QJsonValue()));
    QJsonObject sets;
    for (auto set = thing.sets.cbegin(); set != thing.sets.cend(); ++set) {
        QJsonObject items;
        for (auto item = set->cbegin(); item != set->cend(); ++item)
            if (item->priority == addedItem)
                items.insert(item.key(), valueOf(item->value).value_or(QJsonValue()));
        sets.insert(set.key(), items);
    }
    return {{"id", id}, {"fields", fields}, {"sets", sets}, {"updated", Hlc::seconds(thing.latest)}};
}

// The operations of one entity of a plugin's, of one thing when `id` is given.
[[nodiscard]] QList<SpaceOperation> operationsOf(const Library &library, const QString &plugin, const QString &entity,
                                                 const QString &id = {}) {
    QString sql = u"SELECT d.name, o.seq, o.hlc, i.name, o.field, o.value FROM space_ops AS o "
                  "JOIN log_ids AS d ON d.id = o.device JOIN log_ids AS i ON i.id = o.id "
                  "WHERE o.space = (SELECT id FROM log_ids WHERE name = ?) AND o.entity = ?"_s;
    QVariantList values{PluginData::spaceOf(plugin), plugin + u':' + entity};
    if (!id.isEmpty()) {
        sql += u" AND o.id = (SELECT id FROM log_ids WHERE name = ?)"_s;
        values << id;
    }
    QList<SpaceOperation> ops;
    for (QSqlQuery query = ChangeLog::query(library, sql, values); query.next();)
        ops << SpaceOperation{.space = PluginData::spaceOf(plugin),
                              .device = query.value(0).toString(),
                              .seq = query.value(1).toLongLong(),
                              .hlc = query.value(2).toLongLong(),
                              .author = {},
                              .entity = plugin + u':' + entity,
                              .id = query.value(3).toString(),
                              .field = query.value(4).toString(),
                              .value = query.value(5).toString()};
    return ops;
}

} // namespace

QString PluginData::spaceOf(QStringView plugin) { return spacePrefix + plugin; }

bool PluginData::isPluginSpace(QStringView space) { return space.startsWith(spacePrefix); }

std::expected<void, QString> PluginData::set(const Library &library, const Thing &thing, const QJsonObject &fields) {
    QList<std::pair<QString, QString>> written;
    for (auto field = fields.begin(); field != fields.end(); ++field) {
        if (!isField(field.key()))
            return std::unexpected(
                u"A field is letters, digits, ., - and _, starting with a letter, at most %1 long, and not \"removed\"; this one is \"%2\"."_s
                    .arg(maxNameLength)
                    .arg(field.key()));
        written.append({field.key(), textOf(field.value())});
    }
    return write(library, thing, written);
}

std::expected<void, QString> PluginData::change(const Library &library, const Thing &thing, const QString &set, const Change &change) {
    if (!isField(set))
        return std::unexpected(u"A set is named as a field is; this one is \"%1\"."_s.arg(set));
    if (change.added.isEmpty() && change.removed.isEmpty())
        return {};
    const QJsonObject value{{"add", change.added}, {"remove", QJsonArray::fromStringList(change.removed)}};
    if (const QString why = whyNotChange(value); !why.isEmpty())
        return std::unexpected(why);
    return write(library, thing, {{setPrefix + set, QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))}});
}

std::expected<void, QString> PluginData::remove(const Library &library, const Thing &thing) {
    return write(library, thing, {{removedField, u"1"_s}});
}

QJsonValue PluginData::read(const Library &library, const Thing &thing) {
    if (!whyNotThing(thing).isEmpty())
        return QJsonValue::Null;
    const QJsonArray things = reduce(operationsOf(library, thing.plugin, thing.entity, thing.id));
    return things.isEmpty() ? QJsonValue(QJsonValue::Null) : things.first();
}

QJsonArray PluginData::list(const Library &library, const QString &plugin, const QString &entity) {
    if (!whyNotThing({.plugin = plugin, .entity = entity, .id = u"-"_s}).isEmpty())
        return {};
    return reduce(operationsOf(library, plugin, entity));
}

QStringList PluginData::entities(const Library &library, const QString &plugin) {
    QStringList named;
    const QString prefix = plugin + u':';
    for (QSqlQuery query = ChangeLog::query(library,
                                            "SELECT DISTINCT entity FROM space_ops WHERE space = (SELECT id FROM log_ids WHERE name = ?) "
                                            "ORDER BY entity",
                                            {spaceOf(plugin)});
         query.next();)
        if (const QString entity = query.value(0).toString(); entity.startsWith(prefix))
            named << entity.mid(prefix.size());
    return named;
}

QJsonArray PluginData::reduce(std::span<const SpaceOperation> ops) {
    QMap<QString, Reduced> things;
    for (const SpaceOperation &op : ops)
        fold(things[op.id], op);
    QJsonArray reduced;
    for (auto thing = things.cbegin(); thing != things.cend(); ++thing)
        if (!SpaceReducer::isRemoved(thing->alive, thing->removed))
            reduced << toJson(thing.key(), *thing);
    return reduced;
}

QString PluginData::whyRefused(const SpaceOperation &op) {
    if (!isPluginSpace(op.space))
        return {};
    const QString plugin = op.space.mid(spacePrefix.size());
    if (!op.entity.startsWith(plugin + u':'))
        return u"An operation of %1 is of another's entity, %2."_s.arg(op.space, op.entity);
    if (const QString why = whyNotThing({.plugin = plugin, .entity = op.entity.mid(plugin.size() + 1), .id = op.id}); !why.isEmpty())
        return why;
    if (const QString why = whyTooLarge(op.value); !why.isEmpty())
        return why;
    if (op.field == removedField)
        return {};
    if (op.field.startsWith(setPrefix)) {
        QJsonParseError error{};
        const QJsonDocument change = QJsonDocument::fromJson(op.value.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !change.isObject() || !isField(op.field.mid(setPrefix.size())))
            return u"A change of a set of %1 is no change of one."_s.arg(op.space);
        return whyNotChange(change.object());
    }
    if (!isField(op.field) || !valueOf(op.value))
        return u"A field of %1 is no field: %2."_s.arg(op.space, op.field);
    return {};
}
