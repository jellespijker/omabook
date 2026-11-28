// plugin.json, as ScriptManifest (scriptplugin.h) reads it: what a plugin
// says of itself before any of its script runs, so that what it says is
// known while it isn't running too.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "scriptapi.h"
#include "scriptplugin.h"

namespace {

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const auto &item : value.toArray())
        list << item.toString();
    return list;
}

// "formats": [{"extension": "omanotebook", "name": "Notebook", "view": "notebook"}], each
// of the plugin's; `wrong` says what of it isn't right.
QList<PluginFormat> formatsOf(const QJsonValue &value, const ScriptManifest &manifest, QString *wrong) {
    static const QRegularExpression extension("^[a-z0-9][a-z0-9.]*$");
    static const QRegularExpression named("^[a-z0-9_-]+$");
    QList<PluginFormat> formats;
    for (const auto &item : value.toArray()) {
        const QJsonObject format = item.toObject();
        const PluginFormat known{.extension = format.value("extension").toString().toLower(),
                                 .name = format.value("name").toString(),
                                 .plugin = manifest.id,
                                 .title = manifest.name,
                                 .address = {},
                                 .view = format.value("view").toString()};
        if (!extension.match(known.extension).hasMatch() || known.name.isEmpty())
            *wrong = "Each of plugin.json's \"formats\" is {\"extension\": \"txtbook\", \"name\": \"Text book\"}, with a "
                     "\"view\" when a book in it opens in the plugin's view.";
        else if (!known.view.isEmpty() && !named.match(known.view).hasMatch())
            *wrong = "plugin.json's format " + known.name + " names the view \"" + known.view
                     + "\"; a view's id is lower-case letters, digits, - and _.";
        else
            formats << known;
    }
    if (!value.isUndefined() && !value.isArray())
        *wrong = "plugin.json's \"formats\" should be a list.";
    return formats;
}

// Where `script` doesn't parse; none when it does, or can't be read. The
// engine reads it whole before its first statement, which throws, so nothing
// of it is run; the bare engine gives it nothing to reach either.
std::optional<QString> syntaxError(const QString &script, const QString &shownName) {
    QFile file(script);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    QString text = QString::fromUtf8(file.readAll());
    // A "#!" line is only that at the very start, where the throw goes.
    if (text.startsWith(u"#!"))
        text.replace(0, 2, QStringLiteral("//"));
    QJSEngine engine;
    // On the script's first line, so that every line keeps its number.
    const QJSValue read = engine.evaluate(QStringLiteral("throw 0;") + text, shownName);
    if (!read.isError() || read.errorType() != QJSValue::SyntaxError)
        return std::nullopt;
    return shownName + ':' + QString::number(read.property(QStringLiteral("lineNumber")).toInt()) + ": " + read.toString();
}

} // namespace

std::optional<QString> ScriptManifest::whyNotLoaded() const {
    if (!isScript() || needsNewer)
        return std::nullopt;
    if (!problem.isEmpty())
        return problem;
    return syntaxError(folder + "/" + script, script);
}

ScriptManifest ScriptManifest::read(const QString &folder) {
    ScriptManifest manifest;
    manifest.folder = QDir(folder).absolutePath();
    manifest.id = QFileInfo(manifest.folder).fileName();
    manifest.name = manifest.id;
    QFile file(manifest.folder + "/plugin.json");
    if (!file.open(QIODevice::ReadOnly)) {
        manifest.problem = "There is no plugin.json in " + manifest.folder + ".";
        return manifest;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    const QJsonObject object = document.object();
    manifest.name = object.value("name").toString(manifest.id);
    manifest.version = object.value("version").toString();
    manifest.author = object.value("author").toString();
    manifest.description = object.value("description").toString();
    manifest.script = object.value("script").toString();
    manifest.permissions = stringList(object.value("permissions"));
    QString wrongFormat;
    manifest.formats = formatsOf(object.value("formats"), manifest, &wrongFormat);
    if (error.error != QJsonParseError::NoError) {
        manifest.problem = QString("plugin.json isn't valid JSON: %1 (at character %2).").arg(error.errorString()).arg(error.offset);
        return manifest;
    }
    if (!document.isObject()) {
        manifest.problem = "plugin.json should be a JSON object.";
        return manifest;
    }
    // One written for a newer Omabook is not loaded: what it calls may not be here.
    bool malformed = false;
    const QString need = ScriptApi::needsNewer(object.value("omabook"), &malformed);
    if (!need.isEmpty()) {
        manifest.problem = need;
        manifest.needsNewer = !malformed;
        return manifest;
    }
    if (!manifest.isScript())
        return manifest;
    if (!wrongFormat.isEmpty())
        manifest.problem = wrongFormat;
    for (const QString &permission : std::as_const(manifest.permissions))
        if (!ScriptApi::permissionNames().contains(permission))
            manifest.problem = "plugin.json asks for the permission \"" + permission
                               + "\", which there isn't; there are: " + ScriptApi::permissionNames().join(", ") + ".";
    // The script is a file of the plugin's own folder, not a way out of it.
    const QString script = QDir::cleanPath(manifest.folder + "/" + manifest.script);
    if (!script.startsWith(manifest.folder + "/") || !QFileInfo(script).isFile())
        manifest.problem = "plugin.json names the script \"" + manifest.script + "\", and there is no such file in the plugin's folder.";
    return manifest;
}
