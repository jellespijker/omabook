#include "plugintools.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <archive.h>
#include <archive_entry.h>
#include <climits>
#include <memory>

#include "calibreplugin.h"
#include "mcp.h"
#include "plugins.h"
#include "scriptapi.h"
#include "scriptplugins.h"
#include "settings.h"

namespace {

QJsonObject problem(const QString &why) { return {{"error", why}}; }

// The plugin folder and the settings of a call: the caller's when it brought
// them, the user's otherwise.
struct Where {
    QString folder;
    Settings *settings = nullptr;
    std::unique_ptr<Settings> own;

    explicit Where(const PluginTools::Context &context) : folder(context.folder), settings(context.settings) {
        if (!settings) {
            own = std::make_unique<Settings>();
            settings = own.get();
        }
        if (folder.isEmpty())
            folder = ScriptPlugins::defaultFolder();
    }
};

const char *const calibreOff = "The Calibre plugin is switched off.";

// --- Unpacking ---

// A name inside a package as a path under the plugin's folder, or false for
// one that would land anywhere else: an absolute path, a step up, a
// backslash (which another system reads as a separator).
bool landsInside(const QString &name, QString *clean) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(QChar(0)))
        return false;
    QStringList parts;
    for (const QString &part : name.split('/', Qt::SkipEmptyParts)) {
        if (part == "..")
            return false;
        if (part != ".")
            parts << part;
    }
    *clean = parts.join('/');
    return true;
}

// What was unpacked so far, held to the limits.
struct Tally {
    PluginTools::Limits limits;
    int files = 0;
    qint64 bytes = 0;

    bool another(QString *error) {
        if (++files <= limits.files)
            return true;
        *error = QString("It holds more than %1 files, which is more than a plugin is allowed.").arg(limits.files);
        return false;
    }
    // Whether `count` more bytes of one file, `soFar` of which were taken, fit.
    bool fits(const QString &name, qint64 soFar, qint64 count, QString *error) {
        if (soFar + count > limits.fileBytes) {
            *error = QString("\"%1\" is larger than %2 MB, which is more than a plugin's file is allowed.")
                         .arg(name).arg(limits.fileBytes / (1024 * 1024));
            return false;
        }
        if (bytes + count > limits.totalBytes) {
            *error = QString("It unpacks to more than %1 MB, which is more than a plugin is allowed.")
                         .arg(limits.totalBytes / (1024 * 1024));
            return false;
        }
        bytes += count;
        return true;
    }
};

struct Closer {
    void operator()(archive *a) const { archive_read_free(a); }
};

QString entryName(archive_entry *entry) {
    if (const char *utf8 = archive_entry_pathname_utf8(entry))
        return QString::fromUtf8(utf8);
    if (const wchar_t *wide = archive_entry_pathname_w(entry))
        return QString::fromWCharArray(wide);
    return QString::fromLocal8Bit(archive_entry_pathname(entry));
}

// Unpacks an archive into `into`, in one pass, refusing the whole of it for
// one entry that isn't a plain file inside the folder. The sizes an archive
// claims are not believed: the bytes are counted as they come out.
bool unpack(const QString &path, const QString &into, Tally *tally, QString *error) {
    std::unique_ptr<archive, Closer> reader(archive_read_new());
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), QFile::encodeName(path).constData(), 1 << 16) != ARCHIVE_OK) {
        *error = path + " isn't an archive that can be read.";
        return false;
    }
    archive_entry *entry = nullptr;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        const QString name = entryName(entry);
        const auto type = archive_entry_filetype(entry);
        QString clean;
        if (!landsInside(name, &clean)) {
            *error = "\"" + name + "\" would land outside the plugin's folder, so nothing of this archive was installed.";
            return false;
        }
        if (archive_entry_hardlink(entry) || (type != AE_IFREG && type != AE_IFDIR)) {
            *error = "\"" + name + "\" is a link or a special file, which a plugin has no use for, so nothing of this "
                     "archive was installed.";
            return false;
        }
        if (type == AE_IFDIR || clean.isEmpty())
            continue;
        if (!tally->another(error))
            return false;
        const QString target = into + "/" + clean;
        QFile file(target);
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            *error = "\"" + name + "\" can't be unpacked.";
            return false;
        }
        char buffer[1 << 16];
        qint64 size = 0;
        for (;;) {
            const la_ssize_t count = archive_read_data(reader.get(), buffer, sizeof buffer);
            if (count == 0)
                break;
            if (count < 0) {
                *error = "\"" + name + "\" can't be read from the archive.";
                return false;
            }
            if (!tally->fits(name, size, count, error))
                return false;
            if (file.write(buffer, count) != count) {
                *error = "\"" + name + "\" can't be unpacked.";
                return false;
            }
            size += count;
        }
        file.close();
        // A program keeps its right to be run, and nothing more than that.
        const bool runs = archive_entry_perm(entry) & 0100;
        file.setPermissions(runs ? QFile::Permissions(0x7755) : QFile::Permissions(0x6644));
    }
    if (status != ARCHIVE_EOF) {
        *error = path + " isn't an archive that can be read.";
        return false;
    }
    return true;
}

