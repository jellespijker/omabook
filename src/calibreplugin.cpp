#include "calibreplugin.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutexLocker>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

#include "bookarchive.h"
#include "calibre.h"
#include "metadatasource.h"
#include "plugins.h"
#include "settings.h"

namespace {

// A library put off with "Not now" is offered again after this long.
constexpr qint64 askAgainAfter = 7 * 24 * 3600;

QStringList stringList(const QJsonValue &value) {
    QStringList list;
    for (const QJsonValue &item : value.toArray())
        list << item.toString();
    return list;
}

QJsonObject readJson(const QString &file) {
    QFile json(file);
    return json.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(json.readAll()).object() : QJsonObject();
}

// Calibre names fields its own way ("comments", "pubdate", "identifier:isbn").
QStringList fieldKeys(const QStringList &calibreFields) {
    static const QHash<QString, QString> names{
        {"title", "title"}, {"authors", "authors"}, {"series", "series"}, {"publisher", "publisher"},
        {"pubdate", "published"}, {"languages", "language"}, {"identifier:isbn", "isbn"}, {"comments", "description"},
        {"tags", "tags"},
    };
    QStringList keys;
    for (const QString &field : calibreFields)
        if (names.contains(field) && !keys.contains(names.value(field)))
            keys << names.value(field);
    return keys;
}

// Calibre's kinds of plugin, by the class a plugin is built on, with the
// kind Omabook files it under.
struct Kind {
    const char *base;
    const char *type;
    const char *kind;
};
const Kind kinds[] = {
    {"Source", "Metadata source", "metadata"},
    {"InputFormatPlugin", "Conversion input", "conversion"},
    {"OutputFormatPlugin", "Conversion output", "conversion"},
    {"FileTypePlugin", "File type", "import"},
    {"MetadataReaderPlugin", "Metadata reader", "format"},
    {"MetadataWriterPlugin", "Metadata writer", "format"},
    {"CatalogPlugin", "Catalog generator", "catalog"},
    {"AIProviderPlugin", "AI provider", "ai"},
    {"DevicePlugin", "Device interface", "device"},
    {"USBMS", "Device interface", "device"},
    {"MTP_DEVICE", "Device interface", "device"},
    {"InterfaceActionBase", "User interface action", "interface"},
    {"PreferencesPlugin", "Preferences", "interface"},
    {"EditBookToolPlugin", "Edit book tool", "interface"},
    {"ViewerPlugin", "Viewer", "interface"},
    {"StoreBase", "Store", "store"},
    {"InputProfile", "Input profile", "conversion"},
    {"OutputProfile", "Output profile", "conversion"},
    {"LibraryClosedPlugin", "Library closed", "other"},
};

// What Omabook makes of a plugin of each kind. Only two kinds are used; the
// others are listed with the reason, so it is plain what was found.
void judge(CalibreAddOn *addOn, bool calibreInstalled, bool converts) {
    if (addOn->kind == "metadata") {
        addOn->usable = calibreInstalled;
        addOn->note = calibreInstalled ? "Asked when a book's details are looked up, run inside Calibre."
                                       : "Calibre itself isn't installed, so this plugin can't be run.";
    } else if (addOn->type == "Conversion input" || addOn->type == "Conversion output") {
        addOn->usable = converts;
        addOn->note = converts ? "Used by Calibre's ebook-convert whenever Omabook converts a book."
                               : "Calibre's ebook-convert isn't installed, so this plugin can't be run.";
    } else if (addOn->type.isEmpty()) {
        addOn->note = "Omabook can't tell what kind of Calibre plugin this is, so it is left to Calibre.";
    } else {
        addOn->note = "Omabook can't use Calibre's \"" + addOn->type + "\" plugins yet; this one works in Calibre only.";
    }
}

// The first quoted text after `key =` in a piece of Python, with or without
// the _() that marks it for translation.
QString pythonString(const QString &code, const QString &key) {
    const QRegularExpression assignment("\\b" + key + "\\s*=\\s*(?:_\\(\\s*)?[ubr]*(['\"])(.*?)\\1");
    return assignment.match(code).captured(2);
}

// A plugin in Calibre's plugins folder is a zip whose __init__.py holds a
// class built on one of Calibre's plugin classes, with its name, version
// and author as plain assignments. Reading those takes no Calibre.
CalibreAddOn readZip(const QString &file) {
    CalibreAddOn addOn;
    addOn.installed = true;
    addOn.file = file;
    addOn.name = QFileInfo(file).completeBaseName();
    addOn.kind = "other";
    const Archive zip(file);
    if (!zip.entries().contains("__init__.py"))
        return addOn;
    const QString code = QString::fromUtf8(zip.read("__init__.py"));
    static const QRegularExpression classLine("^class\\s+\\w+\\s*\\(([^)]*)\\)\\s*:", QRegularExpression::MultilineOption);
    for (auto classes = classLine.globalMatch(code); classes.hasNext();) {
        const auto found = classes.next();
        const Kind *known = nullptr;
        for (const QString &base : found.captured(1).split(',')) {
            const QString bare = base.trimmed().section('.', -1);
            const auto match = std::find_if(std::begin(kinds), std::end(kinds), [&](const Kind &kind) { return bare == kind.base; });
            if (match != std::end(kinds))
                known = &*match;
        }
        if (!known)
            continue;
        // The class's own lines, up to the next thing at the left margin.
        static const QRegularExpression leftMargin("^\\S", QRegularExpression::MultilineOption);
        const qsizetype end = code.indexOf(leftMargin, found.capturedEnd());
        const QString body = code.mid(found.capturedEnd(), end < 0 ? -1 : end - found.capturedEnd());
        addOn.type = known->type;
        addOn.kind = known->kind;
        if (const QString name = pythonString(body, "name"); !name.isEmpty())
            addOn.name = name;
        addOn.author = pythonString(body, "author");
        addOn.description = pythonString(body, "description");
        static const QRegularExpression version("\\bversion\\s*=\\s*\\(([^)]*)\\)");
        QStringList parts;
        for (const QString &part : version.match(body).captured(1).split(',', Qt::SkipEmptyParts))
            parts << part.trimmed();
        addOn.version = parts.join('.');
        break;
    }
    return addOn;
}

CalibreAddOn addOnFromJson(const QJsonObject &object) {
    CalibreAddOn addOn;
    addOn.name = object.value("name").toString();
    addOn.type = object.value("type").toString();
    addOn.kind = object.value("kind").toString();
    addOn.version = object.value("version").toString();
    addOn.author = object.value("author").toString();
    addOn.description = object.value("description").toString();
    addOn.fields = stringList(object.value("fields"));
    addOn.installed = object.value("installed").toBool();
    addOn.file = object.value("file").toString();
    addOn.enabledInCalibre = object.value("enabled_in_calibre").toBool(true);
    addOn.usable = object.value("usable").toBool();
    addOn.note = object.value("note").toString();
    return addOn;
}

} // namespace

