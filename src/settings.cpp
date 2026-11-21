#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <array>
#include <utility>

#include "pagelayout.h"
#include "readingformat.h"

using namespace Qt::StringLiterals;

namespace {

// What a setting with a few words to choose from may be.
constexpr std::array graphModes{"auto"_L1, "ask"_L1, "off"_L1};
constexpr std::array markModes{"map"_L1, "always"_L1, "never"_L1};
constexpr std::array pluginUpdateModes{"auto"_L1, "tell"_L1, "never"_L1};
constexpr std::array sorts{"recent"_L1, "title"_L1,  "author"_L1,    "series"_L1,  "added"_L1,
                           "progress"_L1, "rating"_L1, "published"_L1, "finished"_L1};

} // namespace

Settings::Settings(const QString &file, QObject *parent)
    : QObject(parent),
      m_file(file.isEmpty() ? defaultFile() : file) {
    read();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { read(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { read(); });
    watch();
}

QString Settings::defaultFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omabook/config.json";
}

void Settings::read() {
    QFile config(m_file);
    if (config.open(QIODevice::ReadOnly)) {
        QJsonParseError error;
        const QJsonObject read = QJsonDocument::fromJson(config.readAll(), &error).object();
        // A file caught half-written, or with a typo, changes nothing.
        const QJsonObject values = error.error == QJsonParseError::NoError ? upgraded(read) : read;
        if (error.error == QJsonParseError::NoError && values != m_values) {
            m_values = values;
            emit changed();
        }
    }
    watch();
}

// A save replaces the file, which drops it from the watch.
void Settings::watch() {
    const QString folder = QFileInfo(m_file).absolutePath();
    if (QFileInfo::exists(m_file) && !m_watcher.files().contains(m_file))
        m_watcher.addPath(m_file);
    if (QFileInfo::exists(folder) && !m_watcher.directories().contains(folder))
        m_watcher.addPath(folder);
}

QString Settings::libraryPath() const {
    if (!m_sessionLibrary.isEmpty())
        return m_sessionLibrary;
    const QString path = m_values.value("library").toString();
    return path.isEmpty() ? QDir::homePath() + "/Books" : path;
}

void Settings::setLibraryPath(const QString &path) {
    const QString absolute = QDir(path).absolutePath();
    const bool wasSession = !std::exchange(m_sessionLibrary, QString()).isEmpty();
    QStringList known = libraries();
    known.removeAll(absolute);
    known.prepend(absolute);
    m_values.insert("libraries", QJsonArray::fromStringList(known));
    // Already the one in use: only the session's library is let go of.
    if (wasSession && m_values.value("library") == absolute) {
        save();
        emit changed();
        return;
    }
    set("library", absolute);
}

void Settings::useLibraryForSession(const QString &path) {
    const QString absolute = QDir(path).absolutePath();
    if (absolute == libraryPath())
        return;
    m_sessionLibrary = absolute;
    emit changed();
}

QStringList Settings::libraries() const {
    QStringList libraries;
    for (const QJsonValue &value : m_values.value("libraries").toArray())
        libraries << value.toString();
    if (!libraries.contains(libraryPath()))
        libraries.prepend(libraryPath());
    return libraries;
}

void Settings::forgetLibrary(const QString &path) {
    QStringList known = libraries();
    if (path == libraryPath() || !known.removeOne(path))
        return;
    set("libraries", QJsonArray::fromStringList(known));
}

QString Settings::graph() const {
    const QString mode = m_values.value("graph").toString();
    return std::ranges::contains(graphModes, mode) ? mode : QString("auto");
}

void Settings::setGraph(const QString &mode) {
    if (std::ranges::contains(graphModes, mode))
        set("graph", mode);
}

QString Settings::marks() const {
    const QString mode = m_values.value("marks").toString();
    return std::ranges::contains(markModes, mode) ? mode : QString("always");
}

void Settings::setMarks(const QString &mode) {
    if (std::ranges::contains(markModes, mode))
        set("marks", mode);
}

void Settings::setPaneWidth(const QString &pane, int width) {
    QJsonObject widths = m_values.value("pane_widths").toObject();
    if (width <= 0)
        widths.remove(pane);
    else
        widths.insert(pane, std::clamp(width, 120, 4000));
    if (widths != m_values.value("pane_widths").toObject())
        set("pane_widths", widths);
}

void Settings::setHighlightName(const QString &color, const QString &name) {
    QJsonObject names = m_values.value("highlight_names").toObject();
    if (const QString simple = name.simplified().left(40); simple.isEmpty())
        names.remove(color);
    else
        names.insert(color, simple);
    if (names != m_values.value("highlight_names").toObject())
        set("highlight_names", names);
}

QStringList Settings::hiddenAnnotators() const {
    QStringList hidden;
    for (const QJsonValue person : m_values.value("hidden_annotators").toArray())
        hidden << person.toString();
    return hidden;
}

