// A script plugin switched on or off (ScriptTools::enable, scripttools.h), as
// `omabook plugin enable` and the MCP tool do it.
#include <QFileInfo>

#include "failure.h"
#include "scriptsession.h"
#include "scripttools.h"
#include "settings.h"

QJsonObject ScriptTools::enable(const Context &context, const QString &name, bool enabled) {
    const ScriptSession session(context);
    const ScriptManifest manifest = ScriptManifest::read(session.folder() + "/" + name);
    if (!validName(name) || !QFileInfo(manifest.folder).isDir() || !manifest.isScript())
        return {{"error", "There is no script plugin " + name + " in " + session.folder() + "."}};
    // One that can't load is not said to be on (#289).
    if (const auto why = enabled ? manifest.whyNotLoaded() : std::nullopt)
        return {{"error", Failure(Failure::Code::PluginNotSwitchedOn, {name, *why}).text()}};
    session.settings()->setPluginEnabled("folder:" + name, enabled);
    return {{"name", name}, {"id", "folder:" + name}, {"enabled", enabled}};
}
