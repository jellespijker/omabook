#include "scripttools.h"

#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <cstdio>
#include <memory>

#include "ai.h"
#include "formatprovider.h"
#include "library.h"
#include "mcp.h"
#include "plugintools.h"
#include "scriptapi.h"
#include "scriptplugins.h"
#include "settings.h"

namespace {

QJsonObject problem(const QString &why) { return {{"error", why}}; }

const char *const nameRule = "A plugin's name is its folder's: lower-case letters, digits, - and _, such as \"reading-log\".";

// What a call works with: the settings, the plugin folder and the library,
// each the caller's when it brought one and the user's otherwise. The
// library is only opened when a script asks for it.
class Session {
public:
    explicit Session(const ScriptTools::Context &context) : m_context(context) {
        if (!m_context.settings) {
            m_ownSettings = std::make_unique<Settings>();
            m_context.settings = m_ownSettings.get();
        }
        if (m_context.folder.isEmpty())
            m_context.folder = ScriptPlugins::defaultFolder();
    }

    QString folder() const { return m_context.folder; }
    Settings *settings() const { return m_context.settings; }
    Library *library() {
        if (m_context.library)
            return m_context.library();
        if (!m_ownLibrary && !m_libraryTried) {
            m_libraryTried = true;
            auto library = std::make_unique<Library>(m_context.settings->libraryPath());
            if (library->open())
                m_ownLibrary = std::move(library);
        }
        return m_ownLibrary.get();
    }
    ScriptServices services() {
        ScriptServices services;
        services.settings = m_context.settings;
        services.library = [this] { return library(); };
        services.ai = [this](QString *why) { return makeAiProvider(*m_context.settings, why); };
        services.network = &m_network;
        return services;
    }

private:
    ScriptTools::Context m_context;
    std::unique_ptr<Settings> m_ownSettings;
    std::unique_ptr<Library> m_ownLibrary;
    bool m_libraryTried = false;
    QNetworkAccessManager m_network;
};

QJsonObject registered(const ScriptPlugin &plugin) {
    QJsonArray sources, catalogs, importers, commands, panels;
    for (const ScriptPlugin::Source &source : plugin.sources())
        sources << QJsonObject{{"name", source.name}, {"fields", QJsonArray::fromStringList(source.fields)}};
    for (const ScriptPlugin::Catalog &catalog : plugin.catalogs())
        catalogs << QJsonObject{{"format", catalog.format}, {"description", catalog.description}};
    for (const ScriptPlugin::Importer &importer : plugin.importers())
        importers << importer.name;
    for (const ScriptPlugin::Command &command : plugin.commands())
        commands << QJsonObject{{"id", command.id}, {"title", command.title}, {"place", command.place}, {"key", command.key}};
    for (const ScriptPlugin::Panel &panel : plugin.panels())
        panels << QJsonObject{{"place", panel.place}, {"title", panel.title}, {"source", QFileInfo(panel.source).fileName()}};
    QJsonArray formats;
    for (const ScriptPlugin::Format &format : plugin.formats())
        formats << QJsonObject{{"name", format.name}, {"extensions", QJsonArray::fromStringList(format.extensions)},
                               {"claims", format.claims.isCallable()}};
    return {{"metadata", sources}, {"catalogs", catalogs}, {"importers", importers}, {"commands", commands},
            {"events", QJsonArray::fromStringList(plugin.events())}, {"panels", panels}, {"formats", formats}};
}

// One plugin loaded for the length of a call, with what it said and what
// went wrong in it gathered up.
struct Loaded {
    std::unique_ptr<ScriptPlugin> plugin;
    QStringList said, failures;
    QString error; // why it didn't load