void Settings::setAnnotatorHidden(const QString &person, bool hidden) {
    QStringList now = hiddenAnnotators();
    now.removeAll(person);
    if (hidden && !person.isEmpty())
        now << person;
    if (now != hiddenAnnotators())
        set("hidden_annotators", QJsonArray::fromStringList(now));
}

QJsonObject Settings::shelf(const QString &library) const {
    return m_values.value("shelves").toObject().value(library).toObject();
}

void Settings::setShelf(const QString &library, const QJsonObject &shelf) {
    QJsonObject shelves = m_values.value("shelves").toObject();
    shelves.insert(library, shelf);
    set("shelves", shelves);
}

QString Settings::graphTenant(const QString &library) const { return m_values.value("graph_tenants").toObject().value(library).toString(); }

void Settings::setGraphTenant(const QString &library, const QString &tenant) {
    QJsonObject tenants = m_values.value("graph_tenants").toObject();
    tenants.insert(library, tenant);
    set("graph_tenants", tenants);
}

int Settings::number(QAnyStringView key) const {
    const ReadingFormat::Range *range = ReadingFormat::range(key);
    return std::clamp(m_values.value(key.toString()).toInt(range->fallback), range->low, range->high);
}

void Settings::setNumber(QAnyStringView key, int value) {
    const ReadingFormat::Range *range = ReadingFormat::range(key);
    set(key.toString(), std::clamp(value, range->low, range->high));
}

int Settings::pages() const {
    const int pages = m_values.value("pages").toInt(0);
    return pages >= 1 && pages <= 3 ? pages : 0;
}

void Settings::setPages(int pages) {
    if (pages >= 0 && pages <= 3)
        set("pages", pages == 0 ? QJsonValue("auto") : QJsonValue(pages));
}

QString Settings::align() const {
    const QString align = m_values.value("align").toString();
    if (std::ranges::contains(ReadingFormat::alignments, align))
        return align;
    // A config.json from before there was a choice of alignment says only
    // whether the text is justified.
    return m_values.value("justify").toBool(false) ? QString("justify") : QString("start");
}

void Settings::setAlign(const QString &align) {
    if (std::ranges::contains(ReadingFormat::alignments, align))
        set("align", align);
}

QJsonObject Settings::format() const {
    QJsonObject format;
    for (const ReadingFormat::Range &range : ReadingFormat::ranges)
        format.insert(range.key, number(range.key));
    format.insert("font_family", fontFamily());
    format.insert("align", align());
    // The other choices, as config.json has them, or their defaults.
    for (const ReadingFormat::Choice &choice : ReadingFormat::choices)
        if (choice.key != "align"_L1) {
            const QString word = m_values.value(choice.key).toString();
            format.insert(choice.key, std::ranges::contains(choice.words, word) ? word : QString(choice.fallback));
        }
    return format;
}

QJsonObject Settings::options() const {
    QJsonObject options = format();
    options.insert("cover_size", coverSize());
    options.insert("pages", pages() == 0 ? QJsonValue("auto") : QJsonValue(pages()));
    options.insert("folio", folio());
    options.insert("sort", sort());
    return options;
}

bool Settings::setOption(const QString &key, const QJsonValue &value) {
    if (ReadingFormat::keys().contains(key)) {
        if (!ReadingFormat::problem(key, value).isEmpty())
            return false;
        set(key, key == "font_family" ? QJsonValue(value.toString().trimmed()) : value);
        return true;
    }
    // The older name for the alignment, still taken from an agent or a
    // change written before there was a choice.
    if (key == "justify") {
        if (!value.isBool())
            return false;
        set("align", value.toBool() ? "justify" : "start");
        return true;
    }
    // The older names of the longest line and of the pages side by side,
    // still taken from a plugin written before they changed.
    if (key == "line_width") {
        if (!value.isDouble() || value.toInt() < 320 || value.toInt() > 1600)
            return false;
        setMeasure(PageLayout::measureFromPixels(value.toInt(), fontSize()));
        return true;
    }
    if (key == "spread") {
        const QString spread = value.toString();
        if (spread != "auto" && spread != "always" && spread != "never")
            return false;
        setPages(spread == "always" ? 2 : spread == "never" ? 1 : 0);
        return true;
    }
    // The pages side by side, as a word or a number.
    if (key == "pages") {
        const QString word = value.isDouble() ? QString::number(value.toDouble()) : value.toString();
        if (word != "auto" && word != "1" && word != "2" && word != "3")
            return false;
        setPages(word == "auto" ? 0 : word.toInt());
        return true;
    }
    const auto within = [&value](int low, int high) { return value.isDouble() && value.toInt() >= low && value.toInt() <= high; };
    const auto oneOf = [&value](const auto &words) { return std::ranges::contains(words, value.toString()); };
    const bool fits = key == "cover_size" ? within(96, 480)
                      : key == "folio" ? value.isBool()
                      : key == "plugin_updates" ? oneOf(pluginUpdateModes)
                      : key == "sort" ? oneOf(sorts)
                      : false;
    if (fits)
        set(key, value);
    return fits;
}

