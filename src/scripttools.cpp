#include "scripttools.h"

#include <QCommandLineParser>
#include <QDateTime>
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
#include <optional>

#include "ai.h"
#include "formatprovider.h"
#include "library.h"
#include "mcp.h"
#include "pluginargument.h"
#include "pluginfinder.h"
#include "pluginsources.h"
#include "plugintools.h"
#include "secretstore.h"
#include "scriptapi.h"
#include "scriptplugins.h"
#include "scriptsession.h"
#include "testaudio.h"
#include "settings.h"

namespace {

QJsonObject problem(const QString &why) { return {{"error", why}}; }

constexpr auto nameRule = "A plugin's name is its folder's: lower-case letters, digits, - and _, such as \"reading-log\".";

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
    QJsonArray launchers;
    for (const ScriptPlugin::Launcher &launcher : plugin.launchers())
        launchers << QJsonObject{{"id", launcher.id}, {"title", launcher.title}, {"author", launcher.author},
                                 {"cover", launcher.cover.isEmpty() ? QString() : QFileInfo(launcher.cover).fileName()}};
    QJsonArray formats;
    for (const ScriptPlugin::Format &format : plugin.formats())
        formats << QJsonObject{{"name", format.name}, {"extensions", QJsonArray::fromStringList(format.extensions)},
                               {"claims", format.claims.isCallable()}};
    return {{"metadata", sources}, {"catalogs", catalogs}, {"importers", importers}, {"commands", commands},
            {"events", QJsonArray::fromStringList(plugin.events())}, {"panels", panels}, {"formats", formats},
            {"launchers", launchers}};
}

// One plugin loaded for the length of a call, with what it said and what
// went wrong in it gathered up.
struct Loaded {
    std::unique_ptr<ScriptPlugin> plugin;
    QStringList said, failures;
    QString error; // why it didn't load

    Loaded(ScriptSession &session, const QString &name) {
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
    ScriptSession session(context);
    const ScriptPlugins plugins(session.services(), session.folder(), nullptr, false);
    QJsonArray array;
    for (const ScriptPlugins::Info &info : plugins.list())
        array << info.toJson();
    return {{"folder", session.folder()}, {"plugins", array}};
}

QJsonObject ScriptTools::create(const Context &context, const QString &name, const QString &templateName) {
    const ScriptSession session(context);
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
        const QByteArray contents = QString::fromUtf8(ScriptApi::templateFile(templateName, file)).replace("{{name}}", name)
                                        .replace("{{omabook}}", ScriptApi::hostVersion()).toUtf8();
        if (!written.open(QIODevice::WriteOnly) || written.write(contents) != contents.size())
            return problem("The plugin can't be written to " + folder + ".");
        files << file;
    }
    return {{"name", name}, {"folder", folder}, {"template", templateName}, {"files", files},
            {"next", "Edit " + folder + "/main.js, then run: omabook plugin check " + name}};
}

QJsonObject ScriptTools::write(const Context &context, const QString &name, const QString &file, const QString &contents) {
    const ScriptSession session(context);
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
    ScriptSession session(context);
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
    ScriptSession session(context);
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
    } else if (kind == "launcher") {
        // Opening it, as the shelf does: there is no window to put a form
        // in, so a form it shows is answered null.
        plugin.launch(target, done);
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
                       "command:<id>, catalog:<format>, importer:<name>, launcher:<id> or event:<name>.");
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
    const ScriptSession session(context);
    const ScriptManifest manifest = ScriptManifest::read(session.folder() + "/" + name);
    if (!validName(name) || !QFileInfo(manifest.folder).isDir() || !manifest.isScript())
        return problem("There is no script plugin " + name + " in " + session.folder() + ".");
    session.settings()->setPluginEnabled("folder:" + name, enabled);
    return {{"name", name}, {"id", "folder:" + name}, {"enabled", enabled}};
}

