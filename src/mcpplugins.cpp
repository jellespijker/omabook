// The MCP tools over the registry of plugins (plugins.h): what there is, and
// switching one on or off.
#include <QJsonArray>

#include "mcp.h"
#include "plugins.h"
#include "settings.h"

namespace {

using namespace Mcp;

QJsonObject pluginJson(const PluginInfo &plugin) {
    return {{"id", plugin.id},
            {"name", plugin.name},
            {"kind", plugin.kind},
            {"origin", plugin.origin},
            {"version", plugin.version},
            {"description", plugin.description},
            {"fields", QJsonArray::fromStringList(plugin.fields)},
            {"enabled", plugin.enabled},
            {"switchable", plugin.switchable},
            {"problem", plugin.problem}};
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): the tool registry is filled at start-up (mcp.h), as mcpcommands.cpp fills it.
const bool registered = [] {
    registerTool({"plugins_list", "List plugins",
                  "What Omabook is made of and what has been added to it: each plugin's id, kind (metadata, format, "
                  "conversion, ...), origin (built-in, calibre, folder), the fields a metadata source brings, and "
                  "whether it is on. Only switchable ones can be turned on or off.",
                  schema({{"kind", string("Only plugins of this kind, such as metadata.")}}), ReadOnly, [](const QJsonObject &arguments) {
                      Settings settings;
                      const Plugins registry(&settings);
                      QJsonArray plugins;
                      for (const PluginInfo &plugin : registry.list())
                          if (!arguments.contains("kind") || plugin.kind == arguments.value("kind").toString())
                              plugins << pluginJson(plugin);
                      return Result::ok({{"folder", registry.folder()}, {"plugins", plugins}});
                  }});

    registerTool({"plugin_set_enabled", "Turn a plugin on or off",
                  "Switches a plugin on or off, by its id from plugins_list. Only those marked switchable can be, and a "
                  "script plugin that can't load isn't switched on.",
                  schema({{"id", string("The plugin's id, such as builtin:Crossref, or its name.")},
                          {"enabled", boolean("True to turn it on, false to turn it off.")}},
                         {"id", "enabled"}),
                  Changes, [](const QJsonObject &arguments) {
                      Settings settings;
                      Plugins registry(&settings);
                      QStringList could;
                      const QString given = arguments.value("id").toString();
                      const QString id = registry.idFor(given, &could);
                      if (id.isEmpty())
                          return Result::fail("There is no plugin " + given + " that can be turned on or off, or more than one"
                                              + (could.isEmpty() ? QString() : " (could be: " + could.join(", ") + ")")
                                              + "; see plugins_list.");
                      if (const auto switched = registry.switchIfLoads(id, arguments.value("enabled").toBool()); !switched)
                          return Result::fail(switched.error().text());
                      for (const PluginInfo &plugin : registry.list())
                          if (plugin.id == id)
                              return Result::ok(pluginJson(plugin));
                      return Result::ok({{"id", id}, {"enabled", arguments.value("enabled").toBool()}});
                  }});
    return true;
}();

} // namespace