const QString CalibrePlugin::id = "calibre";

QJsonObject CalibreLibrary::toJson() const {
    return {{"path", path}, {"name", name}, {"books", books}, {"answer", answer}, {"answered_at", double(answeredAt)}};
}

QJsonObject CalibreAddOn::toJson() const {
    return {{"name", name}, {"type", type}, {"kind", kind}, {"version", version}, {"author", author},
            {"description", description}, {"fields", QJsonArray::fromStringList(fields)}, {"installed", installed},
            {"file", file}, {"enabled_in_calibre", enabledInCalibre}, {"usable", usable}, {"note", note}};
}

CalibrePlugin::Where CalibrePlugin::Where::onThisComputer() {
    Where where;
    where.calibreDebug = QStandardPaths::findExecutable("calibre-debug");
    where.ebookConvert = QStandardPaths::findExecutable("ebook-convert");
    // Calibre lets its settings be kept elsewhere, and so follows this.
    const QString moved = qEnvironmentVariable("CALIBRE_CONFIG_DIRECTORY");
    where.configFolder = !moved.isEmpty() ? moved
                                          : QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/calibre";
    where.defaultLibrary = QDir::homePath() + "/Calibre Library";
    where.cacheFolder = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omabook";
    return where;
}

CalibrePlugin::Where CalibrePlugin::Where::standIn(const QString &calibreDebug) {
    Where where;
    where.calibreDebug = calibreDebug == "-" ? QString() : calibreDebug;
    where.configFolder = qEnvironmentVariable("CALIBRE_CONFIG_DIRECTORY");
    where.cacheFolder = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omabook";
    return where;
}

