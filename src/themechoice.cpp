#include "themechoice.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <array>
#include <cstdio>

#include "customization.h"

namespace {

using namespace Qt::StringLiterals;

// The names Omabook draws with, in the order a colors.toml it writes lists them.
constexpr std::array<QLatin1StringView, 12> names{
    "accent"_L1, "background"_L1, "foreground"_L1, "selection"_L1, "muted"_L1, "red"_L1,
    "green"_L1,  "yellow"_L1,     "orange"_L1,     "blue"_L1,      "magenta"_L1, "cyan"_L1};

QString readLine(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readLine()).trimmed() : QString();
}

QString colorsIn(const QString &folder) { return folder + "/colors.toml"; }
QString omarchyFile(const ThemePlaces &places) { return places.omarchy + "/theme/colors.toml"; }
QString aetherInOmarchy(const ThemePlaces &places) { return colorsIn(places.omarchyThemes + "/aether"); }
QString aetherOwn(const ThemePlaces &places) { return places.aether + "/theme/colors.toml"; }

bool isPath(const QString &choice) {
    return choice.contains('/') || choice.startsWith('~') || choice.endsWith(".toml"_L1);
}

// A path as style.json may write it: from the home folder, from Omabook's
// folder, or absolute; a folder stands for the colors.toml in it.
QString pathOf(const QString &choice, const ThemePlaces &places) {
    QString path = choice;
    if (path == "~"_L1 || path.startsWith("~/"_L1))
        path = QDir::homePath() + path.mid(1);
    if (QFileInfo(path).isRelative())
        path = QDir(QFileInfo(places.user).absolutePath()).filePath(path);
    if (QFileInfo(path).isDir())
        path = colorsIn(path);
    return QDir::cleanPath(path);
}

QStringList swatch(const Theme::Palette &palette) {
    QStringList colors;
    for (const char *name : {"background", "foreground", "accent", "red", "green", "yellow", "blue", "magenta"})
        colors << palette.colors.value(name).name();
    return colors;
}

// A colour moved in lightness, as Aether's lighten and darken do it.
QColor lightened(const QColor &color, double by) {
    const QColor hsl = color.toHsl();
    return QColor::fromHslF(hsl.hslHueF(), hsl.hslSaturationF(), std::clamp(hsl.lightnessF() + by, 0.0, 1.0)).toRgb();
}
QColor scaled(const QColor &color, double by) {
    const QColor hsl = color.toHsl();
    return QColor::fromHslF(hsl.hslHueF(), hsl.hslSaturationF(), std::clamp(hsl.lightnessF() * by, 0.0, 1.0)).toRgb();
}

bool writeFile(const QString &path, const QByteArray &contents) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
}

QVariantMap option(const QString &choice, const ThemeSource &source, const ThemePlaces &places) {
    const Theme::Palette palette = Themes::palette(source.path, places);
    return {{"choice", choice},     {"kind", choice == "auto"_L1 ? u"auto"_s : source.kind},
            {"id", source.id},      {"name", source.name},
            {"light", palette.light}, {"swatch", swatch(palette)},
            {"source", source.toMap()}};
}

} // namespace

ThemePlaces ThemePlaces::here(const QString &omabookFolder) {
    const QString home = QDir::homePath();
    return {home + "/.local/state/omarchy/current", home + "/.config/omarchy/themes",
            userConfigFolder(QProcessEnvironment::systemEnvironment()) + "/aether", omabookFolder + "/themes"};
}

QString ThemePlaces::userConfigFolder(const QProcessEnvironment &environment) {
#if defined(Q_OS_WIN)
    return QDir::fromNativeSeparators(environment.value("AppData"));
#elif defined(Q_OS_MACOS)
    return environment.value("HOME", QDir::homePath()) + "/Library/Application Support";
#else
    // Go takes $XDG_CONFIG_HOME only when it is an absolute path.
    const QString xdg = environment.value("XDG_CONFIG_HOME");
    if (!xdg.isEmpty() && QDir::isAbsolutePath(xdg))
        return xdg;
    return environment.value("HOME", QDir::homePath()) + "/.config";
#endif
}