// Copies a plugin's folder into `into`, by the same rules. What a
// repository keeps of itself stays behind.
bool copyFolder(const QString &from, const QString &into, Tally *tally, QString *error) {
    const QDir source(from);
    QDirIterator walk(from, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QFileInfo entry(walk.next());
        const QString name = source.relativeFilePath(entry.filePath());
        if (name == ".git" || name.startsWith(".git/"))
            continue;
        if (entry.isSymLink() || (!entry.isFile() && !entry.isDir())) {
            *error = "\"" + name + "\" is a link or a special file, which a plugin has no use for.";
            return false;
        }
        if (entry.isDir()) {
            QDir().mkpath(into + "/" + name);
            continue;
        }
        if (!tally->another(error) || !tally->fits(name, 0, entry.size(), error))
            return false;
        const QString target = into + "/" + name;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(entry.filePath(), target)) {
            *error = "\"" + name + "\" can't be copied.";
            return false;
        }
        QFile::setPermissions(target, entry.isExecutable() ? QFile::Permissions(0x7755) : QFile::Permissions(0x6644));
    }
    return true;
}

// A package laid out in a folder of its own, where it can be looked at
// before any of it is where a running Omabook would find it.
struct Staged {
    std::unique_ptr<QTemporaryDir> dir;
    QString root;      // where its plugin.json is
    QString suggested; // the name the package gives itself
    Tally tally;
    QString error;

    Staged(const QString &source, const QString &under, const PluginTools::Limits &limits) {
        tally.limits = limits;
        const QFileInfo given(source);
        if (source.isEmpty() || !given.exists()) {
            error = "There is no " + (source.isEmpty() ? QString("plugin named") : source) + ".";
            return;
        }
        QDir().mkpath(under);
        dir = std::make_unique<QTemporaryDir>(under + "/.installing-XXXXXX");
        if (!dir->isValid()) {
            error = "The plugin can't be unpacked in " + under + ".";
            return;
        }
        const bool unpacked = given.isDir() ? copyFolder(given.absoluteFilePath(), dir->path(), &tally, &error)
                                            : unpack(given.absoluteFilePath(), dir->path(), &tally, &error);
        if (!unpacked)
            return;
        // Archives are often one folder with the plugin inside it.
        const QDir top(dir->path());
        const QStringList folders = top.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
        const QStringList files = top.entryList(QDir::Files | QDir::Hidden);
        if (files.contains("plugin.json")) {
            root = dir->path();
            suggested = given.isDir() ? given.fileName() : given.completeBaseName();
        } else if (folders.size() == 1 && files.isEmpty() && QFileInfo(top.filePath(folders.first() + "/plugin.json")).isFile()) {
            root = top.filePath(folders.first());
            suggested = folders.first();
        } else {
            error = "There is no plugin.json in " + source + ", so it isn't a plugin.";
        }
    }
};