CalibrePlugin::CalibrePlugin(Settings *settings, const QString &calibreDebug)
    : CalibrePlugin(settings, calibreDebug.isEmpty() ? Where::onThisComputer() : Where::standIn(calibreDebug)) {}

CalibrePlugin::CalibrePlugin(Settings *settings, const Where &where) : m_settings(settings), m_where(where) {}

bool CalibrePlugin::installed() const { return !m_where.calibreDebug.isEmpty() || !m_where.ebookConvert.isEmpty(); }

// Whether there is a library is asked on every look at the plugin list, so
// Calibre's settings are read for it once; listing the libraries looks again.
bool CalibrePlugin::found() const {
    if (installed())
        return true;
    {
        QMutexLocker locker(&m_lock);
        if (m_looked)
            return m_anyLibrary;
    }
    const bool any = !libraryFolders().isEmpty();
    QMutexLocker locker(&m_lock);
    m_looked = true;
    m_anyLibrary = any;
    return any;
}

// "Off" answers false whatever the default, and "on" true whatever the
// default: that is how a choice is told from none.
bool CalibrePlugin::switchedOff() const { return !m_settings->pluginEnabled(id, true); }

bool CalibrePlugin::enabled() const {
    if (switchedOff())
        return false;
    return m_settings->pluginEnabled(id, false) || found();
}

void CalibrePlugin::setEnabled(bool enabled) { m_settings->setPluginEnabled(id, enabled); }

// The folders Calibre's own settings name, most used first, then the folder
// it makes for a first library. Calibre remembers libraries that have since
// been moved or deleted, so only folders that still hold one count.
QStringList CalibrePlugin::libraryFolders() const {
    QStringList named;
    if (!m_where.configFolder.isEmpty()) {
        named << readJson(m_where.configFolder + "/global.py.json").value("library_path").toString();
        // How often each library was opened; older Calibres kept it in gui.py.json.
        for (const char *file : {"/gui.json", "/gui.py.json"}) {
            const QJsonObject uses = readJson(m_where.configFolder + file).value("library_usage_stats").toObject();
            QStringList folders = uses.keys();
            std::stable_sort(folders.begin(), folders.end(), [&](const QString &a, const QString &b) {
                return uses.value(a).toInt() > uses.value(b).toInt();
            });
            named += folders;
        }
    }
    named << m_where.defaultLibrary;
    QStringList folders;
    for (const QString &folder : std::as_const(named)) {
        if (folder.isEmpty() || !Calibre::isLibrary(folder))
            continue;
        const QString clean = QDir(folder).absolutePath();
        if (!folders.contains(clean))
            folders << clean;
    }
    return folders;
}

QList<CalibreLibrary> CalibrePlugin::libraries() const {
    QList<CalibreLibrary> libraries;
    if (!enabled())
        return libraries;
    const QJsonObject answers = m_settings->calibreLibraries();
    const QStringList folders = libraryFolders();
    {
        QMutexLocker locker(&m_lock);
        m_looked = true;
        m_anyLibrary = !folders.isEmpty();
    }
    for (const QString &folder : folders) {
        CalibreLibrary library;
        library.path = folder;
        library.name = QFileInfo(folder).fileName();
        library.books = Calibre::bookCount(folder);
        library.answer = answers.value(folder).toObject().value("answer").toString();
        library.answeredAt = qint64(answers.value(folder).toObject().value("at").toDouble());
        libraries << library;
    }
    return libraries;
}