QJsonObject ThemeSource::toJson() const {
    QJsonObject object{{"choice", choice}, {"kind", kind}, {"id", id}, {"name", name}, {"path", path}, {"why", why}};
    if (!note.isEmpty())
        object.insert("note", note);
    return object;
}

QString Themes::displayName(const QString &id) {
    QStringList words = id.split(QRegularExpression("[-_ ]+"), Qt::SkipEmptyParts);
    for (QString &word : words)
        word[0] = word[0].toUpper();
    return words.join(' ');
}

QString Themes::idOf(const QString &name) {
    return name.trimmed().toLower().split(QRegularExpression("[\\s_]+"), Qt::SkipEmptyParts).join('-');
}

ThemeSource resolveTheme(const QString &choice, const ThemePlaces &places) {
    const QString asked = choice.trimmed().isEmpty() ? u"auto"_s : choice.trimmed();
    const QString keyword = asked.toLower();
    auto bundled = [&places](const QString &id, const QString &why, const QString &note = {}) {
        return ThemeSource{id, "bundled", id, Themes::displayName(id), colorsIn(places.bundled + "/" + id), why, note};
    };
    auto fallback = [&](const QString &note) {
        ThemeSource source = bundled(QString(Themes::fallbackId), note, note);
        source.choice = keyword == "omarchy"_L1 || keyword == "aether"_L1 || keyword == "auto"_L1 ? keyword : asked;
        return source;
    };
    // Omarchy's current theme, by the name Omarchy gives it.
    auto omarchy = [&](const QString &why) {
        const QString id = readLine(places.omarchy + "/theme.name");
        return ThemeSource{"omarchy", "omarchy", id.isEmpty() ? u"omarchy"_s : id,
                           id.isEmpty() ? u"Omarchy"_s : Themes::displayName(id), omarchyFile(places), why, {}};
    };
    const bool omarchyHere = QFileInfo::exists(omarchyFile(places));

    if (keyword == "auto"_L1) {
        if (omarchyHere) {
            ThemeSource source = omarchy("Automatic: Omarchy is on this computer, so its current theme.");
            source.choice = "auto";
            return source;
        }
        if (QFileInfo::exists(aetherOwn(places)))
            return {"auto", "aether", "aether", "Aether", aetherOwn(places),
                    "Automatic: Aether's theme is on this computer and Omarchy isn't.", {}};
        ThemeSource source = bundled(QString(Themes::fallbackId),
                                     "Automatic: neither Omarchy nor Aether is on this computer, so Omarchy's default theme.");
        source.choice = "auto";
        return source;
    }
    if (keyword == "omarchy"_L1)
        return omarchyHere ? omarchy("Chosen: Omarchy's current theme, followed as it changes.")
                           : fallback("Omarchy isn't on this computer, so Omabook uses Tokyo Night.");
    if (keyword == "aether"_L1) {
        // On Omarchy, Aether writes a theme into Omarchy and switches to it;
        // elsewhere it writes theme/colors.toml in its own folder.
        const QString why = "Chosen: Aether's palette, followed as Aether changes it.";
        if (omarchyHere && readLine(places.omarchy + "/theme.name") == "aether"_L1)
            return {"aether", "aether", "aether", "Aether", omarchyFile(places), why, {}};
        for (const QString &path : {aetherInOmarchy(places), aetherOwn(places)})
            if (QFileInfo::exists(path))
                return {"aether", "aether", "aether", "Aether", path, why, {}};
        return fallback("Aether hasn't written a theme on this computer, so Omabook uses Tokyo Night.");
    }
    if (isPath(asked)) {
        const QString path = pathOf(asked, places);
        if (!QFileInfo(path).isFile())
            return fallback("There is no colors.toml at " + path + ", so Omabook uses Tokyo Night.");
        const QFileInfo file(path);
        const QString id = file.baseName() == "colors"_L1 ? file.dir().dirName() : file.baseName();
        return {asked, "file", id, Themes::displayName(id), path, "Chosen: the colors.toml at " + path + ".", {}};
    }
    const QString id = Themes::idOf(asked);
    if (QFileInfo::exists(colorsIn(places.user + "/" + id)))
        return {id, "user", id, Themes::displayName(id), colorsIn(places.user + "/" + id),
                "Chosen: a theme of your own, in " + places.user + "/" + id + ".", {}};
    if (QFileInfo::exists(colorsIn(places.bundled + "/" + id)))
        return bundled(id, "Chosen: one of Omarchy's themes, built into Omabook.");
    return fallback("There is no theme called \"" + asked + "\", so Omabook uses Tokyo Night.");
}

