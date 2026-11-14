#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QLoggingCategory>
#include <QString>
#include <QStringList>
#include <optional>

// What Omabook did and what went wrong, kept in one file a person or an
// agent can read afterwards: `omabook logs`, the MCP tool `logs_read`.
//
// Everything Omabook says through Qt's logging (qCInfo(lcAi) << …) lands
// there, the QML engine's warnings included, with the time, the level, the
// part of Omabook and which program said it (the window, a command, the MCP
// server). It never holds what was read or asked: no book text, no prompt,
// no answer, no token; only what happened, how big and how long.
//
// The file is `omabook.log` in the state folder (~/.local/state/omabook),
// cut at 2 MB with the two before kept as .1 and .2. OMABOOK_DEBUG=1 keeps
// the debug lines as well.

Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcAi)
Q_DECLARE_LOGGING_CATEGORY(lcLibrary)
Q_DECLARE_LOGGING_CATEGORY(lcGraph)
Q_DECLARE_LOGGING_CATEGORY(lcPlugins)
Q_DECLARE_LOGGING_CATEGORY(lcSites)
Q_DECLARE_LOGGING_CATEGORY(lcSync)

namespace Log {

// Starts writing to the log file, as `program` ("window", "cli", "mcp").
// Messages still go where they went before as well (the terminal, or the
// MCP server's standard error). `folder` is the state folder when empty.
void install(const QString &program, const QString &folder = {});
// The file the log is written to.
QString path(const QString &folder = {});

struct Entry {
    QDateTime when;
    QString level;      // debug, info, warning, critical
    QString program;    // window, cli, mcp
    QString category;   // the part of Omabook: ai, library, graph, plugins, sites, sync, app, qml
    QString message;
    QJsonObject toJson() const;
};

struct Filter {
    int lines = 100;                  // the newest this many, after the rest
    QString least = "debug";          // the lowest level shown
    QString category;                 // only this part, when not empty
    std::optional<QDateTime> since;   // only after this
    QString words;                    // only lines that hold these
};
// The log as entries, oldest first, the rotated files included.
QList<Entry> read(const Filter &filter, const QString &folder = {});

} // namespace Log