    Loaded(Session &session, const QString &name) {
        const ScriptManifest manifest = ScriptManifest::read(session.folder() + "/" + name);
        if (!ScriptTools::validName(name) || !QFileInfo(manifest.folder).isDir()) {
            error = "There is no plugin " + name + " in " + session.folder() + ".";
            return;
        }
        if (manifest.problem.isEmpty() && !manifest.isScript()) {
            error = name + " is not a script plugin: its plugin.json names no \"script\".";
            return;
        }
        plugin = std::make_unique<ScriptPlugin>(manifest, session.services());
        QObject::connect(plugin.get(), &ScriptPlugin::said, plugin.get(), [this](const QString &text) { said << text; });
        QObject::connect(plugin.get(), &ScriptPlugin::failed, plugin.get(), [this](const QString &why) { failures << why; });
        if (!plugin->load(&error))
            return;
        settle(10000);
        // The book formats it registers are Omabook's for as long as it is
        // loaded here, wherever its folder is and whether or not it is
        // switched on: what it makes or opens in this run is in a format
        // of its own. What had those names before has them again after.
        const QList<FormatProvider> before = Formats::providers();
        for (const ScriptPlugin::Format &format : plugin->formats()) {
            const FormatProvider provider = plugin->provider(format);
            for (const FormatProvider &known : before)
                if (known.id == provider.id)
                    replaced << known;
            formats << provider.id;
            Formats::addProvider(provider);
        }
    }

    ~Loaded() {
        for (const QString &id : std::as_const(formats))
            Formats::removeProvider(id);
        for (const FormatProvider &provider : std::as_const(replaced))
            Formats::addProvider(provider);
    }
    Loaded(const Loaded &) = delete;
    Loaded &operator=(const Loaded &) = delete;

    // Lets what the script was asked, and what it asked to be waited for,
    // finish. A timer it set or a request it merely started is not waited
    // for: a command that ran one thing is done when that thing is.
    bool settle(int milliseconds) {
        return ScriptPlugins::waitFor([this] { return plugin->answered(); }, milliseconds);
    }

    QStringList formats;
    QList<FormatProvider> replaced;
};

void print(FILE *stream, const QString &text) {
    fprintf(stream, "%s\n", qPrintable(text));
    fflush(stream);
}

QString pretty(const QJsonValue &value) {
    if (value.isString())
        return value.toString();
    if (value.isObject() || value.isArray()) {
        const QJsonDocument document = value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject());
        return QString::fromUtf8(document.toJson(QJsonDocument::Indented)).trimmed();
    }
    return value.isNull() || value.isUndefined() ? QString() : value.toVariant().toString();
}

} // namespace

bool ScriptTools::failed(const QJsonObject &answer) { return !answer.value("error").toString().isEmpty(); }

bool ScriptTools::validName(const QString &name) {
    static const QRegularExpression pattern("^[a-z0-9][a-z0-9_-]{0,63}$");
    return pattern.match(name).hasMatch();
}

QJsonObject ScriptTools::located(const QString &error) {
    static const QRegularExpression where("^([^:\\s]+):(\\d+): (.*)$", QRegularExpression::DotMatchesEverythingOption);
    const auto match = where.match(error);
    if (!match.hasMatch())
        return {{"file", ""}, {"line", 0}, {"message", error}};
    return {{"file", match.captured(1)}, {"line", match.captured(2).toInt()}, {"message", match.captured(3)}};
}

QJsonObject ScriptTools::api() {
    QJsonObject api = ScriptApi::json();
    api.insert("markdown", ScriptApi::guide());
    return api;
}

QJsonObject ScriptTools::list(const Context &context) {
    Session session(context);
    const ScriptPlugins plugins(session.services(), session.folder(), nullptr, false);
    QJsonArray array;
    for (const ScriptPlugins::Info &info : plugins.list())
        array << info.toJson();
    return {{"folder", session.folder()}, {"plugins", array}};
}

QJsonObject ScriptTools::create(const Context &context, const QString &name, const QString &templateName) {
    const Session session(context);
    if (!validName(name))
        return problem(nameRule);
    if (!ScriptApi::templates().contains(templateName))
        return problem("There is no template " + templateName + "; there are: " + ScriptApi::templates().join(", ") + ".");
    const QString folder = session.folder() + "/" + name;
    if (QFileInfo::exists(folder))
        return problem("There already is a " + folder + ".");
    if (!QDir().mkpath(folder))
        return problem("The plugin's folder can't be made: " + folder);
    QJsonArray files;
    for (const QString &file : ScriptApi::templateFiles(templateName)) {
        QFile written(folder + "/" + file);
        const QByteArray contents = QString::fromUtf8(ScriptApi::templateFile(templateName, file)).replace("{{name}}", name).toUtf8();
        if (!written.open(QIODevice::WriteOnly) || written.write(contents) != contents.size())
            return problem("The plugin can't be written to " + folder + ".");
        files << file;
    }
    return {{"name", name}, {"folder", folder}, {"template", templateName}, {"files", files},
            {"next", "Edit " + folder + "/main.js, then run: omabook plugin check " + name}};
}