QString Themes::problem(const QString &choice, const ThemePlaces &places) {
    const QString keyword = choice.trimmed().toLower();
    if (keyword.isEmpty() || keyword == "auto"_L1 || keyword == "omarchy"_L1 || keyword == "aether"_L1)
        return {};
    if (isPath(choice.trimmed())) {
        const QString path = pathOf(choice.trimmed(), places);
        return QFileInfo(path).isFile() ? QString() : "There is no colors.toml at " + path + ".";
    }
    const QString id = idOf(choice);
    if (QFileInfo::exists(colorsIn(places.user + "/" + id)) || QFileInfo::exists(colorsIn(places.bundled + "/" + id)))
        return {};
    return "There is no theme called \"" + choice.trimmed()
           + "\": write \"auto\", \"omarchy\", \"aether\", a name from `omabook theme list`, or the path of a colors.toml.";
}

double Themes::contrast(const QColor &a, const QColor &b) noexcept {
    // Held by value: std::minmax of temporaries would hand back dangling references.
    const double first = relativeLuminance(a), second = relativeLuminance(b);
    return (std::max(first, second) + .05) / (std::min(first, second) + .05);
}

Theme::Palette Themes::readable(Theme::Palette palette) {
    const auto &colors = palette.colors;
    if (!colors.contains("muted") || !colors.contains("background") || !colors.contains("foreground"))
        return palette;
    const QColor muted = colors.value("muted"), background = colors.value("background"), foreground = colors.value("foreground");
    const QColor selection = colors.value("selection", background);
    auto reads = [&](const QColor &color) { return contrast(color, background) >= 4.5 && contrast(color, selection) >= 3.0; };
    if (reads(muted))
        return palette;
    // As little of the foreground as it takes, so the theme's tint stays.
    for (int step = 1; step <= 40; ++step) {
        const double t = step / 40.0;
        // Judged as it will be drawn: in whole steps of 1/255.
        const QColor mixed(QColor::fromRgbF(float(muted.redF() + (foreground.redF() - muted.redF()) * t),
                                            float(muted.greenF() + (foreground.greenF() - muted.greenF()) * t),
                                            float(muted.blueF() + (foreground.blueF() - muted.blueF()) * t)).name());
        if (reads(mixed) || step == 40) {
            palette.colors.insert("muted", mixed);
            break;
        }
    }
    return palette;
}

Theme::Palette Themes::palette(const QString &path, const ThemePlaces &places) {
    Theme::Palette merged = Theme::readThemeColors(colorsIn(places.bundled + "/" + QString(fallbackId)));
    if (!path.isEmpty()) {
        const Theme::Palette chosen = Theme::readThemeColors(path);
        merged.colors.insert(chosen.colors);
        if (!chosen.colors.isEmpty())
            merged.light = chosen.light;
    }
    return readable(merged);
}

