// A tool as the MCP server calls it (Mcp::call, mcp.h), and one that is one of
// Omabook's commands, run as one (Mcp::command).
#include "mcp.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QProcess>

#include "library.h"
#include "settings.h"

using namespace Qt::StringLiterals;

Mcp::Result Mcp::call(const Tool &tool, const QJsonObject &arguments) {
    const int unsaved = Settings::unsaved();
    Result result = tool.handler(arguments);
    if (!result.failed() && Settings::unsaved() > unsaved)
        return Result::fail(Settings::unsavedNote() + ' ' + Settings().problem());
    return result;
}

Mcp::Result Mcp::command(const QStringList &arguments, bool json) {
    QString error;
    const auto library = Mcp::library(&error);
    if (!library)
        return Result::fail(error);
    QProcess process;
    QStringList asked = arguments;
    asked << "--library" << library->root();
    if (json)
        asked << "--json";
    process.start(QCoreApplication::applicationFilePath(), asked);
    // A sync or an import of a large library takes minutes, not seconds.
    if (!process.waitForStarted() || !process.waitForFinished(15 * 60 * 1000)) {
        process.kill();
        process.waitForFinished(2000);
        return Result::fail("omabook " + arguments.value(0) + " didn't finish.");
    }
    const QByteArray output = process.readAllStandardOutput();
    const QString said = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || (process.exitCode() != 0 && output.trimmed().isEmpty()))
        return Result::fail(said.isEmpty() ? "omabook " + arguments.value(0) + " failed." : said);
    // A command that couldn't save a setting fails, whatever else it printed (#275).
    if (process.exitCode() != 0 && said.contains(u"omabook: "_s + Settings::unsavedNote()))
        return Result::fail(Settings::unsavedNote() + ' ' + Settings().problem());
    if (!json)
        return Result::ok({{"output", QString::fromUtf8(output)}});
    const QJsonDocument document = QJsonDocument::fromJson(output);
    QJsonObject data = document.isObject() ? document.object() : QJsonObject{{"result", document.array()}};
    // What the command complained of while still answering is passed on.
    if (!said.isEmpty())
        data.insert("notes", said);
    return Result::ok(data);
}
