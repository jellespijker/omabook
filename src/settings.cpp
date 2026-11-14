#include "settings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>

Settings::Settings(const QString &file, QObject *parent)
    : QObject(parent),
      m_file(file.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omabook/config.json"
                            : file) {
    read();
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { read(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { read(); });
    watch();
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
    const QString path = m_values.value("library").toString();
    return path.isEmpty() ? QDir::homePath() + "/Books" : path;
}

void Settings::setLibraryPath(const QString &path) {
    const QString absolute = QDir(path).absolutePath();
    QStringList known = libraries();
    known.removeAll(absolute);
    known.prepend(absolute);
    m_values.insert("libraries", QJsonArray::fromStringList(known));
    set("library", absolute);
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
    return QStringList{"auto", "ask", "off"}.contains(mode) ? mode : QString("auto");
}

void Settings::setGraph(const QString &mode) {
    if (QStringList{"auto", "ask", "off"}.contains(mode))
        set("graph", mode);
}

void Settings::setPaneWidth(const QString &pane, int width) {
    QJsonObject widths = m_values.value("pane_widths").toObject();
    if (width <= 0)
        widths.remove(pane);
    else
        widths.insert(pane, qBound(120, width, 4000));
    if (widths != m_values.value("pane_widths").toObject())
        set("pane_widths", widths);
}

QJsonObject Settings::shelf(const QString &library) const {
    return m_values.value("shelves").toObject().value(library).toObject();
}

void Settings::setShelf(const QString &library, const QJsonObject &shelf) {
    QJsonObject shelves = m_values.value("shelves").toObject();
    shelves.insert(library, shelf);
    set("shelves", shelves);
}

QJsonObject Settings::options() const {
    return {{"cover_size", coverSize()}, {"font_size", fontSize()}, {"font_family", fontFamily()}, {"line_width", lineWidth()},
            {"line_height", lineHeight()}, {"paragraph_spacing", paragraphSpacing()}, {"justify", justify()},
            {"spread", spread()}, {"sort", sort()}};
}

bool Settings::setOption(const QString &key, const QJsonValue &value) {
    auto within = [&](int low, int high) { return value.isDouble() && value.toInt() >= low && value.toInt() <= high; };
    const bool fits = key == "cover_size" ? within(96, 480)
                      : key == "font_size" ? within(10, 48)
                      : key == "line_width" ? within(320, 1600)
                      : key == "line_height" ? within(100, 300)
                      : key == "paragraph_spacing" ? within(0, 80)
                      : key == "justify" ? value.isBool()
                      : key == "font_family" ? value.isString()
                      : key == "spread" ? QStringList{"auto", "always", "never"}.contains(value.toString())
                      : key == "plugin_updates" ? QStringList{"auto", "tell", "never"}.contains(value.toString())
                      : key == "sort" ? QStringList{"recent", "title", "author", "series", "added", "progress", "rating",
                                                    "published", "finished"}.contains(value.toString())
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
constexpr int configVersion = 1;

QJsonObject Settings::upgraded(QJsonObject values) {
    const int from = values.value("config_version").toInt(0);
    // if (from < 2) { …move the old key to the new one… }
    // A file from a newer Omabook is read as it is: keys this one doesn't
    // know are kept and written back untouched.
    if (from < configVersion)
        values.insert("config_version", configVersion);
    return values;
}

void Settings::save() const {
    QDir().mkpath(QFileInfo(m_file).absolutePath());
    QSaveFile file(m_file);
    // No rewriting in place: a folder that can't hold the temp file fails the
    // save instead of risking a truncated config.
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(upgraded(m_values)).toJson()) > 0)
        file.commit();
    const_cast<Settings *>(this)->watch();
}