QVariantList Themes::list(const ThemePlaces &places) {
    // The user's own shadow a bundled theme of the same name.
    QMap<QString, QVariantMap> found;
    for (const auto &[folder, kind] : {std::pair{places.bundled, u"bundled"_s}, std::pair{places.user, u"user"_s}}) {
        for (const QString &id : QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = colorsIn(folder + "/" + id);
            if (!QFileInfo::exists(path))
                continue;
            const Theme::Palette palette = Themes::palette(path, places);
            found.insert(displayName(id).toLower(), {{"id", id}, {"name", displayName(id)}, {"kind", kind},
                                                     {"path", path}, {"light", palette.light}, {"swatch", swatch(palette)}});
        }
    }
    QVariantList themes;
    for (const QVariantMap &theme : std::as_const(found))
        themes << theme;
    return themes;
}

bool Themes::omarchyHere(const ThemePlaces &places) { return QFileInfo::exists(omarchyFile(places)); }

bool Themes::aetherHere(const ThemePlaces &places) {
    return QFileInfo::exists(aetherInOmarchy(places)) || QFileInfo::exists(aetherOwn(places))
           || readLine(places.omarchy + "/theme.name") == "aether"_L1;
}

QString Themes::writeChoice(const QString &folder, const QString &choice, const ThemePlaces &places) {
    if (const QString why = problem(choice, places); !why.isEmpty())
        return why;
    const QString path = folder + "/style.json";
    QJsonObject style;
    if (QFile file(path); file.exists()) {
        if (!file.open(QIODevice::ReadOnly))
            return path + " can't be read.";
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            return "style.json has a mistake in it (see style.error); fix that first.";
        style = document.object();
    }
    const QString keyword = choice.trimmed().toLower();
    const QString kept = keyword.isEmpty() || keyword == "auto"_L1               ? QString()
                         : keyword == "omarchy"_L1 || keyword == "aether"_L1     ? keyword
                         : isPath(choice.trimmed())                             ? choice.trimmed()
                                                                                : idOf(choice);
    if (kept.isEmpty())
        style.remove("theme");
    else
        style.insert("theme", kept);
    if (!writeFile(path, QJsonDocument(style).toJson()))
        return path + " can't be written.";
    return {};
}

Theme::Palette Themes::fromBlueprint(const QJsonObject &blueprint, QString *problem) {
    const QJsonObject palette = blueprint.value("palette").toObject();
    const QJsonArray given = palette.value("colors").toArray();
    QList<QColor> colors;
    for (const QJsonValue &value : given)
        if (QColor::isValidColorName(value.toString()))
            colors << QColor(value.toString());
    if (colors.size() < 16) {
        if (problem)
            *problem = QString("An Aether blueprint has 16 colors in its palette; this one has %1.").arg(colors.size());
        return {};
    }
    Theme::Palette result;
    result.light = palette.value("mode").toString() == "light"_L1 || palette.value("lightMode").toBool();
    auto &out = result.colors;
    out.insert("background", colors[0]);
    for (int slot = 1; const char *hue : {"red", "green", "yellow", "blue", "magenta", "cyan"})
        out.insert(hue, colors[slot++]);
    out.insert("foreground", colors[7]);
    out.insert("muted", colors[8]);
    const QJsonObject extended = palette.value("extendedColors").toObject();
    auto extendedColor = [&extended](const char *name) {
        const QString value = extended.value(name).toString();
        return QColor::isValidColorName(value) ? QColor(value) : QColor();
    };
    out.insert("accent", extendedColor("accent").isValid() ? extendedColor("accent") : colors[4]);
    // Aether's default selection is the foreground and background swapped,
    // which would turn a selected row inside out; it then derives one.
    const QColor selectionBackground = extendedColor("selection_background"),
                 selectionForeground = extendedColor("selection_foreground");
    const bool inverted = !selectionBackground.isValid()
                          || (selectionBackground == colors[7] && (!selectionForeground.isValid() || selectionForeground == colors[0]));
    out.insert("selection", !inverted ? selectionBackground : result.light ? scaled(colors[0], .85) : lightened(colors[0], .10));
    out.insert("orange", lightened(colors[1], .15));
    const QJsonObject native = palette.value("nativeColors").toObject();
    for (const QLatin1StringView name : names)
        if (QColor::isValidColorName(native.value(name).toString()))
            out.insert(name, QColor(native.value(name).toString()));
    return result;
}

