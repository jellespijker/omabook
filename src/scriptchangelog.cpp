#include "scriptchangelog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <utility>

#include "library.h"
#include "plugindata.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QStringList keysOf(const QString &json) {
    QStringList keys;
    for (const auto &key : QJsonDocument::fromJson(json.toUtf8()).array())
        keys << (key.isString() ? key.toString() : QString::fromUtf8(QJsonDocument(QJsonArray{key}).toJson(QJsonDocument::Compact)));
    return keys;
}

[[nodiscard]] QString compact(const QJsonValue &value) {
    if (value.isArray())
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    if (value.isObject())
        return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
    return u"null"_s;
}

} // namespace

ScriptChangeLog::ScriptChangeLog(QString plugin, LibraryOf library, Fail fail)
    : m_plugin(std::move(plugin)), m_library(std::move(library)), m_fail(std::move(fail)) {}

void ScriptChangeLog::set(const QString &entity, const QString &id, const QString &fields) {
    if (const Library *library = m_library())
        if (const auto written = PluginData::set(*library, {.plugin = m_plugin, .entity = entity, .id = id},
                                                 QJsonDocument::fromJson(fields.toUtf8()).object());
            !written)
            m_fail(written.error());
}

void ScriptChangeLog::add(const QString &entity, const QString &id, const QString &set, const QString &items, const QString &removed) {
    if (const Library *library = m_library())
        if (const auto written =
                PluginData::change(*library, {.plugin = m_plugin, .entity = entity, .id = id}, set,
                                   {.added = QJsonDocument::fromJson(items.toUtf8()).object(), .removed = keysOf(removed)});
            !written)
            m_fail(written.error());
}

void ScriptChangeLog::remove(const QString &entity, const QString &id, const QString &set, const QString &keys) {
    const Library *library = m_library();
    if (library == nullptr)
        return;
    const PluginData::Thing thing{.plugin = m_plugin, .entity = entity, .id = id};
    if (const auto written = set.isEmpty() ? PluginData::remove(*library, thing)
                                           : PluginData::change(*library, thing, set, {.added = {}, .removed = keysOf(keys)});
        !written)
        m_fail(written.error());
}

QString ScriptChangeLog::read(const QString &entity, const QString &id) {
    const Library *library = m_library();
    return library != nullptr ? compact(PluginData::read(*library, {.plugin = m_plugin, .entity = entity, .id = id})) : u"null"_s;
}

QString ScriptChangeLog::list(const QString &entity) {
    const Library *library = m_library();
    return compact(library != nullptr ? PluginData::list(*library, m_plugin, entity) : QJsonArray());
}
