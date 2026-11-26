// A tool as the MCP server calls it (Mcp::call, mcp.h).
#include "mcp.h"

#include "settings.h"

Mcp::Result Mcp::call(const Tool &tool, const QJsonObject &arguments) {
    const int unsaved = Settings::unsaved();
    Result result = tool.handler(arguments);
    if (!result.failed() && Settings::unsaved() > unsaved)
        return Result::fail("The change was not saved. " + Settings().problem());
    return result;
}