QByteArray Themes::toToml(const Theme::Palette &palette) {
    QByteArray toml = "mode = \"" + QByteArray(palette.light ? "light" : "dark") + "\"\n\n";
    for (const QLatin1StringView name : names)
        if (palette.colors.contains(name))
            toml += QByteArray(name.data(), name.size()) + " = \"" + palette.colors.value(name).name().toLatin1() + "\"\n";
    return toml;
}

QString Themes::importBlueprint(const QString &blueprint, const ThemePlaces &places, QString *problem) {
    auto failed = [problem](const QString &why) {
        if (problem)
            *problem = why;
        return QString();
    };
    QString file = QFileInfo(blueprint).isFile() ? blueprint : QString();
    const QString wanted = idOf(blueprint);
    // By its name: the file's, or the one written inside it.
    for (const QFileInfo &entry : QDir(places.aether + "/blueprints").entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
        if (!file.isEmpty())
            break;
        QFile candidate(entry.filePath());
        const QString named = candidate.open(QIODevice::ReadOnly)
                                  ? QJsonDocument::fromJson(candidate.readAll()).object().value("name").toString()
                                  : QString();
        if (idOf(entry.completeBaseName()) == wanted || (!named.isEmpty() && idOf(named) == wanted))
            file = entry.filePath();
    }
    if (file.isEmpty())
        return failed("There is no Aether blueprint called \"" + blueprint + "\" in " + places.aether + "/blueprints.");
    QFile opened(file);
    if (!opened.open(QIODevice::ReadOnly))
        return failed(file + " can't be read.");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(opened.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return failed(QFileInfo(file).fileName() + " isn't an Aether blueprint: " + error.errorString() + ".");
    QString why;
    const Theme::Palette palette = fromBlueprint(document.object(), &why);
    if (!why.isEmpty())
        return failed(why);
    const QString name = document.object().value("name").toString();
    const QString id = idOf(name.isEmpty() ? QFileInfo(file).completeBaseName() : name);
    if (id.isEmpty() || id.contains('/') || id.startsWith('.'))
        return failed("The blueprint's name can't be a folder's name.");
    if (!writeFile(colorsIn(places.user + "/" + id), toToml(palette)))
        return failed(colorsIn(places.user + "/" + id) + " can't be written.");
    return id;
}

ThemeChoice::ThemeChoice(Customization *custom, const ThemePlaces &places, QObject *parent)
    : QObject(parent), m_custom(custom), m_places(places) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &ThemeChoice::reload);
    connect(custom, &Customization::styleChanged, this, &ThemeChoice::reload);
    reload();
}

// The same rules as Theme's: a theme switch replaces files and folders, so
// everything is watched anew after every change, and the nearest folder
// that exists is watched too, so Omarchy or Aether installed while Omabook
// runs is seen at once.
void ThemeChoice::watch() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QStringList files{omarchyFile(m_places), m_places.omarchy + "/theme.name", aetherInOmarchy(m_places),
                      aetherOwn(m_places), m_places.user + "/colors.toml"};
    if (m_source.kind == "file"_L1 || m_source.kind == "user"_L1)
        files << m_source.path;
    QStringList paths;
    for (const QString &file : std::as_const(files)) {
        const QString folder = QFileInfo(file).absolutePath();
        QString ancestor = folder;
        while (!QFileInfo::exists(ancestor) && ancestor != "/"_L1 && !ancestor.isEmpty())
            ancestor = QFileInfo(ancestor).absolutePath();
        paths << ancestor << QFileInfo(ancestor).absolutePath() << QFileInfo(folder).absolutePath() << folder << file;
    }
    paths.removeDuplicates();
    for (const QString &path : std::as_const(paths))
        if (!path.startsWith(':') && path != "/"_L1 && QFileInfo::exists(path))
            m_watcher.addPath(path);
}