// The library a command was given (--library), and the sound `run` tries a
// plugin with (#135): a test microphone and a yes to recording given
// beforehand, never on the person's own library, since what is recorded there
// goes to their other computers. False, with why said, when it can't be.
bool whatRunUses(const QCommandLineParser &parser, ScriptTools::Context &context, std::unique_ptr<Library> &library,
                 std::optional<AudioDevices> &room) {
    if (parser.isSet("library")) {
        library = std::make_unique<Library>(parser.value("library"));
        QString error;
        if (!library->open(&error)) {
            print(stderr, error);
            return false;
        }
        context.library = [&library] { return library.get(); };
    }
    const bool testing = parser.isSet("test-microphone");
    if (!testing && !parser.isSet("allow-microphone"))
        return true;
    if (!parser.isSet("library") || !ScriptSession::scratch(Settings(), parser.value("library"))) {
        print(stderr, "--test-microphone and --allow-microphone are for a scratch library: name one with --library that is "
                      "none of your own libraries, such as a new folder.");
        return false;
    }
    const auto hears = testing ? TestAudio::microphone(parser.value("test-microphone")) : std::nullopt;
    if (testing && !hears) {
        print(stderr, "--test-microphone hears tone, silence, or a WAV file of mono 16-bit samples at 48 kHz.");
        return false;
    }
    if (hears)
        context.audio = &room.emplace(*hears, [] { return TestAudio::speakers(); });
    context.microphoneGranted = parser.isSet("allow-microphone");
    return true;
}

int runPluginCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Script plugins: JavaScript in ~/.config/omabook/plugins/<name>/ that adds sources of book details, catalog "
        "formats, commands, panels and hooks.\n\n"
        "  plugin new <name> [--template t]   Start a plugin from a template: " + ScriptApi::templates().join(", ") + "\n"
        "  plugin check <name>                Load it with no window; errors come with file and line\n"
        "  plugin run <name> <what> [json|-]  Run a top-level function, or metadata:<source>, command:<id>,\n"
        "                                     catalog:<format>, importer:<name>, launcher:<id>, event:<name>, and\n"
        "                                     print the answer; - reads the JSON from standard input\n"
        "  plugin list                        Every script plugin, what it registered and what is wrong with it\n"
        "  plugin enable|disable <name>       Switch one on or off\n"
        "  plugin install <folder|zip>        Install a plugin someone else wrote, checked first; it stays off until\n"
        "                                     enabled (or --enable), so its permissions can be read\n"
        "  plugin remove <name>               Move one of your own plugins to the trash\n"
        "  plugin search [words]              Find plugins to install: those Omabook's registry lists first, then\n"
        "                                     repositories on GitHub with the topic omabook-plugin, each checked first\n"
        "  plugin add <name|github address>   Install a listed plugin by its name, or one from a GitHub repository\n"
        "                                     (owner/repo, or a link to it, to a release, tag, branch or folder of it),\n"
        "                                     checked the same way, and remember where it came from; --pin <tag|branch>\n"
        "                                     stays at one\n"
        "  plugin sources                     Where each plugin came from, what it follows and when it was last checked;\n"
        "                                     --updates auto|tell|never says what is done about updates\n"
        "  plugin update [<name>|--all]       Install what is newer (--check only says); one that asks for a permission\n"
        "                                     the installed version doesn't have waits for --yes\n"
        "  plugin pin <name> <tag|branch>     Stay at a tag or follow a branch; plugin follow <name> goes back to the\n"
        "                                     latest release, or the default branch\n"
        "  plugin rollback <name>             Go back to the version before the last update\n"
        "  plugin forget-source <name>        Stop updating a plugin; it stays installed\n"
        "  plugin token [--forget]            Keep a GitHub token (read from standard input) in the keyring, for\n"
        "                                     plugins in private repositories\n"
        "  plugin api                         Everything a script can call, as Markdown (or --json)");
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"template", "What `new` starts from; hook when not given.", "template", "hook"});
    parser.addOption({"library", "Use the library in this folder instead of the current one.", "folder"});
    parser.addOption({"folder", "Use this plugin folder instead of ~/.config/omabook/plugins.", "folder"});
    parser.addOption({"wait", "How many minutes `run` waits for an answer before giving up; 10 when not given.", "minutes", "10"});
    parser.addOption({"test-microphone", "run, in a scratch library (--library): the microphone hears this: tone, silence, or a WAV "
                                         "file of mono 16-bit samples at 48 kHz; what plays goes nowhere.", "sound"});
    parser.addOption({"allow-microphone", "run, in a scratch library (--library): the plugin may record without asking."});
    parser.addOption({"name", "install: the folder name to install under, instead of the package's own.", "name"});
    parser.addOption({"enable", "install: switch the plugin on at once."});
    parser.addOption({"replace", "install: replace a plugin of the same name, which goes to the trash."});
    parser.addOption({"pin", "add: the tag to stay at, or the branch to follow, instead of the latest release.", "ref"});
    parser.addOption({"all", "update: every plugin that has a source."});
    parser.addOption({"check", "update: only say whether there is something newer."});
    parser.addOption({"yes", "update: also install an update that asks for a permission the installed version doesn't have."});
    parser.addOption({"updates", "sources: what is done about updates: auto, tell or never.", "mode"});
    parser.addOption({"forget", "token: take the token out of the keyring."});
    parser.addPositionalArgument("action", "new, check, run, list, enable, disable, install, remove, search, add, sources, update, pin, "
                                           "follow, rollback, forget-source, token or api.");
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
    std::optional<AudioDevices> testRoom;
    if (!whatRunUses(parser, context, library, testRoom))
        return 1;
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
        if (!wants(2, "run <name> <function | metadata:<source> | command:<id> | catalog:<format> | importer:<name> | event:<name>> [json | -]"))
            return 1;
        const auto given = PluginArgument::parse(words.mid(3));
        if (!given) {
            print(stderr, "The arguments aren't valid JSON: " + given.error());
            return 1;
        }
        const QJsonObject answer = ScriptTools::run(context, words.at(1), words.at(2), *given);
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
    // Plugins from GitHub repositories, and keeping them up to date.
    const auto channelText = [](const QJsonObject &channel) {
        const QString kind = channel.value("kind").toString("auto"), name = channel.value("name").toString();
        return kind == "tag" ? "stays at " + name
               : kind == "branch" ? "follows the branch " + name
                                  : QString("follows the latest release, or the default branch while there is none");
    };
    const auto versionText = [](const QJsonObject &installed) {
        const QString version = installed.value("version").toString(), commit = installed.value("commit").toString().left(7);
        return (version.isEmpty() ? installed.value("ref").toString() : version) + (commit.isEmpty() ? QString() : " (" + commit + ")");
    };
    // What became of one source that was checked or updated, as a line.
    const auto outcome = [&](const QJsonObject &result) {
        const QString name = result.value("name").toString();
        if (result.value("updated").toBool())
            return name + ": updated from " + result.value("from").toString() + " to " + result.value("to").toString() + ".";
        if (result.value("waits").toBool())
            return name + ": " + result.value("pending").toObject().value("ref").toString() + " needs Omabook "
                   + result.value("pending").toObject().value("needs").toString() + " or newer; this is " + ScriptApi::hostVersion()
                   + ", so the version installed stays.";
        if (result.value("review").toBool()) {
            const QJsonObject pending = result.value("pending").toObject();
            const QStringList more = pending.value("permissions").toVariant().toStringList();
            return name + ": version " + pending.value("version").toString()
                   + (more.isEmpty() ? " goes by another name (" + pending.value("title").toString() + ")"
                                     : " asks for more than the one installed (" + more.join(", ") + ")")
                   + ", so it wasn't installed. If that is all right: omabook plugin update " + name + " --yes";
        }
        if (ScriptTools::failed(result))
            return name + ": " + result.value("error").toString();
        if (result.value("pinned").toBool())
            return name + ": " + channelText(result.value("channel").toObject()) + ", so there is nothing to check.";
        if (result.value("update").toBool())
            return name + ": " + result.value("latest").toObject().value("ref").toString() + " is newer than what is installed ("
                   + versionText(result.value("installed").toObject()) + "). Install it: omabook plugin update " + name;
        return name + ": up to date.";
    };
    if (action == "sources") {
        ScriptSession session(context);
        if (parser.isSet("updates")) {
            if (!QStringList{"auto", "tell", "never"}.contains(parser.value("updates"))) {
                print(stderr, "Updates are auto (installed by themselves), tell (only said) or never (not checked).");
                return 1;
            }
            session.settings()->setPluginUpdates(parser.value("updates"));
        }
        const PluginSources sources({session.folder(), session.settings(), {}});
        const QJsonArray list = sources.list();
        const QString mode = session.settings()->pluginUpdates();
        QStringList lines{mode == "auto" ? "Updates install by themselves, unless one asks for more than the version it replaces."
                          : mode == "tell" ? "Updates are only told of." : "Updates are not checked for."};
        for (const QJsonValue &value : list) {
            const QJsonObject entry = value.toObject();
            const qint64 checked = qint64(entry.value("checked_at").toDouble());
            const QString folderPart = entry.value("subfolder").toString();
            lines << QString("%1\t%2\t%3\t%4\tchecked %5%6")
                         .arg(entry.value("name").toString(), entry.value("repository").toString() + (folderPart.isEmpty() ? QString() : "/" + folderPart),
                              channelText(entry.value("channel").toObject()), versionText(entry.value("installed").toObject()),
                              checked > 0 ? QDateTime::fromSecsSinceEpoch(checked).toString("yyyy-MM-dd HH:mm") : QString("never"),
                              entry.value("missing").toBool() ? "\t(the plugin is gone)"
                              : !entry.value("pending").toObject().isEmpty() ? "\t(an update waits for you: it asks for more)"
                              : !entry.value("problem").toString().isEmpty() ? "\t(" + entry.value("problem").toString() + ")"
                              : entry.value("update").toBool() ? "\t(an update is there)" : QString());
        }
        if (list.isEmpty())
            lines << "No plugin came from a GitHub repository. Add one: omabook plugin add owner/repo";
        return finish(QJsonObject{{"folder", sources.folder()}, {"updates", mode}, {"sources", list}}, lines.join('\n'));
    }
    if (action == "search") {
        PluginFinder finder(context);
        const QJsonObject answer = PluginSources::wait([&](PluginSources::Done done) { finder.search(words.mid(1).join(' '), done); });
        QStringList lines;
        for (const QJsonValue &value : answer.value("results").toArray()) {
            const QJsonObject found = value.toObject();
            const QStringList permissions = found.value("permissions").toVariant().toStringList();
            QStringList facts{found.value("listed").toBool() ? (found.value("reviewed").toBool() ? "listed, reviewed" : "listed")
                                                             : "not reviewed"};
            if (found.contains("stars"))
                facts << QString::number(found.value("stars").toInt()) + (found.value("stars").toInt() == 1 ? " star" : " stars");
            if (!found.value("updated").toString().isEmpty())
                facts << "updated " + found.value("updated").toString();
            if (!found.value("license").toString().isEmpty())
                facts << found.value("license").toString();
            lines << found.value("name").toString() + "\t" + found.value("title").toString() + "\t" + found.value("repository").toString()
                         + "\t(" + facts.join(", ") + ")";
            lines << "  " + found.value("description").toString();
            lines << "  " + (found.value("program").toBool() ? QString("It is a program of its own: it can do whatever you can on this computer.")
                             : permissions.isEmpty() ? QString("It asks for nothing beyond the library.")
                                                     : "It may: " + permissions.join(", ") + ".")
                         + (found.value("installed").toBool() ? QString(" It is installed.")
                            : !found.value("installable").toBool() ? " It can't be installed here: " + found.value("problem").toString()
                            : " Install it: omabook plugin add " + (found.value("listed").toBool() ? found.value("name").toString()
                                                                                                  : found.value("address").toString()));
            if (!found.value("listed").toBool())
                lines << "  Not reviewed: nobody has looked at it for you. A plugin can do whatever its permissions allow.";
        }
        if (answer.value("results").toArray().isEmpty())
            lines << "No plugin was found" + (words.size() > 1 ? " for \"" + words.mid(1).join(' ') + "\"" : QString()) + ".";
        if (!answer.value("problem").toString().isEmpty())
            lines << answer.value("problem").toString();
        else if (!answer.value("discovery").toBool())
            lines << "Only the registry was searched: looking on GitHub is switched off in the settings.";
        return finish(answer, lines.join('\n'));
    }
    if (action == "add") {
        if (!wants(1, "add <name|github address> [--name <name>] [--pin <tag|branch>] [--enable] [--replace]"))
            return 1;
        PluginSources sources(context);
        PluginSources::AddOptions options;
        options.name = parser.value("name");
        options.pin = parser.value("pin");
        options.enable = parser.isSet("enable");
        options.replace = parser.isSet("replace");
        const QJsonObject answer = PluginSources::wait([&](PluginSources::Done done) { sources.add(words.at(1), options, done); });
        QStringList lines;
        if (!ScriptTools::failed(answer)) {
            const QString name = answer.value("name").toString();
            lines << "Installed " + answer.value("title").toString() + " " + answer.value("version").toString() + " from "
                         + answer.value("repository").toString() + " (" + answer.value("ref").toString() + ", "
                         + answer.value("commit").toString().left(7) + ") in " + answer.value("folder").toString();
            const QStringList permissions = answer.value("permissions").toVariant().toStringList();
            lines << (answer.value("kind").toString() == "program"
                          ? QString("It is a program of its own: it can do whatever you can on this computer.")
                          : permissions.isEmpty() ? QString("It asks for nothing beyond the library.")
                                                  : "It may: " + permissions.join(", ") + ".");
            lines << "It " + channelText(answer.value("channel").toObject()) + ".";
            lines << (answer.value("enabled").toBool() ? name + " is on." : "It is off. Switch it on with: omabook plugins enable folder:" + name);
        }
        return finish(answer, lines.join('\n'));
    }
    if (action == "update") {
        if (words.size() < 2 && !parser.isSet("all")) {
            print(stderr, "Usage: omabook plugin update <name>|--all [--check] [--yes]");
            return 1;
        }
        PluginSources sources(context);
        const bool only = parser.isSet("check"), approved = parser.isSet("yes");
        QJsonObject answer;
        QStringList lines;
        if (parser.isSet("all")) {
            answer = PluginSources::wait([&](PluginSources::Done done) { sources.checkAll(!only, false, done); });
            for (const QJsonValue &result : answer.value("results").toArray())
                lines << outcome(result.toObject());
            if (answer.value("results").toArray().isEmpty())
                lines << "No plugin has a source to check; see omabook plugin sources.";
            // One that failed fails the command; the rest were still done.
            for (const QJsonValue &result : answer.value("results").toArray())
                if (ScriptTools::failed(result.toObject()) && !answer.contains("error"))
                    answer.insert("error", outcome(result.toObject()));
            if (json)
                print(stdout, pretty(answer));
            else
                print(ScriptTools::failed(answer) ? stderr : stdout, lines.join('\n'));
            return ScriptTools::failed(answer) ? 1 : 0;
        }
        answer = PluginSources::wait([&](PluginSources::Done done) {
            if (only)
                sources.check(words.at(1), done);
            else
                sources.update(words.at(1), approved, done);
        });
        return finish(answer, ScriptTools::failed(answer) ? QString() : outcome(answer));
    }
    if (action == "pin" || action == "follow") {
        if (!wants(action == "pin" ? 2 : 1, action == "pin" ? "pin <name> <tag|branch>" : "follow <name>"))
            return 1;
        PluginSources sources(context);
        QJsonObject answer;
        if (action == "follow") {
            answer = sources.follow(words.at(1));
            if (!ScriptTools::failed(answer))
                answer = PluginSources::wait([&](PluginSources::Done done) { sources.update(words.at(1), false, done); });
        } else {
            answer = PluginSources::wait([&](PluginSources::Done done) { sources.pin(words.at(1), words.at(2), done); });
        }
        return finish(answer, ScriptTools::failed(answer) ? QString()
                                  : words.at(1) + " " + channelText(sources.source(words.at(1)).value("channel").toObject()) + ". "
                                        + outcome(answer).section(": ", 1));
    }
    if (action == "rollback") {
        if (!wants(1, "rollback <name>"))
            return 1;
        const QJsonObject answer = PluginSources(context).rollBack(words.at(1));
        return finish(answer, ScriptTools::failed(answer) ? QString()
                                  : words.at(1) + " is back at " + answer.value("version").toString() + " (it was "
                                        + answer.value("from").toString() + ").");
    }
    if (action == "forget-source") {
        if (!wants(1, "forget-source <name>"))
            return 1;
        const QJsonObject answer = PluginSources(context).forget(words.at(1));
        return finish(answer, ScriptTools::failed(answer) ? QString()
                                  : words.at(1) + " is no longer updated from " + answer.value("repository").toString() + ". It stays installed.");
    }
    if (action == "token") {
        ScriptSession session(context);
        if (parser.isSet("forget")) {
            Secrets::shared().forget("github");
            session.settings()->setPluginToken(false);
            return finish(QJsonObject{{"token", false}}, "The GitHub token is forgotten.");
        }
        // From standard input, so it is never in a command line or a history.
        char line[512] = {0};
        const QString secret = fgets(line, sizeof line, stdin) ? QString::fromUtf8(line).trimmed() : QString();
        if (secret.isEmpty()) {
            print(stderr, "Give the token on standard input: omabook plugin token < file, or pipe it in.");
            return 1;
        }
        QString error, why;
        if (!Secrets::shared().put("github", secret, &error)) {
            print(stderr, "The token couldn't be kept: " + error);
            return 1;
        }
        session.settings()->setPluginToken(true);
        const bool lasting = Secrets::shared().lasting(&why);
        return finish(QJsonObject{{"token", true}, {"lasting", lasting}},
                      lasting ? QString("The GitHub token is in the keyring. It is sent to api.github.com and nowhere else.")
                              : "The GitHub token is kept for this run only: " + why);
    }
    print(stderr, "There is no omabook plugin " + action + ".\n\n" + parser.helpText().trimmed());
    return 1;
}

