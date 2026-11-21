// What the plugins are told of their data in the change log (ScriptPlugins,
// scriptplugins.h, #126): what came from elsewhere, each plugin of its own.

#include "scriptplugin.h"
#include "scriptplugins.h"

void ScriptPlugins::lookAtTheLog() { m_logFeed.look(m_services.library ? m_services.library() : nullptr); }

// Only to a plugin that runs: one that loads later reads its data then, as it is.
void ScriptPlugins::tellLog(const QString &plugin, const QJsonObject &entities) {
    ScriptPlugin *running = this->plugin(plugin);
    if (!running)
        return;
    running->emitEvent("log.changed", {{"entities", entities}});
    emit logChanged(plugin, entities.toVariantMap());
}