// What a staged package says of itself, or why it can't be a plugin.
QJsonObject describePackage(const Staged &staged) {
    QFile manifest(staged.root + "/plugin.json");
    QJsonParseError parse;
    const QJsonDocument document = manifest.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(manifest.readAll(), &parse)
                                                                      : QJsonDocument();
    if (!document.isObject())
        return problem("Its plugin.json can't be read: "
                       + (parse.error == QJsonParseError::NoError ? QString("it should be a JSON object") : parse.errorString()) + ".");
    const QJsonObject object = document.object();
    const QString script = object.value("script").toString();
    QStringList command, permissions;
    for (const QJsonValue &value : object.value("command").toArray())
        command << value.toString();
    for (const QJsonValue &value : object.value("permissions").toArray())
        permissions << value.toString();

    QString kind;
    if (!script.isEmpty()) {
        kind = "script";
        QString clean;
        if (!landsInside(script, &clean) || !QFileInfo(staged.root + "/" + clean).isFile())
            return problem("Its plugin.json names the script \"" + script + "\", which isn't in it.");
        for (const QString &permission : std::as_const(permissions))
            if (!ScriptApi::permissionNames().contains(permission))
                return problem("It asks for the permission \"" + permission + "\", which this Omabook doesn't know; there are: "
                               + ScriptApi::permissionNames().join(", ") + ".");
    } else if (!command.isEmpty() && !command.first().isEmpty()) {
        kind = "program";
        const QString type = object.value("type").toString("metadata");
        if (type != "metadata")
            return problem("This Omabook doesn't know plugins of type \"" + type + "\".");
        // A program it brings itself has to be there; one it expects on the
        // computer is the manager's to report when it is missing.
        const QString program = command.first();
        QString clean;
        if (program.startsWith("./") && (!landsInside(program, &clean) || !QFileInfo(staged.root + "/" + clean).isFile()))
            return problem("Its plugin.json runs \"" + program + "\", which isn't in it.");
        // A program is not held to permissions: it can do what the user can.
        permissions = QStringList{"program"};
    } else {
        return problem("Its plugin.json names neither a \"script\" nor a \"command\", so there is nothing in it to run.");
    }
    return {{"title", object.value("name").toString()},
            {"version", object.value("version").toString()},
            {"author", object.value("author").toString()},
            {"description", object.value("description").toString()},
            {"kind", kind},
            {"permissions", QJsonArray::fromStringList(permissions)},
            {"files", staged.tally.files},
            {"bytes", staged.tally.bytes}};
}

// The answer of inspect() for a staged package: what it is and what it
// would be called.
QJsonObject inspected(const Staged &staged, const QString &source, const QString &folder, const PluginTools::InstallOptions &options) {
    if (!staged.error.isEmpty())
        return problem(staged.error);
    QJsonObject answer = describePackage(staged);
    if (ScriptTools::failed(answer))
        return answer;
    const QString name = options.name.isEmpty() ? PluginTools::folderName(staged.suggested) : options.name;
    if (!ScriptTools::validName(name))
        return problem(options.name.isEmpty()
                           ? "No name for the plugin can be made of \"" + staged.suggested + "\"; give it one."
                           : "\"" + name + "\" can't be a plugin's name: lower-case letters, digits, - and _, such as \"reading-log\".");
    if (answer.value("title").toString().isEmpty())
        answer.insert("title", name);
    answer.insert("name", name);
    answer.insert("source", QFileInfo(source).absoluteFilePath());
    answer.insert("installed", QFileInfo::exists(folder + "/" + name));
    return answer;
}

// To the trash, where a mistake can be undone; a filesystem with no trash
// gets it deleted, since the user asked for it to go.
bool discard(const QString &path, bool *trashed) {
    *trashed = QFile::moveToTrash(path);
    return *trashed || QDir(path).removeRecursively();
}

bool copyTree(const QString &from, const QString &into) {
    Tally unlimited;
    unlimited.limits = {INT_MAX, Q_INT64_C(1) << 60, Q_INT64_C(1) << 60};
    QString error;
    return copyFolder(from, into, &unlimited, &error);
}

} // namespace

QStringList PluginTools::groups() { return {"metadata", "reading", "script", "program", "calibre", "format", "catalog", "ai"}; }