// The same, as tools for an agent talking to `omabook mcp`.
namespace {

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
        [](const QJsonObject &) { return Mcp::Result::of(ScriptTools::list(serverContext())); },
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
                return Mcp::Result::of(ScriptTools::create(serverContext(), name, arguments.value("template").toString()));
            return Mcp::Result::of(ScriptTools::write(serverContext(), name, arguments.value("file").toString(),
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
            return Mcp::Result::of(answer);
        },
    }) && ok;
    Mcp::Tool run{
        "plugin_run", "Run part of a plugin",
        "Runs one thing of a script plugin and answers what it returned, waiting for a promise: a top-level "
        "function (`arguments` is its list of arguments), or `metadata:<source>` (a book), `command:<id>` (a "
        "context), `catalog:<format>`, `importer:<name>` (a book), `launcher:<id>` (as if opened on the shelf), `event:<name>` "
        "(the payload). The script runs "
        "with the permissions its plugin.json asks for, on the real library.",
        Mcp::schema({{"name", Mcp::string(nameIs)},
                     {"what", Mcp::string("A top-level function's name, or metadata:<source>, command:<id>, catalog:<format>, "
                                          "importer:<name>, event:<name>.")},
                     {"arguments", QJsonObject{{"description", "A list of arguments for a function; an object for the others."}}}},
                    {"name", "what"}),
        Mcp::Changes,
        [](const QJsonObject &arguments) {
            return Mcp::Result::of(ScriptTools::run(serverContext(), arguments.value("name").toString(), arguments.value("what").toString(),
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
            return Mcp::Result::of(ScriptTools::enable(serverContext(), arguments.value("name").toString(), arguments.value("enabled").toBool()));
        },
    }) && ok;
    return ok;
}();

} // namespace
