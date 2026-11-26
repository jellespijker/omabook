#include "log.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>
#include <mutex>

Q_LOGGING_CATEGORY(lcApp, "omabook.app", QtInfoMsg)
Q_LOGGING_CATEGORY(lcAi, "omabook.ai", QtInfoMsg)
Q_LOGGING_CATEGORY(lcLibrary, "omabook.library", QtInfoMsg)
Q_LOGGING_CATEGORY(lcGraph, "omabook.graph", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPlugins, "omabook.plugins", QtInfoMsg)
Q_LOGGING_CATEGORY(lcSites, "omabook.sites", QtInfoMsg)
Q_LOGGING_CATEGORY(lcSync, "omabook.sync", QtInfoMsg)
Q_LOGGING_CATEGORY(lcHub, "omabook.hub", QtInfoMsg)

namespace {

constexpr qint64 maximumSize = 2 * 1024 * 1024;
constexpr int keptOld = 2;

struct Sink {
    std::mutex lock;
    QString file;
    QString program;
    bool debug = false;
    QtMessageHandler before = nullptr;
};
Sink &sink() {
    static Sink sink;
    return sink;
}

QString levelName(QtMsgType type) {
    switch (type) {
    case QtDebugMsg: return "debug";
    case QtInfoMsg: return "info";
    case QtWarningMsg: return "warning";
    case QtCriticalMsg: return "critical";
    case QtFatalMsg: return "fatal";
    }
    return "info";
}

int rank(const QString &level) {
    static const QStringList order{"debug", "info", "warning", "critical", "fatal"};
    const qsizetype at = order.indexOf(level);
    return at < 0 ? 0 : int(at);
}

// "omabook.ai" is "ai"; Qt's own and the QML engine's keep a short name.
QString partOf(const char *category) {
    const QString name = QString::fromLatin1(category ? category : "default");
    if (name.startsWith("omabook."))
        return name.mid(8);
    if (name.startsWith("qt.qml") || name == "qml" || name == "js")
        return "qml";
    return name == "default" ? QString("app") : name;
}

// The oldest goes, the others move up a place, and a new file is begun.
void rotate(const QString &file) {
    QFile::remove(file + "." + QString::number(keptOld));
    for (int n = keptOld - 1; n >= 1; --n)
        QFile::rename(file + "." + QString::number(n), file + "." + QString::number(n + 1));
    QFile::rename(file, file + ".1");
}

void append(Sink &to, QtMsgType type, const char *category, const QString &message) {
    // One line each: what is written can't break the shape of the file.
    QString text = message;
    text.replace('\n', " | ").replace('\r', ' ');
    const QString line = QString("%1 %2 %3 %4 %5\n")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), levelName(type), to.program,
                                  partOf(category), text);
    const std::scoped_lock locked(to.lock);
    if (QFileInfo(to.file).size() > maximumSize)
        rotate(to.file);
    QFile file(to.file);
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write(line.toUtf8());
}

void handle(QtMsgType type, const QMessageLogContext &context, const QString &message) {
    Sink &to = sink();
    if (type != QtDebugMsg || to.debug)
        append(to, type, context.category, message);
    if (to.before)
        to.before(type, context, message);
}

const QRegularExpression &lineShape() {
    static const QRegularExpression shape(R"(^(\S+) (\S+) (\S+) (\S+) (.*)$)");
    return shape;
}

} // namespace

QString Log::path(const QString &folder) {
    const QString state = !folder.isEmpty() ? folder
                          : qEnvironmentVariableIsSet("XDG_STATE_HOME") ? qEnvironmentVariable("XDG_STATE_HOME") + "/omabook"
                                                                        : QDir::homePath() + "/.local/state/omabook";
    return state + "/omabook.log";
}

void Log::install(const QString &program, const QString &folder) {
    Sink &to = sink();
    {
        const std::scoped_lock locked(to.lock);
        to.file = path(folder);
        to.program = program;
        to.debug = qEnvironmentVariable("OMABOOK_DEBUG") == "1";
        QDir().mkpath(QFileInfo(to.file).absolutePath());
    }
    // The handler that was there before ours is called after it. One put
    // in its place since (the MCP server's, which keeps standard output for
    // the protocol) is kept the same way; ours never calls itself.
    if (const QtMessageHandler current = qInstallMessageHandler(handle); current != handle)
        to.before = current;
    if (to.debug)
        QLoggingCategory::setFilterRules("omabook.*.debug=true");
}

void Log::keep(const QLoggingCategory &category, const QString &message) {
    if (Sink &to = sink(); !to.file.isEmpty())
        append(to, QtWarningMsg, category.categoryName(), message);
}

QJsonObject Log::Entry::toJson() const {
    return {{"when", when.toString(Qt::ISODateWithMs)}, {"level", level}, {"program", program},
            {"category", category}, {"message", message}};
}

QList<Log::Entry> Log::read(const Filter &filter, const QString &folder) {
    const QString file = path(folder);
    QList<Entry> entries;
    for (int n = keptOld; n >= 0; --n) {
        QFile part(n == 0 ? file : file + "." + QString::number(n));
        if (!part.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream lines(&part);
        while (!lines.atEnd()) {
            const QRegularExpressionMatch match = lineShape().match(lines.readLine());
            if (!match.hasMatch())
                continue;
            Entry entry{QDateTime::fromString(match.captured(1), Qt::ISODateWithMs), match.captured(2), match.captured(3),
                        match.captured(4), match.captured(5)};
            if (rank(entry.level) < rank(filter.least))
                continue;
            if (!filter.category.isEmpty() && entry.category != filter.category)
                continue;
            if (filter.since && entry.when < *filter.since)
                continue;
            if (!filter.words.isEmpty() && !entry.message.contains(filter.words, Qt::CaseInsensitive))
                continue;
            entries << entry;
        }
    }
    if (filter.lines > 0 && entries.size() > filter.lines)
        entries = entries.mid(entries.size() - filter.lines);
    return entries;
}