void ThemeChoice::reload() {
    const QString chosen = m_custom->style().value("theme").toString().trimmed();
    const QString choice = chosen.isEmpty() ? u"auto"_s : chosen;
    const ThemeSource source = resolveTheme(m_previewing.isEmpty() ? choice : m_previewing, m_places);
    const Theme::Palette palette = Themes::palette(source.path, m_places);
    const bool differs = choice != m_choice || !(source == m_source) || palette.colors != m_palette.colors
                         || palette.light != m_palette.light;
    m_choice = choice;
    m_source = source;
    m_palette = palette;
    watch();

    QVariantList options{option("auto", resolveTheme("auto", m_places), m_places)};
    if (Themes::omarchyHere(m_places))
        options << option("omarchy", resolveTheme("omarchy", m_places), m_places);
    if (Themes::aetherHere(m_places))
        options << option("aether", resolveTheme("aether", m_places), m_places);
    for (const QVariant &value : Themes::list(m_places)) {
        const QVariantMap theme = value.toMap();
        options << option(theme.value("id").toString(), resolveTheme(theme.value("id").toString(), m_places), m_places);
    }
    if (options != m_options) {
        m_options = options;
        emit optionsChanged();
    }
    if (differs)
        emit changed();
}

void ThemeChoice::preview(const QString &choice) {
    const QString wanted = choice.trimmed() == m_choice ? QString() : choice.trimmed();
    if (wanted == m_previewing)
        return;
    m_previewing = wanted;
    // Said even when it looks the same, since what is previewed changed.
    m_source = {};
    reload();
}

void ThemeChoice::endPreview() { preview({}); }

QString ThemeChoice::choose(const QString &choice) {
    if (const QString why = Themes::writeChoice(m_custom->folder(), choice, m_places); !why.isEmpty())
        return why;
    m_previewing.clear();
    // Read at once rather than when the watcher says so, so what was chosen
    // is in effect when this returns.
    m_custom->check();
    reload();
    return {};
}

// The command line: `omabook theme [show|list|use <choice>|import <blueprint>]`.
namespace {

void print(FILE *stream, const QString &text) {
    fprintf(stream, "%s\n", qPrintable(text));
    fflush(stream);
}

void printJson(const QJsonObject &object) {
    print(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

QString describe(const ThemeSource &source) {
    const QString kind = source.kind == "omarchy"_L1 ? u"Omarchy's current theme"_s
                         : source.kind == "aether"_L1 ? u"Aether's palette"_s
                         : source.kind == "bundled"_L1 ? u"built in"_s
                         : source.kind == "user"_L1    ? u"your own"_s
                                                       : source.path;
    return source.name + " (" + kind + ")";
}

} // namespace

int runThemeCli(const QStringList &given) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        "Which theme Omabook's colors come from.\n\n"
        "  omabook theme [show]              The theme in use, and why\n"
        "  omabook theme list                The themes there are to choose from\n"
        "  omabook theme use <choice>        auto, omarchy, aether, a name from the list, or a colors.toml\n"
        "  omabook theme import <blueprint>  An Aether blueprint, by name or file, as a theme of your own\n\n"
        "The choice is \"theme\" in ~/.config/omabook/style.json; the colors written there win over it.");
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library", "Ignored: themes are the same for every library.", "folder"});
    parser.addPositionalArgument("action", "show, list, use or import.", "[action]");
    parser.addPositionalArgument("argument", "What to use, or the blueprint to import.", "[argument]");
    QStringList arguments = given;
    arguments.removeAt(1);
    arguments[0] = "omabook theme";
    parser.process(arguments);
    const QStringList words = parser.positionalArguments();
    const QString action = words.value(0, "show");
    const bool json = parser.isSet("json");
    auto fail = [](const QString &why) {
        print(stderr, why);
        return 1;
    };

