#include "plugins.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QProcess>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <concepts>

#include "catalog.h"
#include "goodreads.h"
#include "hardcover.h"
#include "scriptapi.h"
#include "settings.h"

namespace {

using namespace Qt::StringLiterals;

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list << item.toString();
    return list;
}

// The catalogs Omabook brings, each by the id it is listed under and with
// what makes it. To add one: its class, and a line here and in builtIn().
template <std::derived_from<MetadataSource> Source>
MetadataSource *makeSource(QNetworkAccessManager *network, QObject *parent) {
    return new Source(network, parent); // the parent's from here on
}

struct BuiltInSource {
    QLatin1StringView id;
    MetadataSource *(*make)(QNetworkAccessManager *network, QObject *parent);
};

constexpr std::array builtInSources{
    BuiltInSource{"builtin:Open Library"_L1, makeSource<OpenLibrarySource>},
    BuiltInSource{"builtin:Google Books"_L1, makeSource<GoogleBooksSource>},
    BuiltInSource{"builtin:Apple Books"_L1, makeSource<AppleBooksSource>},
    BuiltInSource{"builtin:Crossref"_L1, makeSource<CrossrefSource>},
    BuiltInSource{"builtin:Goodreads"_L1, makeSource<GoodreadsSource>},
    BuiltInSource{"builtin:Hardcover"_L1, makeSource<HardcoverSource>},
};

} // namespace

Plugins::Plugins(Settings *settings, const QString &folder, const QString &calibreDebug)
    : m_settings(settings),
      m_folder(folder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omabook/plugins"
                                : folder),
      m_calibre(settings, calibreDebug) {}

QList<PluginInfo> Plugins::builtIn() const {
    auto make = [](const QString &name, const QString &description, const QStringList &fields) {
        PluginInfo plugin;
        plugin.id = "builtin:" + name;
        plugin.name = name;
        plugin.origin = "built-in";
        plugin.version = OMABOOK_VERSION;
        plugin.author = "Omabook";
        plugin.description = description;
        plugin.fields = fields;
        return plugin;
    };
    // What Omabook is made of, by the kinds Calibre sorts its plugins into.
    // These are always on: they are the app.
    auto part = [&](const QString &kind, const QString &name, const QString &description) {
        PluginInfo plugin = make(name, description, {});
        plugin.id = kind + ":" + name;
        plugin.kind = kind;
        plugin.switchable = false;
        return plugin;
    };
    // What Omabook does with Calibre is listed by the Calibre plugin itself.
    // The field lists are the sources' own, asked of an instance.
    QNetworkAccessManager *none = nullptr;
    // Hardcover is asked with the reader's own token, so it is of no use
    // until they connected; the keyring itself is not asked here.
    PluginInfo hardcover = make("Hardcover", "Hardcover's catalog, kept by its readers: series, genres, descriptions, covers and "
                                             "what its readers rate a book, with your token (Reading Sites). Off, nothing is "
                                             "asked of hardcover.app.",
                                HardcoverSource(none).fields());
    if (!m_settings->site("hardcover").value("secret").toBool())
        hardcover.problem = "Connect Hardcover under Reading Sites first: it is asked with your own token.";
    return {
        part("format", "EPUB", "Reads .epub: text, pictures, contents, details and cover."),
        part("format", "FictionBook", "Reads .fb2, .fb2.zip and .fbz, with footnotes, details and cover."),
        part("format", "Text", "Reads plain text, Markdown and single HTML files, finding chapters in plain text."),
        part("format", "Markdown books", "Reads a folder of Markdown files, or that folder zipped (.mdz, or a .zip of Markdown), as "
                                         "one book: chapters from SUMMARY.md or by name, details from front matter, pictures and links."),
        part("format", "PDF", "Reads .pdf page by page, with its outline, text and details."),
        part("format", "Comics", "Reads .cbz, .cbr, .cb7 and .cbt, with ComicInfo details."),
        part("format", "Kindle details", "Reads the details and cover of .mobi, .azw and .azw3 without converting them."),
        part("import", "Archive Extract", "Adds the books packed inside a .zip, .rar, .7z or .tar."),
        part("reading", "Goodreads shelves", "Your shelves, stars and dates on Goodreads, from its export file or a public "
                                             "profile, and written back out for its import page. It follows the Goodreads "
                                             "source: with that off, nothing is asked of goodreads.com."),
        part("reading", "Open Library reading log", "The reading log of an Open Library account: want to read, reading and read. "
                                                    "Signed in with the account's keys, a private log too, and changes "
                                                    "made here can go back."),
        part("reading", "Hardcover library", "Your library on Hardcover, read and, when you turn that on, written, with a "
                                             "token from your account there. It follows the Hardcover source: with that "
                                             "off, nothing is asked of hardcover.app."),
        part("catalog", "CSV", Catalog::describe("csv")),
        part("catalog", "BibTeX", Catalog::describe("bibtex")),
        part("ai", "Claude Code", "Claude through the claude command and its subscription, with every tool off."),
        part("ai", "Antigravity", "Gemini through the agy command and its subscription, in plan mode and a sandbox."),
        part("ai", "Anthropic API", "Claude through the API, with the key in ANTHROPIC_API_KEY."),
        part("ai", "Command", "Any program that reads a prompt and writes an answer: Ollama, llm, a script."),
        make("Open Library", "The Internet Archive's open catalog of every book ever published.", OpenLibrarySource(none).fields()),
        make("Google Books", "Google's index of books, with descriptions and cover pictures.", GoogleBooksSource(none).fields()),
        make("Apple Books", "Apple's bookshop: sharp covers and publishers' descriptions for books on sale.", AppleBooksSource(none).fields()),
        make("Crossref", "The registry of scholarly publishing: textbooks, monographs, anything with a DOI.", CrossrefSource(none).fields()),
        make("Goodreads", "Goodreads, read natively: series, genres and readers' ratings in a look-up, and your shelves "
                          "there in step with the library (omabook goodreads). Off, nothing is asked of goodreads.com.",
             GoodreadsSource(none).fields()),
        hardcover,
    };
}