QJsonObject ScriptTools::write(const Context &context, const QString &name, const QString &file, const QString &contents) {
    const Session session(context);
    if (!validName(name))
        return problem(nameRule);
    const QString folder = session.folder() + "/" + name;
    const QString target = QDir::cleanPath(folder + "/" + file);
    // A file of the plugin's own folder, wherever a link in it may lead.
    const QString parent = QFileInfo(target).absolutePath();
    bool inside = !file.isEmpty() && target.startsWith(folder + "/") && !QFileInfo(target).isSymLink();
    if (inside && QFileInfo::exists(parent))
        inside = (QFileInfo(parent).canonicalFilePath() + "/").startsWith(QFileInfo(folder).canonicalFilePath() + "/");
    if (!inside)
        return problem("\"" + file + "\" is not a file inside the plugin's folder.");
    QSaveFile written(target);
    written.setDirectWriteFallback(false);
    const QByteArray bytes = contents.toUtf8();
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !written.open(QIODevice::WriteOnly) || written.write(bytes) != bytes.size()
        || !written.commit())
        return problem("The file can't be written: " + target);
    QJsonObject answer{{"name", name}, {"file", target}};
    // Half a plugin (a script and no plugin.json yet) is not yet wrong.
    const ScriptManifest manifest = ScriptManifest::read(folder);
    if (QFileInfo::exists(folder + "/plugin.json") && (!manifest.problem.isEmpty() || manifest.isScript()))
        answer.insert("check", check(context, name));
    else
        answer.insert("next", "Write plugin.json too, naming the script: {\"name\": \"...\", \"script\": \"main.js\", \"permissions\": []}");
    return answer;
}

QJsonObject ScriptTools::check(const Context &context, const QString &name) {
    Session session(context);
    Loaded loaded(session, name);
    if (!loaded.plugin)
        return problem(loaded.error);
    QStringList errors = loaded.failures;
    if (!loaded.error.isEmpty())
        errors.prepend(loaded.error);
    QJsonArray list;
    for (const QString &error : std::as_const(errors))
        list << located(error);
    const ScriptManifest &manifest = loaded.plugin->manifest();
    QJsonObject answer{{"name", name},
                       {"ok", errors.isEmpty()},
                       {"errors", list},
                       {"permissions", QJsonArray::fromStringList(manifest.permissions)},
                       {"registered", registered(*loaded.plugin)},
                       {"said", QJsonArray::fromStringList(loaded.said)},
                       {"log", QJsonArray::fromStringList(loaded.plugin->takeLog())}};
    if (!errors.isEmpty())
        answer.insert("error", errors.first());
    return answer;
}

