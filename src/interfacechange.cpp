#include "interfacechange.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include "themechoice.h"

using namespace Qt::StringLiterals;

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

// Why none of a change can be made; empty when it may be tried.
QString refusal(const Prompts::InterfaceChange &change, const Settings &settings) {
    if (!change.valid)
        return "The answer wasn't a change Omabook can read.";
    return change.settings.isEmpty() ? QString() : settings.problem();
}

} // namespace

Prompts::InterfaceState InterfaceChanges::state(const Customization &custom, const Settings &settings, const QVariantMap &colors,
                                                const Library *library) {
    Prompts::InterfaceState state;
    state.keys = read(custom.folder() + "/keys.json");
    state.style = read(custom.folder() + "/style.json");
    state.settings = settings.options();
    for (auto color = colors.begin(); color != colors.end(); ++color)
        state.colors.insert(color.key(), color.value().value<QColor>().name());
    for (const Customization::Command &command : Customization::commands())
        state.commands += command.id + " (" + command.keys.join(", ") + "): " + command.what + "\n";
    QStringList themes;
    for (const QVariant &theme : Themes::list(ThemePlaces::here(custom.folder())))
        themes << theme.toMap().value("id").toString() + (theme.toMap().value("light").toBool() ? " (light)" : "");
    state.themes = themes.join(", ");
    if (library) {
        for (const Collection &collection : library->collections())
            state.collections += collection.name + ": " + (collection.rule.isEmpty() ? u"(only books put in by hand)"_s : collection.rule) + "\n";
        // The tags met most, so a rule names tags the books really have.
        QHash<QString, int> counts;
        for (const Book &book : library->books())
            for (const QString &tag : book.meta.tags)
                ++counts[tag];
        QStringList tags = counts.keys();
        std::ranges::sort(tags, [&counts](const QString &a, const QString &b) { return counts.value(a) > counts.value(b); });
        QStringList shown;
        for (const QString &tag : tags.mid(0, 80))
            shown << QString("%1 (%2)").arg(tag).arg(counts.value(tag));
        state.tags = shown.join(", ");
    }
    return state;
}

QString InterfaceChanges::apply(Customization &custom, Settings &settings, const Prompts::InterfaceChange &change, Library *library) {
    if (const QString why = refusal(change, settings); !why.isEmpty())
        return why;
    if (!QDir().mkpath(custom.folder()))
        return "The folder " + custom.folder() + " can't be written.";
    const QString keysFile = custom.folder() + "/keys.json", styleFile = custom.folder() + "/style.json";
    // What is there now, to put back. A file that wasn't there is noted as
    // such, so undoing takes it away again.
    QJsonObject before{{"settings", settings.options()}};
    if (QFile::exists(keysFile)) before.insert("keys", read(keysFile));
    if (QFile::exists(styleFile)) before.insert("style", read(styleFile));
    // The collections it touches: each with the rule it had, or none for
    // one that wasn't there.
    QJsonArray touched;
    for (const QJsonValue &value : library ? change.collections : QJsonArray()) {
        const QString name = value.toObject().value("name").toString().simplified();
        const auto there = library->collectionNamed(name);
        touched << QJsonObject{{"name", name}, {"rule", there ? QJsonValue(there->rule) : QJsonValue()}};
    }
    if (!touched.isEmpty())
        before.insert("collections", touched);
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
    for (const QJsonValue &value : library ? change.collections : QJsonArray()) {
        const QString name = value.toObject().value("name").toString().simplified();
        const QString rule = value.toObject().value("rule").toString();
        QString why;
        if (const auto there = library->collectionNamed(name)) {
            library->setCollectionRule(there->id, rule, &why);
        } else if (const auto made = library->addCollection(name, rule); !made) {
            why = made.error();
        }
        if (!why.isEmpty())
            problems << name + ": " + why;
    }
    // The files' own checks have the last word.
    if (const QString refused = custom.check(); !refused.isEmpty())
        problems << refused;
    if (problems.isEmpty())
        return {};
    undo(custom, settings, library);
    return problems.join(' ');
}

bool InterfaceChanges::canUndo(const Customization &custom) { return QFile::exists(undoFile(custom)); }

bool InterfaceChanges::undo(Customization &custom, Settings &settings, Library *library) {
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
    for (const QJsonValue &value : library ? before.value("collections").toArray() : QJsonArray()) {
        const QJsonObject was = value.toObject();
        const auto there = library->collectionNamed(was.value("name").toString());
        if (!there)
            continue;
        if (was.value("rule").isString())
            library->setCollectionRule(there->id, was.value("rule").toString());
        else
            library->removeCollection(there->id);
    }
    QFile::remove(undoFile(custom));
    custom.check();
    return true;
}