QList<CalibreLibrary> CalibrePlugin::librariesToOffer(bool asked) const {
    QList<CalibreLibrary> offered;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const CalibreLibrary &library : libraries()) {
        // An empty library, or one that can't be read, is nothing to offer.
        if (library.books <= 0 || library.answer == "imported" || library.answer == "never")
            continue;
        if (library.answer == "later" && !asked && now - library.answeredAt < askAgainAfter)
            continue;
        offered << library;
    }
    return offered;
}

void CalibrePlugin::answer(const QString &library, const QString &answer) {
    m_settings->setCalibreLibrary(QDir(library).absolutePath(), answer);
}

bool CalibrePlugin::reads(const QString &folder) const { return !switchedOff() && Calibre::isLibrary(folder); }

ImportOptions CalibrePlugin::importOptions() const {
    ImportOptions options;
    options.calibre = !switchedOff();
    return options;
}

// The bridge runs inside Calibre and is under Calibre's license, so it is a
// file of its own installed beside Omabook (share/omabook), never part of
// the program. A build that isn't installed uses the one in its source tree.
QString CalibrePlugin::bridge() const {
    const QString installed = QStandardPaths::locate(QStandardPaths::GenericDataLocation, "omabook/calibre_bridge.py");
    if (!installed.isEmpty())
        return installed;
    const QString source = QStringLiteral(OMABOOK_BRIDGE_SOURCE);
    return QFileInfo::exists(source) ? source : QString();
}

QString CalibrePlugin::cacheFile() const { return m_where.cacheFolder + "/calibre-plugins.json"; }

// What the list of plugins depends on: which Calibre it was asked of, and
// when that Calibre and its plugins last changed. While this is unchanged,
// the list on disk still holds: Calibre wasn't updated and no plugin was
// added, removed or switched in Calibre.
QString CalibrePlugin::stamp() const {
    auto changed = [](const QString &path) {
        const QFileInfo info(path);
        return info.exists() ? QString::number(info.lastModified().toMSecsSinceEpoch()) : QString("-");
    };
    return QStringList{OMABOOK_VERSION, m_where.calibreDebug, m_where.ebookConvert, m_where.configFolder,
                       changed(m_where.calibreDebug), changed(m_where.configFolder + "/plugins"),
                       changed(m_where.configFolder + "/customize.py.json")}
        .join('|');
}

QList<CalibreAddOn> CalibrePlugin::discover() const {
    QList<CalibreAddOn> addOns;
    const bool converts = !m_where.ebookConvert.isEmpty();
    const bool runs = !m_where.calibreDebug.isEmpty();

    // Calibre's metadata sources, asked of Calibre itself: only it knows
    // which are set up and what each can bring.
    const QString script = runs ? bridge() : QString();
    if (!script.isEmpty()) {
        QProcess calibre;
        calibre.start(m_where.calibreDebug, {"-e", script, "--", "list"});
        if (!calibre.waitForFinished(15000)) {
            calibre.kill();
            calibre.waitForFinished(1000);
        }
        const QByteArray output = calibre.readAllStandardOutput();
        const qsizetype at = output.lastIndexOf("OMABOOK-JSON:");
        const QJsonArray listed = at < 0 ? QJsonArray() : QJsonDocument::fromJson(output.mid(at + 13).split('\n').first()).array();
        for (const QJsonValue &value : listed) {
            const QJsonObject object = value.toObject();
            CalibreAddOn addOn;
            addOn.name = object.value("name").toString();
            addOn.type = "Metadata source";
            addOn.kind = "metadata";
            addOn.version = object.value("version").toString();
            addOn.author = object.value("author").toString();
            addOn.description = object.value("description").toString();
            addOn.fields = fieldKeys(stringList(object.value("fields")));
            addOn.installed = object.value("installed").toBool();
            addOn.enabledInCalibre = object.value("enabled").toBool();
            judge(&addOn, true, converts);
            if (!object.value("identifies").toBool()) {
                // Plugins that only fetch covers answer nothing a look-up can show.
                addOn.usable = false;
                addOn.note = "It only fetches covers, which Omabook's look-up doesn't ask Calibre for.";
            } else if (!object.value("configured").toBool()) {
                addOn.usable = false;
                addOn.note = "It needs setting up in Calibre first (Preferences, Metadata download).";
            }
            addOns << addOn;
        }
    }

    // What the user installed in Calibre, of any kind, read from the plugins
    // folder. customize.py.json names each plugin's zip and holds Calibre's
    // own switches.
    if (!m_where.configFolder.isEmpty()) {
        const QJsonObject customize = readJson(m_where.configFolder + "/customize.py.json");
        // Calibre writes a set as {"__class__": "set", "__value__": [...]}.
        auto names = [&](const QString &key) {
            const QJsonValue value = customize.value(key);
            return stringList(value.isObject() ? value.toObject().value("__value__") : value);
        };
        const QStringList disabled = names("disabled_plugins"), enabled = names("enabled_plugins");
        QHash<QString, QString> nameOfZip;
        const QJsonObject zips = customize.value("plugins").toObject();
        for (auto it = zips.constBegin(); it != zips.constEnd(); ++it)
            nameOfZip.insert(QFileInfo(it.value().toString()).fileName(), it.key());

        const QDir folder(m_where.configFolder + "/plugins");
        for (const QFileInfo &zip : folder.entryInfoList({"*.zip"}, QDir::Files, QDir::Name)) {
            CalibreAddOn addOn = readZip(zip.filePath());
            if (nameOfZip.contains(zip.fileName()))
                addOn.name = nameOfZip.value(zip.fileName());
            const auto listed = std::find_if(addOns.begin(), addOns.end(),
                                             [&](const CalibreAddOn &other) { return other.name == addOn.name; });
            if (listed != addOns.end()) {
                // Calibre already described it; this adds where it lives.
                listed->installed = true;
                listed->file = addOn.file;
                continue;
            }
            addOn.enabledInCalibre = !disabled.contains(addOn.name) || enabled.contains(addOn.name);
            judge(&addOn, runs, converts);
            addOns << addOn;
        }
    }
    return addOns;
}

