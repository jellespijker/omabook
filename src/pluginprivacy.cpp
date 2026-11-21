#include "pluginprivacy.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "log.h"

using namespace Qt::StringLiterals;

QList<PrivateBooks::Kind> PluginPrivacy::listed(const QString &folder) {
    QList<PrivateBooks::Kind> kinds;
    const QDir plugins(folder);
    for (const QString &id : plugins.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile manifest(plugins.filePath(id + u"/plugin.json"_s));
        if (!manifest.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject said = QJsonDocument::fromJson(manifest.readAll()).object();
        for (const QJsonValue format : said.value("private"_L1).toArray())
            if (format.isString())
                kinds << PrivateBooks::Kind{
                    .format = format.toString().trimmed().toLower(), .plugin = id, .title = said.value("name"_L1).toString(id)};
    }
    return kinds;
}

qsizetype PluginPrivacy::record(const Library &library, const QString &folder) {
    qsizetype news = 0;
    for (const PrivateBooks::Kind &kind : listed(folder)) {
        const auto said = PrivateBooks::declare(library, kind);
        if (!said)
            qCWarning(lcPlugins) << "Couldn't say that" << kind.plugin << "keeps its books private:" << said.error().text();
        news += said.value_or(false) ? 1 : 0;
    }
    return news;
}