QString Settings::aiModelFor(const QString &provider, const QString &tier) const {
    const QJsonObject chosen = m_values.value("ai_models").toObject().value(provider).toObject();
    if (chosen.contains(tier))
        return chosen.value(tier).toString();
    return provider == "anthropic" && tier == "main" ? aiModel() : QString();
}

void Settings::setAiModelFor(const QString &provider, const QString &tier, const QString &model) {
    QJsonObject all = m_values.value("ai_models").toObject();
    QJsonObject chosen = all.value(provider).toObject();
    // Kept even when empty: it is what says the older key no longer counts.
    chosen.insert(tier, model.trimmed());
    all.insert(provider, chosen);
    set("ai_models", all);
}

bool Settings::pluginEnabled(const QString &id, bool byDefault) const {
    return m_values.value("plugins").toObject().value(id).toBool(byDefault);
}

void Settings::setPluginEnabled(const QString &id, bool enabled) {
    QJsonObject plugins = m_values.value("plugins").toObject();
    plugins.insert(id, enabled);
    set("plugins", plugins);
}

void Settings::setCalibreLibrary(const QString &folder, const QString &answer) {
    QJsonObject libraries = calibreLibraries();
    if (answer.isEmpty())
        libraries.remove(folder);
    else
        libraries.insert(folder, QJsonObject{{"answer", answer}, {"at", double(QDateTime::currentSecsSinceEpoch())}});
    set("calibre_libraries", libraries);
}

QString Settings::siteIdentity(const QString &scheme) const {
    // The two first sites kept theirs under names of their own, and still do.
    if (scheme == "goodreads")
        return goodreadsUser();
    if (scheme == "openlibrary")
        return openLibraryUser();
    return site(scheme).value("identity").toString();
}

void Settings::setSiteIdentity(const QString &scheme, const QString &identity) {
    if (scheme == "goodreads") {
        setGoodreadsUser(identity);
    } else if (scheme == "openlibrary") {
        setOpenLibraryUser(identity);
    } else {
        QJsonObject connection = site(scheme);
        connection.insert("identity", identity.trimmed());
        setSite(scheme, connection);
    }
}

void Settings::setSite(const QString &scheme, const QJsonObject &connection) {
    QJsonObject sites = m_values.value("sites").toObject();
    if (connection.isEmpty())
        sites.remove(scheme);
    else
        sites.insert(scheme, connection);
    if (sites.isEmpty() && !m_values.contains("sites"))
        return;
    set("sites", sites);
}

bool Settings::siteOn(const QString &scheme) const {
    return scheme == "goodreads" ? pluginEnabled("builtin:Goodreads", true)
           : scheme == "hardcover" ? pluginEnabled("builtin:Hardcover", true)
           : true;
}

void Settings::set(const QString &key, const QJsonValue &value) {
    if (m_values.value(key) == value)
        return;
    m_values.insert(key, value);
    save();
    emit changed();
}

// The shape of config.json. It goes up when a key is renamed or restructured;
// the step that carries an older file forward is added to `upgraded` below,
// so a setting made in an older Omabook is never silently dropped. Keys that
// are only added need no step: a missing key reads as its default.
constexpr int configVersion = 3;

QJsonObject Settings::upgraded(QJsonObject values) {
    const int from = values.value("config_version").toInt(0);
    // 2: "justify" (true or false) became "align", which has four choices.
    if (from < 2 && values.contains("justify")) {
        if (!values.contains("align"))
            values.insert("align", values.value("justify").toBool() ? "justify" : "start");
        values.remove("justify");
    }
    // 3: the longest line became a measure in characters ("measure"), from
    // pixels ("line_width"), and the choice of two pages ("spread": "auto",
    // "always" or "never") a count of pages ("pages": "auto", 1, 2 or 3).
    // A width left at the old default is the new default.
    if (from < 3) {
        if (values.contains("line_width")) {
            const int pixels = values.value("line_width").toInt(720);
            if (!values.contains("measure") && pixels != 720)
                values.insert("measure", PageLayout::measureFromPixels(pixels, values.value("font_size").toInt(19)));
            values.remove("line_width");
        }
        if (values.contains("spread")) {
            const QString spread = values.value("spread").toString();
            if (!values.contains("pages"))
                values.insert("pages", spread == "always" ? QJsonValue(2) : spread == "never" ? QJsonValue(1) : QJsonValue("auto"));
            values.remove("spread");
        }
    }
    // A file from a newer Omabook is read as it is: keys this one doesn't
    // know are kept and written back untouched.
    if (from < configVersion)
        values.insert("config_version", configVersion);
    return values;
}

void Settings::save() {
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile file(m_file);
    // No rewriting in place: a folder that can't hold the temp file fails the
    // save instead of risking a truncated config.
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(upgraded(m_values)).toJson()) > 0)
        file.commit();
    watch();
}