QList<CalibreAddOn> CalibrePlugin::knownAddOns() const {
    if (!enabled())
        return {};
    QMutexLocker locker(&m_lock);
    if (!m_loaded) {
        const QJsonObject cached = readJson(cacheFile());
        m_stamp = cached.value("stamp").toString();
        m_addOns.clear();
        // A list made for another Calibre (or for none) says nothing of this
        // one; a list that is merely old is shown until Calibre is asked again.
        const QString asked = QStringList{m_where.calibreDebug, m_where.ebookConvert, m_where.configFolder}.join('|');
        if (cached.value("asked").toString() == asked)
            for (const QJsonValue &value : cached.value("plugins").toArray())
                m_addOns << addOnFromJson(value.toObject());
        else
            m_stamp.clear();
        m_loaded = true;
    }
    return m_addOns;
}

QList<CalibreAddOn> CalibrePlugin::addOns(bool refresh) const {
    if (!enabled())
        return {};
    knownAddOns();
    const QString current = stamp();
    {
        QMutexLocker locker(&m_lock);
        if (!refresh && m_stamp == current)
            return m_addOns;
    }
    // Calibre takes a moment to start, so it is asked outside the lock.
    const QList<CalibreAddOn> found = discover();
    QJsonArray plugins;
    for (const CalibreAddOn &addOn : found)
        plugins << addOn.toJson();
    const QString asked = QStringList{m_where.calibreDebug, m_where.ebookConvert, m_where.configFolder}.join('|');
    QDir().mkpath(m_where.cacheFolder);
    QSaveFile cache(cacheFile());
    if (cache.open(QIODevice::WriteOnly)
        && cache.write(QJsonDocument(QJsonObject{{"stamp", current}, {"asked", asked}, {"plugins", plugins}})
                           .toJson(QJsonDocument::Compact)) > 0)
        cache.commit();
    QMutexLocker locker(&m_lock);
    m_stamp = current;
    m_addOns = found;
    return m_addOns;
}