QString PluginTools::group(const PluginInfo &plugin) {
    if (plugin.kind == "script")
        return "script";
    // A program of the user's that answers look-ups stands among the
    // sources; one that can't be used as it is stands apart.
    if (plugin.origin == "folder")
        return plugin.kind == "metadata" && plugin.problem.isEmpty() ? "metadata" : "program";
    if (plugin.id == CalibrePlugin::id || plugin.kind == "calibre")
        return "calibre";
    if (plugin.kind == "conversion" || plugin.kind == "import")
        return "format";
    return groups().contains(plugin.kind) ? plugin.kind : "format";
}

QString PluginTools::missingProgram(const PluginInfo &plugin) {
    if (plugin.origin != "folder" || !plugin.script.isEmpty() || plugin.command.isEmpty() || !plugin.problem.isEmpty())
        return {};
    const QString program = plugin.command.first();
    if (!program.contains('/'))
        return QStandardPaths::findExecutable(program).isEmpty() ? program : QString();
    const QFileInfo file(QDir(plugin.folder).absoluteFilePath(program));
    return file.isFile() && file.isExecutable() ? QString() : program;
}

QString PluginTools::state(const PluginInfo &plugin) {
    if (plugin.kind == "script")
        return !plugin.enabled ? "off" : plugin.problem.isEmpty() ? "on" : "broken";
    if (plugin.origin == "folder") {
        if (!plugin.problem.isEmpty())
            return "broken";
        if (!plugin.enabled)
            return "off";
        return missingProgram(plugin).isEmpty() ? "on" : "needs";
    }
    if (!plugin.enabled || plugin.problem == calibreOff)
        return "off";
    return plugin.problem.isEmpty() ? "on" : "needs";
}

QJsonObject PluginTools::lastError(const QString &pluginFolder, const QString &name) {
    QFile file(pluginFolder + "/" + name + ".error");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QString text = QString::fromUtf8(file.readAll()).trimmed();
    if (text.isEmpty())
        return {};
    // ScriptPlugins adds a line when the version before still runs.
    static const QString keptLine = "Omabook is using the last version that worked.";
    const bool kept = text.endsWith(keptLine);
    QJsonObject error = ScriptTools::located(kept ? text.chopped(keptLine.size()).trimmed() : text);
    error.insert("kept", kept);
    return error;
}

bool PluginTools::dismissError(const QString &pluginFolder, const QString &name) {
    return ScriptTools::validName(name) && QFile::remove(pluginFolder + "/" + name + ".error");
}