QJsonObject ScriptTools::run(const Context &context, const QString &name, const QString &what, const QJsonValue &arguments) {
    Session session(context);
    Loaded loaded(session, name);
    if (!loaded.plugin || !loaded.error.isEmpty())
        return problem(loaded.error);
    ScriptPlugin &plugin = *loaded.plugin;

    bool answered = false;
    QJsonValue result;
    QString error;
    const ScriptPlugin::Done done = [&](const QJsonValue &value, const QString &why) {
        answered = true;
        result = value;
        error = why;
    };
    const QString kind = what.section(':', 0, 0), target = what.section(':', 1);
    const QJsonObject object = arguments.toObject();
    if (!what.contains(':')) {
        // The arguments as a list; one that isn't a list is the only one.
        plugin.callFunction(what, arguments.isArray() ? arguments.toArray()
                                  : arguments.isNull() || arguments.isUndefined() ? QJsonArray() : QJsonArray{arguments}, done);
    } else if (kind == "metadata") {
        plugin.search(target, object, done);
    } else if (kind == "command") {
        QJsonObject where{{"book", 0}, {"place", "library"}};
        for (auto it = object.begin(); it != object.end(); ++it)
            where.insert(it.key(), it.value());
        plugin.runCommand(target, where, done);
    } else if (kind == "importer") {
        plugin.imported(target, object, done);
    } else if (kind == "catalog") {
        QJsonArray books = arguments.toArray();
        if (!arguments.isArray())
            if (const Library *library = session.library())
                for (const Book &book : library->books())
                    books << book.toJson();
        plugin.makeCatalog(target, books, done);
    } else if (kind == "format") {
        // The book at a path (a file, a folder or an archive), as the
        // script's open answers it.
        const BookSource source(arguments.isString() ? arguments.toString() : object.value("path").toString());
        QString why;
        const QJsonObject book = plugin.openBook(target, source, &why);
        done(book, why);
    } else if (kind == "event") {
        if (!ScriptApi::eventNames().contains(target))
            return problem("There is no event " + target + "; there are: " + ScriptApi::eventNames().join(", ") + ".");
        plugin.emitEvent(target, object);
        answered = true;
    } else {
        return problem("\"" + what + "\" is not something to run: name a top-level function, or metadata:<source>, "
                       "command:<id>, catalog:<format>, importer:<name> or event:<name>.");
    }
    const double minutes = qBound(0.001, context.waitMinutes, 24.0 * 60);
    const bool finished = ScriptPlugins::waitFor([&] { return answered && plugin.answered(); }, int(minutes * 60000));
    if (error.isEmpty() && !loaded.failures.isEmpty())
        error = loaded.failures.first();
    if (error.isEmpty() && !finished)
        error = QString("It was still running after %1 minutes and this command has stopped waiting. A command's plugin ends with "
                        "the command, so what it had not finished is dropped; what it already kept (omabook.settings, files "
                        "under data/, books in the library) stays. Run it again to go on from there, with --wait <minutes> "
                        "to allow longer; in the window (omabook open) a plugin runs for as long as the window does.")
                    .arg(minutes);
    QJsonObject answer{{"name", name}, {"ran", what}, {"result", result}, {"said", QJsonArray::fromStringList(loaded.said)},
                       {"log", QJsonArray::fromStringList(plugin.takeLog())}};
    if (!error.isEmpty())
        answer.insert("error", error);
    return answer;
}

QJsonObject ScriptTools::enable(const Context &context, const QString &name, bool enabled) {
    const Session session(context);
    const ScriptManifest manifest = ScriptManifest::read(session.folder() + "/" + name);
    if (!validName(name) || !QFileInfo(manifest.folder).isDir() || !manifest.isScript())
        return problem("There is no script plugin " + name + " in " + session.folder() + ".");
    session.settings()->setPluginEnabled("folder:" + name, enabled);
    return {{"name", name}, {"id", "folder:" + name}, {"enabled", enabled}};
}

int runPluginCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Script plugins: JavaScript in ~/.config/omabook/plugins/<name>/ that adds sources of book details, catalog "
        "formats, commands, panels and hooks.\n\n"
        "  plugin new <name> [--template t]   Start a plugin from a template: " + ScriptApi::templates().join(", ") + "\n"
        "  plugin check <name>                Load it with no window; errors come with file and line\n"
        "  plugin run <name> <what> [json]    Run a top-level function, or metadata:<source>, command:<id>,\n"
        "                                     catalog:<format>, importer:<name>, event:<name>, and print the answer\n"
        "  plugin list                        Every script plugin, what it registered and what is wrong with it\n"
        "  plugin enable|disable <name>       Switch one on or off\n"
        "  plugin install <folder|zip>        Install a plugin someone else wrote, checked first; it stays off until\n"
        "                                     enabled (or --enable), so its permissions can be read\n"
        "  plugin remove <name>               Move one of your own plugins to the trash\n"
        "  plugin api                         Everything a script can call, as Markdown (or --json)");
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"template", "What `new` starts from; hook when not given.", "template", "hook"});
    parser.addOption({"library", "Use the library in this folder instead of the current one.", "folder"});
    parser.addOption({"folder", "Use this plugin folder instead of ~/.config/omabook/plugins.", "folder"});
    parser.addOption({"wait", "How many minutes `run` waits for an answer before giving up; 10 when not given.", "minutes", "10"});
    parser.addOption({"name", "install: the folder name to install under, instead of the package's own.", "name"});
    parser.addOption({"enable", "install: switch the plugin on at once."});
    parser.addOption({"replace", "install: replace a plugin of the same name, which goes to the trash."});
    parser.addPositionalArgument("action", "new, check, run, list, enable, disable, install, remove or api.");
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = "omabook plugin";
    if (!parser.parse(rest)) {
        print(stderr, parser.errorText());
        return 1;
    }
    const QStringList words = parser.positionalArguments();
    const QString action = words.value(0);
    const bool json = parser.isSet("json");
    if (parser.isSet("help") || action.isEmpty()) {
        print(action.isEmpty() && !parser.isSet("help") ? stderr : stdout, parser.helpText().trimmed());
        return parser.isSet("help") ? 0 : 1;
    }

    ScriptTools::Context context;
    context.folder = parser.value("folder");
    context.waitMinutes = parser.value("wait").toDouble();
    std::unique_ptr<Library> library;
    if (parser.isSet("library")) {
        library = std::make_unique<Library>(parser.value("library"));
        QString error;
        if (!library->open(&error)) {
            print(stderr, error);
            return 1;
        }
        context.library = [&library] { return library.get(); };
    }
    auto finish = [&](const QJsonObject &answer, const QString &text) {
        if (json)
            print(stdout, pretty(answer));
        else if (!text.isEmpty())
            print(stdout, text);
        if (ScriptTools::failed(answer) && !json)
            print(stderr, answer.value("error").toString());
        return ScriptTools::failed(answer) ? 1 : 0;
    };
    auto wants = [&](int count, const QString &usage) {
        if (words.size() - 1 >= count)
            return true;
        print(stderr, "Usage: omabook plugin " + usage);
        return false;
    };

    if (action == "api") {
        if (json)
            print(stdout, pretty(ScriptApi::json()));
        else
            print(stdout, ScriptApi::guide().trimmed());
        return 0;
    }
    if (action == "list") {
        const QJsonObject answer = ScriptTools::list(context);
        QStringList lines;
        for (const QJsonValue &value : answer.value("plugins").toArray()) {
            const QJsonObject plugin = value.toObject();
            QStringList has;
            for (const char *kind : {"metadata", "catalogs", "importers", "commands", "events", "panels"})
                if (const qsizetype count = plugin.value(kind).toArray().size())
                    has << QString("%1 %2").arg(count).arg(kind);
            const QString trouble = plugin.value("problem").toString();
            lines << QString("%1\t%2\t%3%4").arg(!plugin.value("enabled").toBool() ? "off" : plugin.value("running").toBool() ? "on" : "broken",
                                                 plugin.value("name").toString(), has.join(", "),
                                                 trouble.isEmpty() ? QString() : "\t(" + trouble.section('\n', 0, 0) + ")");
        }
        return finish(answer, lines.join('\n'));
    }
    if (action == "new") {
        if (!wants(1, "new <name> [--template " + ScriptApi::templates().join("|") + "]"))
            return 1;
        const QJsonObject answer = ScriptTools::create(context, words.at(1), parser.value("template"));
        return finish(answer, ScriptTools::failed(answer) ? QString()
                                  : "Made " + answer.value("folder").toString() + "\n" + answer.value("next").toString());
    }
    if (action == "check") {
        if (!wants(1, "check <name>"))
            return 1;
        const QJsonObject answer = ScriptTools::check(context, words.at(1));
        QStringList lines;
        if (!ScriptTools::failed(answer)) {
            QStringList has;
            const QJsonObject registered = answer.value("registered").toObject();
            for (auto it = registered.begin(); it != registered.end(); ++it)
                if (const qsizetype count = it.value().toArray().size())
                    has << QString("%1 %2").arg(count).arg(it.key());
            lines << words.at(1) + " loads" + (has.isEmpty() ? QString(", and registers nothing") : ": " + has.join(", ")) + ".";
            const QStringList permissions = answer.value("permissions").toVariant().toStringList();
            if (!permissions.isEmpty())
                lines << "It may: " + permissions.join(", ") + ".";
        } else if (!json) {
            // Every error, not only the first.
            const QJsonArray errors = answer.value("errors").toArray();
            for (qsizetype i = 1; i < errors.size(); ++i) {
                const QJsonObject where = errors.at(i).toObject();
                print(stderr, where.value("file").toString().isEmpty() ? where.value("message").toString()
                              : QString("%1:%2: %3").arg(where.value("file").toString()).arg(where.value("line").toInt()).arg(where.value("message").toString()));
            }
        }
        for (const QJsonValue &line : answer.value("log").toArray())
            if (!json)
                print(stderr, line.toString());
        return finish(answer, lines.join('\n'));
    }
    if (action == "run") {
        if (!wants(2, "run <name> <function | metadata:<source> | command:<id> | catalog:<format> | importer:<name> | event:<name>> [json]"))
            return 1;
        QJsonValue given;
        if (words.size() > 3) {
            QJsonParseError error;
            // A document is an object or a list, so a bare value rides in a list.
            const QJsonDocument document = QJsonDocument::fromJson(("[" + words.mid(3).join(' ') + "]").toUtf8(), &error);
            if (error.error != QJsonParseError::NoError) {
                print(stderr, "The arguments aren't valid JSON: " + error.errorString());
                return 1;
            }
            given = document.array().at(0);
        }
        const QJsonObject answer = ScriptTools::run(context, words.at(1), words.at(2), given);
        if (!json) {
            for (const QJsonValue &line : answer.value("log").toArray())
                print(stderr, line.toString());
            for (const QJsonValue &line : answer.value("said").toArray())
                print(stdout, line.toString());
        }
        return finish(answer, pretty(answer.value("result")));
    }
    if (action == "enable" || action == "disable") {
        if (!wants(1, action + " <name>"))
            return 1;
        const QJsonObject answer = ScriptTools::enable(context, words.at(1), action == "enable");
        return finish(answer, ScriptTools::failed(answer) ? QString() : words.at(1) + (action == "enable" ? " is on." : " is off."));
    }
    if (action == "install") {
        if (!wants(1, "install <folder|zip> [--name <name>] [--enable] [--replace]"))
            return 1;
        PluginTools::InstallOptions options;
        options.name = parser.value("name");
        options.enable = parser.isSet("enable");
        options.replace = parser.isSet("replace");
        const QJsonObject answer = PluginTools::install(context, words.at(1), options);
        QStringList lines;
        if (!ScriptTools::failed(answer)) {
            const QString name = answer.value("name").toString();
            lines << "Installed " + answer.value("title").toString() + " in " + answer.value("folder").toString();
            // What it may do is said before it is on, in the words plugin.json uses.
            const QStringList permissions = answer.value("permissions").toVariant().toStringList();
            lines << (answer.value("kind").toString() == "program"
                          ? QString("It is a program of its own: it can do whatever you can on this computer.")
                          : permissions.isEmpty() ? QString("It asks for nothing beyond the library.")
                                                  : "It may: " + permissions.join(", ") + ".");
            lines << (options.enable ? name + " is on." : "It is off. Switch it on with: omabook plugins enable folder:" + name);
        }
        return finish(answer, lines.join('\n'));
    }
    if (action == "remove") {
        if (!wants(1, "remove <name>"))
            return 1;
        const QJsonObject answer = PluginTools::remove(context, words.at(1));
        return finish(answer, ScriptTools::failed(answer) ? QString()
                                  : words.at(1) + (answer.value("trashed").toBool() ? " is in the trash." : " is removed."));
    }
    print(stderr, "There is no omabook plugin " + action + ".\n\n" + parser.helpText().trimmed());
    return 1;
}

