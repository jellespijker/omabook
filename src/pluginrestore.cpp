#include "pluginrestore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QSet>
#include <algorithm>
#include <iterator>
#include <utility>

#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// A thing of the plugin's as `from` holds it, {id, fields, sets}, and its entity.
struct Held {
    QString entity;
    QJsonObject thing;
    [[nodiscard]] QString id() const { return thing.value("id"_L1).toString(); }
};

[[nodiscard]] QList<Held> everything(const Library &library, const QString &plugin) {
    QList<Held> held;
    for (const QString &entity : PluginData::entities(library, plugin))
        for (const QJsonValue thing : PluginData::list(library, plugin, entity))
            held << Held{.entity = entity, .thing = thing.toObject()};
    return held;
}

// Whether `held` names one of `ids`: as its own id, or as the value of one of its fields.
[[nodiscard]] bool names(const Held &held, const QSet<QString> &ids) {
    if (ids.contains(held.id()))
        return true;
    const QJsonObject fields = held.thing.value("fields"_L1).toObject();
    for (auto field = fields.begin(); field != fields.end(); ++field)
        if (field.value().isString() && ids.contains(field.value().toString()))
            return true;
    return false;
}

// The root and every thing that names it, or names one that does, and so on.
[[nodiscard]] QList<Held> withWhatNamesIt(QList<Held> all, qsizetype root) {
    QList<Held> chosen{all.takeAt(root)};
    QSet<QString> ids{chosen.first().id()};
    for (bool grew = true; grew;) {
        grew = false;
        for (qsizetype at = 0; at < all.size();) {
            if (!names(all.at(at), ids)) {
                ++at;
                continue;
            }
            ids.insert(all.at(at).id());
            chosen << all.takeAt(at);
            grew = true;
        }
    }
    return chosen;
}

// What of `then` goes back where the live library has `now` (null when gone).
[[nodiscard]] PluginRestore::Put putBack(const PluginData::Thing &thing, const QJsonObject &then, const QJsonValue &now) {
    PluginRestore::Put put{.thing = thing, .fields = {}, .items = {}, .back = !now.isObject()};
    const QJsonObject fieldsNow = now.toObject().value("fields"_L1).toObject();
    const QJsonObject fieldsThen = then.value("fields"_L1).toObject();
    for (auto field = fieldsThen.begin(); field != fieldsThen.end(); ++field)
        if (put.back || fieldsNow.value(field.key()) != field.value())
            put.fields.insert(field.key(), field.value());
    const QJsonObject setsNow = now.toObject().value("sets"_L1).toObject();
    const QJsonObject setsThen = then.value("sets"_L1).toObject();
    for (auto set = setsThen.begin(); set != setsThen.end(); ++set) {
        const QJsonObject itemsNow = put.back ? QJsonObject() : setsNow.value(set.key()).toObject();
        const QJsonObject itemsThen = set.value().toObject();
        QJsonObject lacking;
        for (auto item = itemsThen.begin(); item != itemsThen.end(); ++item)
            if (itemsNow.value(item.key()) != item.value())
                lacking.insert(item.key(), item.value());
        if (!lacking.isEmpty())
            put.items.insert(set.key(), lacking);
    }
    return put;
}

// How much an item takes of a change's value: its key and its JSON, and a little for what goes between.
[[nodiscard]] qsizetype sizeOf(const QString &key, const QJsonValue &value) {
    constexpr qsizetype between = 8;
    return key.toUtf8().size() + QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size() + between;
}

// The items written back in changes each within what one write may hold.
[[nodiscard]] std::expected<void, Failure> addBack(const Library &live, const PluginData::Thing &thing, const QString &set,
                                                   const QJsonObject &items) {
    constexpr qsizetype room = PluginData::maxValueBytes - 64; // the change's own {"add":…,"remove":[]}
    QJsonObject batch;
    qsizetype bytes = 0;
    const auto write = [&] -> std::expected<void, Failure> {
        if (batch.isEmpty())
            return {};
        const auto changed = PluginData::change(live, thing, set, {.added = batch, .removed = {}});
        batch = {};
        bytes = 0;
        if (!changed)
            return std::unexpected(Failure::said(changed.error()));
        return {};
    };
    for (auto item = items.begin(); item != items.end(); ++item) {
        const qsizetype size = sizeOf(item.key(), item.value());
        if (batch.size() >= PluginData::maxItems || bytes + size > room)
            if (const auto written = write(); !written)
                return written;
        batch.insert(item.key(), item.value());
        bytes += size;
    }
    return write();
}

} // namespace

std::expected<PluginRestore::Plan, Failure> PluginRestore::plan(const Library &live, const Library &from, const PluginData::Thing &root) {
    QList<Held> all = everything(from, root.plugin);
    const auto found = std::ranges::find_if(all, [&root](const Held &held) { return held.entity == root.entity && held.id() == root.id; });
    if (found == all.end())
        return std::unexpected(Failure(Failure::Code::RestoreThingNotFound, {root.entity + u' ' + root.id}));
    const qsizetype at = std::distance(all.begin(), found);
    Plan plan;
    for (const Held &held : withWhatNamesIt(std::move(all), at)) {
        const PluginData::Thing thing{.plugin = root.plugin, .entity = held.entity, .id = held.id()};
        Put put = putBack(thing, held.thing, PluginData::read(live, thing));
        if (put.fields.isEmpty() && put.items.isEmpty())
            continue;
        ++(put.back ? plan.comingBack : plan.changingBack)[held.entity];
        for (const QJsonObject &items : std::as_const(put.items))
            plan.items += static_cast<int>(items.size());
        plan.puts << std::move(put);
    }
    return plan;
}

std::expected<PluginRestore::Restored, Failure> PluginRestore::apply(const Library &live, const Plan &plan) {
    const Library::Batch batch(live);
    Restored done;
    for (const Put &put : plan.puts) {
        if (const auto set = PluginData::set(live, put.thing, put.fields); !set)
            return std::unexpected(Failure::said(set.error()));
        for (auto items = put.items.cbegin(); items != put.items.cend(); ++items) {
            if (const auto added = addBack(live, put.thing, items.key(), *items); !added)
                return std::unexpected(added.error());
            done.items += static_cast<int>(items->size());
        }
        ++done.things;
    }
    return done;
}