QList<PluginInfo> CalibrePlugin::entries() const {
    const bool on = enabled();
    const bool off = switchedOff();
    auto row = [](const QString &id, const QString &kind, const QString &name, const QString &description) {
        PluginInfo plugin;
        plugin.id = id;
        plugin.name = name;
        plugin.kind = kind;
        plugin.origin = "built-in";
        plugin.version = OMABOOK_VERSION;
        plugin.author = "Omabook";
        plugin.description = description;
        plugin.switchable = false;
        return plugin;
    };

    // The plugin itself: part of Omabook, and the one part that can be
    // switched off.
    PluginInfo self = row(id, "calibre", "Calibre",
                          found() || off
                              ? "Everything Omabook does with Calibre: importing its libraries with what Calibre knows of "
                                "your reading, asking its metadata plugins, and converting with ebook-convert. Switched off, "
                                "nothing of Calibre's is run, read or offered."
                              : "Neither Calibre nor a Calibre library was found on this computer. Switched on, a Calibre "
                                "library you add is still taken with Calibre's details.");
    self.switchable = true;
    self.enabled = on;

    // What it does, under the kinds the rest of Omabook is sorted into.
    PluginInfo conversion = row("conversion:Calibre conversion", "conversion", "Calibre conversion",
                                "Every format Calibre converts (Kindle, Word, DjVu, RTF, ...), turned into EPUB when a "
                                "book is opened, through ebook-convert and all of Calibre's own conversion plugins.");
    PluginInfo libraries = row("import:Calibre Library", "import", "Calibre Library",
                               "Adds a whole Calibre library with Calibre's details, covers and every format, and what "
                               "Calibre knows of your reading: books read, stars, identifiers and highlights.");
    if (off) {
        conversion.problem = libraries.problem = "The Calibre plugin is switched off.";
        conversion.enabled = libraries.enabled = false;
    } else if (m_where.ebookConvert.isEmpty() && on) {
        conversion.problem = "Calibre isn't installed, so only the formats Omabook reads itself can be opened.";
    }

    QList<PluginInfo> rows{self, conversion, libraries};
    for (const CalibreAddOn &addOn : knownAddOns()) {
        // A source that only fetches covers is no source of details.
        if (addOn.kind == "metadata" && !addOn.usable && addOn.note.startsWith("It only fetches covers"))
            continue;
        PluginInfo plugin = row("calibre:" + addOn.name, addOn.kind == "metadata" ? "metadata" : "calibre", addOn.name,
                                addOn.description);
        plugin.origin = "calibre";
        plugin.version = addOn.version;
        plugin.author = addOn.author;
        plugin.fields = addOn.fields;
        // Calibre's own switch is the default here; the registry decides.
        plugin.enabled = addOn.enabledInCalibre;
        if (addOn.kind == "metadata") {
            plugin.switchable = true;
            if (!addOn.usable)
                plugin.problem = addOn.note;
        } else {
            // Listed beneath the Calibre plugin, saying what it is and what
            // Omabook makes of it.
            plugin.description = (addOn.type.isEmpty() ? QString("Calibre plugin") : addOn.type + " plugin for Calibre")
                                 + (addOn.description.isEmpty() ? "." : ": " + addOn.description);
            plugin.enabled = addOn.usable && addOn.enabledInCalibre;
            if (addOn.usable)
                plugin.description += " " + addOn.note;
            else
                plugin.problem = addOn.note;
        }
        rows << plugin;
    }
    return rows;
}

MetadataSource *CalibrePlugin::source(const PluginInfo &plugin, QObject *parent) const {
    if (!enabled() || m_where.calibreDebug.isEmpty() || plugin.origin != "calibre" || plugin.kind != "metadata")
        return nullptr;
    const QString script = bridge();
    if (script.isEmpty())
        return nullptr;
    return new CalibreMetadataSource(plugin.name, plugin.fields, m_where.calibreDebug, script, parent);
}

QString CalibrePlugin::converter() const { return enabled() ? m_where.ebookConvert : QString(); }

QString CalibrePlugin::converterProblem() const {
    if (switchedOff())
        return "Converting needs Calibre, and the Calibre plugin is switched off. Switch it on with: omabook plugins enable calibre";
    if (m_where.ebookConvert.isEmpty())
        return "Reading this format needs Calibre's ebook-convert, which isn't installed.";
    return {};
}