// A folder plugin is a folder holding plugin.json:
//   {"name": "...", "version": "1.0", "author": "...", "description": "...",
//    "type": "metadata", "fields": ["title", ...], "command": ["./lookup.py"]}
QList<PluginInfo> Plugins::fromFolder() const {
    QList<PluginInfo> plugins;
    for (const QFileInfo &entry : QDir(m_folder).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        QFile manifest(entry.filePath() + "/plugin.json");
        if (!manifest.open(QIODevice::ReadOnly))
            continue;
        QJsonParseError error;
        const QJsonObject object = QJsonDocument::fromJson(manifest.readAll(), &error).object();
        PluginInfo plugin;
        plugin.id = "folder:" + entry.fileName();
        plugin.name = object.value("name").toString(entry.fileName());
        plugin.kind = object.value("type").toString("metadata");
        plugin.origin = "folder";
        plugin.version = object.value("version").toString();
        plugin.author = object.value("author").toString();
        plugin.description = object.value("description").toString();
        plugin.fields = stringList(object.value("fields"));
        plugin.folder = entry.filePath();
        plugin.command = stringList(object.value("command"));
        plugin.script = object.value("script").toString();
        plugin.permissions = stringList(object.value("permissions"));
        // One that asks for a newer Omabook is neither loaded nor run.
        bool malformed = false;
        const QString need = ScriptApi::needsNewer(object.value("omabook"), &malformed);
        if (!malformed)
            plugin.needs = need;
        if (!plugin.script.isEmpty()) {
            // A script is run by ScriptPlugins, which says what is wrong
            // with it in <name>.error beside its folder.
            plugin.kind = "script";
            QFile trouble(entry.filePath() + ".error");
            if (trouble.open(QIODevice::ReadOnly))
                plugin.problem = QString::fromUtf8(trouble.readAll()).section('\n', 0, 0);
            plugins << plugin;
            continue;
        }
        if (error.error != QJsonParseError::NoError)
            plugin.problem = "plugin.json can't be read: " + error.errorString();
        else if (!need.isEmpty())
            plugin.problem = need;
        else if (plugin.kind != "metadata")
            plugin.problem = "This Omabook doesn't know plugins of type \"" + plugin.kind + "\".";
        else if (plugin.command.isEmpty())
            plugin.problem = "plugin.json names no command to run.";
        plugins << plugin;
    }
    return plugins;
}