    Customization custom;
    const ThemePlaces places = ThemePlaces::here(custom.folder());
    const QString chosen = custom.style().value("theme").toString();

    if (action == "show"_L1 && words.size() <= 1) {
        const ThemeSource source = resolveTheme(chosen, places);
        if (json) {
            QJsonObject colors;
            const Theme::Palette palette = Themes::palette(source.path, places);
            for (auto it = palette.colors.begin(); it != palette.colors.end(); ++it)
                colors.insert(it.key(), it.value().name());
            QJsonObject object = source.toJson();
            object.insert("light", palette.light);
            object.insert("colors", colors);
            printJson(object);
        } else {
            print(stdout, "theme:\t" + describe(source));
            print(stdout, "\t" + source.why);
            if (!source.path.startsWith(':'))
                print(stdout, "\t" + source.path);
        }
        return 0;
    }
    if (action == "list"_L1 && words.size() == 1) {
        ThemeChoice choice(&custom, places);
        QJsonArray themes;
        QStringList lines;
        for (const QVariant &value : choice.options()) {
            const QVariantMap option = value.toMap();
            const ThemeSource source = resolveTheme(option.value("choice").toString(), places);
            const bool current = option.value("choice").toString() == choice.choice();
            themes << QJsonObject{{"choice", option.value("choice").toString()}, {"kind", option.value("kind").toString()},
                                  {"name", option.value("name").toString()}, {"light", option.value("light").toBool()},
                                  {"current", current}, {"source", source.toJson()}};
            const QString shown = option.value("choice") == "auto"_L1      ? "Automatic: " + describe(source)
                                  : option.value("choice") == "omarchy"_L1 ? "Omarchy: " + source.name
                                  : option.value("choice") == "aether"_L1  ? u"Aether"_s
                                                                           : source.name + (source.kind == "user"_L1 ? " (your own)" : "");
            lines << QString("%1 %2\t%3").arg(current ? "*" : " ", option.value("choice").toString().leftJustified(18), shown);
        }
        QJsonArray blueprints;
        for (const QFileInfo &entry : QDir(places.aether + "/blueprints").entryInfoList({"*.json"}, QDir::Files, QDir::Name))
            blueprints << entry.completeBaseName();
        if (json)
            printJson(QJsonObject{{"choice", choice.choice()}, {"themes", themes}, {"blueprints", blueprints}});
        else
            print(stdout, lines.join('\n'));
        return 0;
    }
    if (action == "use"_L1 && words.size() == 2) {
        if (const QString why = Themes::writeChoice(custom.folder(), words.at(1), places); !why.isEmpty())
            return fail(why);
        custom.check();
        const ThemeSource source = resolveTheme(custom.style().value("theme").toString(), places);
        if (json)
            printJson(source.toJson());
        else
            print(stdout, "Omabook's colors now come from " + describe(source) + (source.note.isEmpty() ? "." : ". " + source.note));
        return 0;
    }
    if (action == "import"_L1 && words.size() == 2) {
        QString why;
        const QString id = Themes::importBlueprint(words.at(1), places, &why);
        if (id.isEmpty())
            return fail(why);
        if (json)
            printJson(QJsonObject{{"id", id}, {"name", Themes::displayName(id)}, {"path", places.user + "/" + id + "/colors.toml"}});
        else
            print(stdout, "Imported as \"" + id + "\": `omabook theme use " + id + "` to use it.");
        return 0;
    }
    print(stderr, parser.helpText().trimmed());
    return 1;
}
