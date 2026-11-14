#include "interfacechange.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {

QString read(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

bool write(const QString &path, const QByteArray &contents) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

QString undoFile(const Customization &custom) { return custom.folder() + "/.before-last-change.json"; }

} // namespace

Prompts::InterfaceState InterfaceChanges::state(const Customization &custom, const Settings &settings, const QVariantMap &colors) {
    Prompts::InterfaceState state;
    state.keys = read(custom.folder() + "/keys.json");
    state.style = read(custom.folder() + "/style.json");
    state.settings = settings.options();
    for (auto color = colors.begin(); color != colors.end(); ++color)
        state.colors.insert(color.key(), color.value().value<QColor>().name());
    for (const Customization::Command &command : Customization::commands())
        state.commands += command.id + " (" + command.keys.join(", ") + "): " + command.what + "\n";
    return state;
}

QString InterfaceChanges::apply(Customization &custom, Settings &settings, const Prompts::InterfaceChange &change) {
    if (!change.valid)
        return "The answer wasn't a change Omabook can read.";
    if (!QDir().mkpath(custom.folder()))
        return "The folder " + custom.folder() + " can't be written.";
    const QString keysFile = custom.folder() + "/keys.json", styleFile = custom.folder() + "/style.json";
    // What is there now, to put back. A file that wasn't there is noted as
    // such, so undoing takes it away again.
    QJsonObject before{{"settings", settings.options()}};
    if (QFile::exists(keysFile)) before.insert("keys", read(keysFile));
    if (QFile::exists(styleFile)) before.insert("style", read(styleFile));
    if (!write(undoFile(custom), QJsonDocument(before).toJson()))
        return "The folder " + custom.folder() + " can't be written.";

    QStringList problems;
    if (change.hasKeys && !write(keysFile, QJsonDocument(change.keys).toJson()))
        problems << "keys.json can't be written.";
    if (change.hasStyle && !write(styleFile, QJsonDocument(change.style).toJson()))
        problems << "style.json can't be written.";
    for (auto option = change.settings.begin(); option != change.settings.end(); ++option)
        if (!settings.setOption(option.key(), option.value()))
            problems << "\"" + option.key() + "\" isn't a setting that can be changed to that.";
    // The files' own checks have the last word.
    if (const QString refused = custom.check(); !refused.isEmpty())
        problems << refused;
    if (problems.isEmpty())
        return {};
    undo(custom, settings);
    return problems.join(' ');
}

bool InterfaceChanges::canUndo(const Customization &custom) { return QFile::exists(undoFile(custom)); }

bool InterfaceChanges::undo(Customization &custom, Settings &settings) {
    QFile file(undoFile(custom));
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject before = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    for (const QString &name : {QString("keys"), QString("style")}) {
        const QString path = custom.folder() + "/" + name + ".json";
        if (before.contains(name))
            write(path, before.value(name).toString().toUtf8());
        else
            QFile::remove(path);
    }
    const QJsonObject options = before.value("settings").toObject();
    for (auto option = options.begin(); option != options.end(); ++option)
        settings.setOption(option.key(), option.value());
    QFile::remove(undoFile(custom));
    custom.check();
    return true;
}
