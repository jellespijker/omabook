#include "pluginformats.h"

#include <QJsonArray>

#include "pluginfinder.h"

QList<PluginFormat> PluginFormats::listed(const QJsonObject &registry) {
    QList<PluginFormat> formats;
    for (const auto &value : registry.value("plugins").toArray()) {
        const QJsonObject plugin = value.toObject();
        for (const auto &format : plugin.value("formats").toArray()) {
            const QString extension = format.toObject().value("extension").toString().toLower();
            const QString name = format.toObject().value("name").toString();
            if (extension.isEmpty() || name.isEmpty())
                continue;
            formats << PluginFormat{.extension = extension,
                                    .name = name,
                                    .plugin = plugin.value("name").toString(),
                                    .title = plugin.value("title").toString(),
                                    .address = PluginFinder::addressOf(plugin)};
        }
    }
    return formats;
}

void PluginFormats::know() { Formats::setPluginFormats(listed(PluginFinder::shipped())); }