// Whether each plugin is on: the user's choice where they made one, and
// otherwise on, except that a Calibre plugin follows Calibre's own switch
// and stays off when Omabook has the same catalog built in.
void Plugins::decide(QList<PluginInfo> *plugins) const {
    static const QStringList doubled{"calibre:Google", "calibre:Open Library"};
    for (PluginInfo &plugin : *plugins) {
        // The Calibre plugin has its own default: on where Calibre is found.
        if (plugin.id == CalibrePlugin::id) {
            plugin.enabled = m_calibre.enabled();
            continue;
        }
        const bool byDefault = plugin.origin != "calibre" || (plugin.enabled && !doubled.contains(plugin.id));
        plugin.enabled = !plugin.switchable || m_settings->pluginEnabled(plugin.id, byDefault);
    }
}

QList<PluginInfo> Plugins::list(bool refresh) const {
    m_calibre.addOns(refresh);
    return known();
}

QList<PluginInfo> Plugins::known() const {
    QList<PluginInfo> plugins = builtIn() + fromFolder() + m_calibre.entries();
    decide(&plugins);
    // What can be added to and switched comes first, then what Omabook is made of.
    static const QStringList order{"metadata", "reading", "format", "conversion", "import", "catalog", "ai"};
    // Among the sources, the order the user set; one they never placed keeps
    // its own place after the ones they did.
    const QStringList wanted = m_settings->pluginOrder();
    auto place = [&](const PluginInfo &plugin) {
        const qsizetype at = plugin.kind == "metadata" ? wanted.indexOf(plugin.id) : -1;
        return at < 0 ? wanted.size() : at;
    };
    std::ranges::stable_sort(plugins, [&](const PluginInfo &a, const PluginInfo &b) {
        const qsizetype kindA = order.indexOf(a.kind), kindB = order.indexOf(b.kind);
        return kindA != kindB ? kindA < kindB : place(a) < place(b);
    });
    return plugins;
}

QStringList Plugins::order() const {
    QStringList ids;
    for (const PluginInfo &plugin : known())
        if (plugin.kind == "metadata")
            ids << plugin.id;
    return ids;
}

bool Plugins::move(const QString &id, int by) {
    QStringList ids = order();
    const qsizetype from = ids.indexOf(id), to = from + by;
    if (from < 0 || to < 0 || to >= ids.size() || by == 0)
        return false;
    ids.move(from, to);
    m_settings->setPluginOrder(ids);
    return true;
}

bool Plugins::setOrder(const QStringList &ids) {
    const QStringList sources = order();
    for (const QString &id : ids)
        if (!sources.contains(id) || ids.count(id) != 1)
            return false;
    m_settings->setPluginOrder(ids);
    return true;
}

bool Plugins::setEnabled(const QString &id, bool enabled) {
    for (const PluginInfo &plugin : known()) {
        if (plugin.id == id && plugin.switchable) {
            m_settings->setPluginEnabled(id, enabled);
            return true;
        }
    }
    return false;
}

QList<MetadataSource *> Plugins::sources(const QList<PluginInfo> &plugins, QNetworkAccessManager *network, QObject *parent) const {
    QList<MetadataSource *> sources;
    for (const PluginInfo &plugin : plugins) {
        if (!plugin.enabled || !plugin.problem.isEmpty() || plugin.kind != "metadata")
            continue;
        if (const auto own = std::ranges::find_if(builtInSources, [&plugin](const BuiltInSource &source) { return plugin.id == source.id; });
            own != builtInSources.end())
            sources << own->make(network, parent);
        else if (plugin.origin == "calibre") {
            if (MetadataSource *source = m_calibre.source(plugin, parent))
                sources << source;
        }
        else if (plugin.origin == "folder")
            sources << new ProcessMetadataSource(plugin.name, plugin.fields, plugin.command.first(), plugin.command.mid(1),
                                                 plugin.folder, parent);
    }
    return sources;
}