QJsonArray PluginTools::overview(const QList<PluginInfo> &plugins, const ScriptPlugins *scripts) {
    int sources = 0;
    for (const PluginInfo &plugin : plugins)
        sources += plugin.kind == "metadata";

    QList<QPair<qsizetype, QJsonObject>> rows;
    int position = 0;
    for (const PluginInfo &plugin : plugins) {
        const QString inGroup = group(plugin), is = state(plugin);
        QJsonObject row{{"id", plugin.id},
                        {"name", plugin.name},
                        {"kind", plugin.kind},
                        {"origin", plugin.origin},
                        {"version", plugin.version},
                        {"author", plugin.author},
                        {"description", plugin.description},
                        {"fields", QJsonArray::fromStringList(plugin.fields)},
                        {"enabled", plugin.enabled},
                        {"switchable", plugin.switchable},
                        {"problem", plugin.problem},
                        {"permissions", QJsonArray::fromStringList(plugin.permissions)},
                        {"folder", plugin.folder},
                        {"group", inGroup},
                        {"state", is},
                        {"removable", plugin.origin == "folder"},
                        {"tryable", plugin.kind == "metadata" && is == "on"}};
        const QString missing = missingProgram(plugin);
        if (!missing.isEmpty())
            row.insert("note", "Its program, " + missing + ", isn't on this computer.");
        else if (is != "on" && !plugin.problem.isEmpty())
            row.insert("note", plugin.problem);
        if (inGroup == "calibre" && plugin.id != CalibrePlugin::id)
            row.insert("beneath", CalibrePlugin::id);
        if (plugin.kind == "metadata") {
            row.insert("position", ++position);
            row.insert("of", sources);
        }
        if (plugin.kind == "script") {
            const QString name = plugin.id.section(':', 1);
            QJsonArray adds;
            const ScriptPlugin *running = scripts ? scripts->plugin(name) : nullptr;
            if (running) {
                for (const ScriptPlugin::Source &source : running->sources())
                    adds << QJsonObject{{"what", "source"}, {"name", source.name}, {"fields", QJsonArray::fromStringList(source.fields)}};
                for (const ScriptPlugin::Catalog &catalog : running->catalogs())
                    adds << QJsonObject{{"what", "catalog"}, {"name", catalog.format}, {"detail", catalog.description}};
                for (const ScriptPlugin::Format &format : running->formats())
                    adds << QJsonObject{{"what", "format"}, {"name", format.name}, {"detail", format.extensions.join(", ")}};
                for (const ScriptPlugin::Importer &importer : running->importers())
                    adds << QJsonObject{{"what", "importer"}, {"name", importer.name}};
                for (const ScriptPlugin::Command &command : running->commands())
                    adds << QJsonObject{{"what", "command"}, {"name", command.title}, {"key", command.key}, {"place", command.place}};
                for (const ScriptPlugin::Panel &panel : running->panels())
                    adds << QJsonObject{{"what", "panel"}, {"name", panel.title}, {"place", panel.place}};
                for (const QString &event : running->events())
                    adds << QJsonObject{{"what", "hook"}, {"name", event}};
                row.insert("tryable", is != "off" && !running->sources().isEmpty());
            }
            row.insert("running", running != nullptr);
            row.insert("adds", adds);
            const QJsonObject error = lastError(QFileInfo(plugin.folder).absolutePath(), name);
            if (!error.isEmpty())
                row.insert("error", error);
        }
        rows.append({groups().indexOf(inGroup), row});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QJsonArray sorted;
    for (const auto &row : std::as_const(rows))
        sorted << row.second;
    return sorted;
}

QJsonArray PluginTools::health(const QJsonArray &overview, const CalibrePlugin &calibre) {
    QJsonArray issues;
    for (const QJsonValue &value : overview) {
        const QJsonObject plugin = value.toObject();
        const QString is = plugin.value("state").toString();
        if (is == "broken")
            issues << QJsonObject{{"id", plugin.value("id")}, {"name", plugin.value("name")}, {"issue", "broken"}};
        else if (is == "needs" && plugin.value("origin").toString() == "folder")
            issues << QJsonObject{{"id", plugin.value("id")}, {"name", plugin.value("name")}, {"issue", "missing"}};
    }
    // Switched off, nothing of Calibre's is read; said only when that
    // leaves a library of Calibre's lying unused.
    if (calibre.switchedOff() && calibre.hasLibraries())
        issues << QJsonObject{{"id", CalibrePlugin::id}, {"name", "Calibre"}, {"issue", "calibre-off"}};
    return issues;
}

QString PluginTools::folderName(const QString &text) {
    static const QRegularExpression other("[^a-z0-9_-]+");
    static const QRegularExpression edges("^[-_]+|-+$");
    // Accents are dropped rather than turned into dashes: "Café" is "cafe".
    QString name;
    for (const QChar c : text.toLower().normalized(QString::NormalizationForm_KD))
        if (c.category() != QChar::Mark_NonSpacing)
            name += c;
    name.replace(other, "-").remove(edges);
    name.truncate(64);
    return ScriptTools::validName(name) ? name : QString();
}

QJsonObject PluginTools::inspect(const Context &context, const QString &source, const InstallOptions &options) {
    const Where where(context);
    // Nothing of it goes near the plugin folder until it is installed.
    const Staged staged(source, QDir::tempPath(), options.limits);
    return inspected(staged, source, where.folder, options);
}

QJsonObject PluginTools::install(const Context &context, const QString &source, const InstallOptions &options) {
    const Where where(context);
    // Unpacked beside where it will live, so that putting it in place is
    // one rename; a folder whose name starts with a dot is no plugin.
    const Staged staged(source, where.folder, options.limits);
    QJsonObject answer = inspected(staged, source, where.folder, options);
    if (ScriptTools::failed(answer))
        return answer;
    const QString name = answer.value("name").toString();
    const QString target = where.folder + "/" + name;
    const bool replaces = QFileInfo::exists(target);
    if (replaces && !options.replace)
        return problem("There already is a plugin " + name + ". To put this one in its place (the one there goes to the "
                       "trash), say so: omabook plugin install --replace, or give it another --name.");

    // Decided before it is in place: a running Omabook loads what appears
    // in the folder, and one that wasn't approved must not run even once.
    where.settings->setPluginEnabled("folder:" + name, options.enable);
    if (replaces) {
        // What the plugin kept of its own (its data folder) stays with it.
        if (QFileInfo(target + "/data").isDir() && !QFileInfo::exists(staged.root + "/data"))
            copyTree(target + "/data", staged.root + "/data");
        bool trashed = false;
        if (!discard(target, &trashed))
            return problem("The plugin that is there, " + target + ", can't be moved out of the way.");
    }
    QFile::remove(target + ".error");
    if (!QDir().rename(staged.root, target))
        return problem("The plugin can't be put in " + target + ".");

    answer.insert("id", "folder:" + name);
    answer.insert("folder", target);
    answer.insert("enabled", options.enable);
    answer.insert("replaced", replaces);
    answer.insert("installed", true);
    if (!options.enable)
        answer.insert("next", "It is off until you switch it on: omabook plugins enable folder:" + name);
    return answer;
}

QJsonObject PluginTools::remove(const Context &context, const QString &name) {
    const Where where(context);
    const QString target = where.folder + "/" + name;
    const QFileInfo folder(target);
    if (!ScriptTools::validName(name) || (!folder.isDir() && !folder.isSymLink()))
        return problem("There is no plugin " + name + " in " + where.folder + ". Only your own plugins can be removed; the "
                       "ones built in and Calibre's can be switched off: omabook plugins disable <id>.");
    bool trashed = false;
    // A link is taken away, and what it points at left alone.
    const bool gone = folder.isSymLink() ? QFile::remove(target) : discard(target, &trashed);
    if (!gone)
        return problem("The plugin can't be removed: " + target);
    QFile::remove(target + ".error");
    return {{"name", name}, {"id", "folder:" + name}, {"folder", target}, {"trashed", trashed}};
}

// The same, as tools for an agent talking to `omabook mcp`.
namespace {

Mcp::Result answered(const QJsonObject &answer) {
    return ScriptTools::failed(answer) ? Mcp::Result::fail(answer.value("error").toString()) : Mcp::Result::ok(answer);
}

const bool toolsRegistered = [] {
    bool ok = Mcp::registerTool({
        "plugin_install", "Install a plugin",
        "Installs a plugin from a folder or a .zip on this computer into the plugin folder, after checking it: it "
        "must hold a plugin.json naming a script or a command, ask only for permissions Omabook knows, and stay "
        "inside its own folder and under the size limits. Answers what it is and the permissions it asks for. It "
        "is installed switched off unless `enable` is true; show the user its permissions before enabling it.",
        Mcp::schema({{"source", Mcp::string("The folder or .zip to install from.")},
                     {"name", Mcp::string("The folder name to install it under; taken from the package when not given.")},
                     {"enable", Mcp::boolean("Switch it on at once. False when not given.")},
                     {"replace", Mcp::boolean("Replace a plugin of the same name, which goes to the trash. False when not given.")}},
                    {"source"}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            PluginTools::InstallOptions options;
            options.name = arguments.value("name").toString();
            options.enable = arguments.value("enable").toBool();
            options.replace = arguments.value("replace").toBool();
            return answered(PluginTools::install({}, arguments.value("source").toString(), options));
        },
    });
    ok = Mcp::registerTool({
        "plugin_remove", "Remove a plugin",
        "Moves one of the user's own plugins (a folder in the plugin folder, by its name from plugin_list) to the "
        "trash. Built-in plugins and Calibre's can't be removed, only switched off with plugin_set_enabled. Ask the "
        "user first.",
        Mcp::schema({{"name", Mcp::string("The plugin's name, which is its folder's: lower-case letters, digits, - and _.")}},
                    {"name"}),
        Mcp::Destructive,
        [](const QJsonObject &arguments) { return answered(PluginTools::remove({}, arguments.value("name").toString())); },
    }) && ok;
    return ok;
}();

} // namespace
