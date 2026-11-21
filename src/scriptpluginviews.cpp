// What of the plugins the window shows (ScriptPlugins, scriptplugins.h): the
// panels and views they put there, what they ask it to show (#77), and
// what they say there briefly (#135).

#include <QFileInfo>
#include <QUrl>

#include "scriptplugin.h"
#include "scriptplugins.h"
#include "scriptwindow.h"

QVariantList ScriptPlugins::panels() const {
    QVariantList list;
    for (const auto &entry : m_plugins) {
        int number = 0;
        for (const ScriptPlugin::Panel &panel : entry.second->panels()) {
            // A panel that doesn't apply isn't there: it takes no room.
            const QString name = number++ == 0 ? entry.first : entry.first + "-" + QString::number(number);
            if (!applies(entry.first, panel.when, panel.visible))
                continue;
            // The file's time in the address makes an edited panel a new component.
            QUrl url = QUrl::fromLocalFile(panel.source);
            url.setQuery(QString::number(QFileInfo(panel.source).lastModified().toMSecsSinceEpoch()));
            list << QVariantMap{
                {"name", name}, {"place", panel.place},  {"title", panel.title}, {"width", panel.width},
                {"url", url},   {"plugin", entry.first}, {"view", panel.view},   {"pluginName", entry.second->manifest().name}};
        }
    }
    return list;
}

void ScriptPlugins::listenToWindow(ScriptPlugin *plugin) {
    connect(plugin, &ScriptPlugin::graphRequested, this, &ScriptPlugins::graphRequested);
    const QString id = plugin->manifest().id;
    connect(plugin, &ScriptPlugin::viewRequested, this, [this, id](const QString &view, const QJsonObject &args, bool toggle) {
        emit viewRequested(id, view, args.toVariantMap(), toggle);
    });
    connect(plugin, &ScriptPlugin::viewClosed, this, [this, id](const QString &view) { emit viewClosed(id, view); });
    connect(plugin, &ScriptPlugin::notified, this,
            [this, id](const QString &text, const QString &action, int token) { emit notified(id, text, action, token); });
}

void ScriptPlugins::act(const QString &plugin, int token) const {
    if (ScriptPlugin *running = this->plugin(plugin))
        running->window().act(token);
}