// The same, as tools for an agent talking to `omabook mcp`.
namespace {

Mcp::Result answered(const QJsonObject &answer) {
    return ScriptTools::failed(answer) ? Mcp::Result::fail(answer.value("error").toString()) : Mcp::Result::ok(answer);
}

// The server's library, which follows the window's.
ScriptTools::Context serverContext() {
    ScriptTools::Context context;
    context.library = []() -> Library * {
        QString error;
        // Kept for the length of the call by the server itself.
        return Mcp::library(&error).get();
    };
    return context;
}

const bool toolsRegistered = [] {
    const QString nameIs = "The plugin's name, which is its folder's: lower-case letters, digits, - and _.";
    bool ok = Mcp::registerTool({
        "plugin_api", "Plugin scripting reference",
        "How to write a script plugin for Omabook: the file layout, everything the `omabook` object offers a script "
        "(functions, events, permissions), the edit and check loop, and worked examples. Read this before plugin_write.",
        Mcp::schema(), Mcp::ReadOnly,
        [](const QJsonObject &) { return Mcp::Result::ok(ScriptTools::api()); },
    });
    ok = Mcp::registerTool({
        "plugin_list", "List script plugins",
        "Every script plugin in the plugin folder: whether it is on and running, what it registered (sources of "
        "details, catalogs, importers, commands, hooks, panels), its permissions, and what is wrong with it if "
        "anything is. (plugins_list lists everything else Omabook is made of.)",
        Mcp::schema(), Mcp::ReadOnly,
        [](const QJsonObject &) { return answered(ScriptTools::list(serverContext())); },
    }) && ok;
    ok = Mcp::registerTool({
        "plugin_write", "Write a plugin's file",
        "Writes one file of a script plugin (plugin.json, main.js, a panel's .qml), creating the plugin when it is "
        "new, and checks the plugin once it has both a plugin.json and its script. With `template` and no `file`, "
        "starts the plugin from a template instead. A running Omabook loads the plugin again by itself.",
        Mcp::schema({{"name", Mcp::string(nameIs)},
                     {"file", Mcp::string("The file inside the plugin's folder, such as plugin.json or main.js.")},
                     {"contents", Mcp::string("The whole text of the file.")},
                     {"template", Mcp::string("Start a new plugin from this template instead of writing a file: metadata, command, "
                                              "hook, catalog or panel.")}},
                    {"name"}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            const QString name = arguments.value("name").toString();
            if (arguments.contains("template") && !arguments.contains("file"))
                return answered(ScriptTools::create(serverContext(), name, arguments.value("template").toString()));
            return answered(ScriptTools::write(serverContext(), name, arguments.value("file").toString(),
                                               arguments.value("contents").toString()));
        },
    }) && ok;
    ok = Mcp::registerTool({
        "plugin_check", "Check a plugin",
        "Loads a script plugin with no window and says whether it works: errors with file and line, what it "
        "registered, what it logged. Run it after every plugin_write.",
        Mcp::schema({{"name", Mcp::string(nameIs)}}, {"name"}), Mcp::ReadOnly,
        // A check that finds errors still answers: the errors are the answer.
        [](const QJsonObject &arguments) {
            QJsonObject answer = ScriptTools::check(serverContext(), arguments.value("name").toString());
            if (answer.contains("errors"))
                answer.remove("error");
            return answered(answer);
        },
    }) && ok;
    Mcp::Tool run{
        "plugin_run", "Run part of a plugin",
        "Runs one thing of a script plugin and answers what it returned, waiting for a promise: a top-level "
        "function (`arguments` is its list of arguments), or `metadata:<source>` (a book), `command:<id>` (a "
        "context), `catalog:<format>`, `importer:<name>` (a book), `event:<name>` (the payload). The script runs "
        "with the permissions its plugin.json asks for, on the real library.",
        Mcp::schema({{"name", Mcp::string(nameIs)},
                     {"what", Mcp::string("A top-level function's name, or metadata:<source>, command:<id>, catalog:<format>, "
                                          "importer:<name>, event:<name>.")},
                     {"arguments", QJsonObject{{"description", "A list of arguments for a function; an object for the others."}}}},
                    {"name", "what"}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            return answered(ScriptTools::run(serverContext(), arguments.value("name").toString(), arguments.value("what").toString(),
                                             arguments.value("arguments")));
        },
    };
    run.openWorld = true;
    ok = Mcp::registerTool(run) && ok;
    ok = Mcp::registerTool({
        "plugin_enable", "Turn a script plugin on or off",
        "Switches a script plugin on or off; a running Omabook follows at once.",
        Mcp::schema({{"name", Mcp::string(nameIs)}, {"enabled", Mcp::boolean("True to turn it on, false to turn it off.")}},
                    {"name", "enabled"}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            return answered(ScriptTools::enable(serverContext(), arguments.value("name").toString(), arguments.value("enabled").toBool()));
        },
    }) && ok;
    return ok;
}();

} // namespace
