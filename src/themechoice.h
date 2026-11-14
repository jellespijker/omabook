#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QProcessEnvironment>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

#include "theme.h"

class Customization;

// Which theme Omabook takes its palette from: style.json's "theme".
//
//   "auto"          Omarchy's current theme where Omarchy is, else what Aether
//                   writes where Aether is, else Omarchy's default, Tokyo Night
//   "omarchy"       Omarchy's current theme, followed live
//   "aether"        Aether's palette, followed live: the theme it writes into
//                   Omarchy on Omarchy, its own theme/colors.toml elsewhere
//   "tokyo-night"   one of Omarchy's themes, bundled, or one in
//                   ~/.config/omabook/themes/<name>/ (which wins)
//   "/a/colors.toml" any colors.toml (or the folder holding one), followed live
//
// Per colour, the user's own colours in style.json win over the chosen
// theme, which wins over bundled Tokyo Night, which wins over Theme's
// built-in fallback.

// Where the themes are. Tests pass a temp tree; nothing here reads the
// environment except here().
struct ThemePlaces {
    QString omarchy;        // Omarchy's current theme: theme/colors.toml and theme.name in it
    QString omarchyThemes;  // Omarchy's installed themes, where Aether puts its own on Omarchy
    QString aether;         // Aether's folder: theme/colors.toml without Omarchy, blueprints/
    QString user;           // Omabook's own themes: <name>/colors.toml
    QString bundled = QStringLiteral(":/themes");

    // This computer's, for Omabook's config folder (~/.config/omabook).
    static ThemePlaces here(const QString &omabookFolder);
    // The folder Go's os.UserConfigDir gives, which is where Aether keeps
    // its files: $XDG_CONFIG_HOME or ~/.config on Linux, ~/Library/Application
    // Support on macOS, %AppData% on Windows. Qt's idea differs on the last two.
    static QString userConfigFolder(const QProcessEnvironment &environment);
};

// What a choice comes to on this computer.
struct ThemeSource {
    QString choice;  // as asked, tidied: "auto", "omarchy", "aether", a theme's id, or a path
    QString kind;    // "omarchy", "aether", "bundled", "user" or "file"
    QString id;      // the theme's folder name: "tokyo-night"
    QString name;    // as shown: "Tokyo Night"
    QString path;    // the colors.toml it reads
    QString why;     // in a sentence, for `omabook info` and the picker
    QString note;    // empty, or why it isn't what was asked for

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] QVariantMap toMap() const { return toJson().toVariantMap(); }
    bool operator==(const ThemeSource &) const = default;
};

// The pure part: what `choice` resolves to with these places. A choice that
// can't be had (Omarchy isn't here, a theme of that name isn't) falls back to
// the bundled default and says so in `note`.
[[nodiscard]] ThemeSource resolveTheme(const QString &choice, const ThemePlaces &places);

namespace Themes {

inline constexpr QLatin1StringView fallbackId{"tokyo-night"};

// "last-horizon" as it is shown: "Last Horizon".
[[nodiscard]] QString displayName(const QString &id);
// "Tokyo Night" or "tokyo-night " as a theme's id: "tokyo-night".
[[nodiscard]] QString idOf(const QString &name);
// Whether style.json may say this; empty when it may, else why not.
[[nodiscard]] QString problem(const QString &choice, const ThemePlaces &places);

// A theme's palette laid over the bundled default, with its muted text made
// readable: Omarchy's themes use "muted" for borders and comments, at about
// 2:1, and Omabook sets secondary text in it, so it is moved towards the
// foreground until it reads (4.5:1 on the background, 3:1 on the selection).
[[nodiscard]] Theme::Palette palette(const QString &path, const ThemePlaces &places);
[[nodiscard]] Theme::Palette readable(Theme::Palette palette);
[[nodiscard]] double contrast(const QColor &a, const QColor &b) noexcept;

// The themes there are to choose from: the bundled ones and the user's,
// each {id, name, kind, light, swatch: [background, foreground, accent, red,
// green, yellow, blue, magenta]}, by name.
[[nodiscard]] QVariantList list(const ThemePlaces &places);
[[nodiscard]] bool omarchyHere(const ThemePlaces &places);
[[nodiscard]] bool aetherHere(const ThemePlaces &places);

// Writes "theme" into style.json in `folder`, keeping the rest of it; "auto"
// takes the key out. Empty on success, else why not.
[[nodiscard]] QString writeChoice(const QString &folder, const QString &choice, const ThemePlaces &places);

// An Aether blueprint (~/.config/aether/blueprints/<name>.json) as a theme of
// Omabook's own, in <user>/<id>/colors.toml. `blueprint` is a file or a
// blueprint's name. Returns the new theme's id, or empty and `problem`.
[[nodiscard]] QString importBlueprint(const QString &blueprint, const ThemePlaces &places, QString *problem);
// The mapping itself, Aether's own rules: colors[0] is the background,
// [1..6] red to cyan, [7] the foreground, [8] muted, the accent its own or
// blue, the selection its own or the background moved a little, orange red
// lightened, and any nativeColors entry of the same name wins.
[[nodiscard]] Theme::Palette fromBlueprint(const QJsonObject &blueprint, QString *problem);
// A palette as a colors.toml Omabook (and Omarchy) reads.
[[nodiscard]] QByteArray toToml(const Theme::Palette &palette);

} // namespace Themes

// The choice, followed: style.json's "theme" as it changes, and the file it
// resolves to as Omarchy or Aether rewrite it. Also what the picker previews
// before anything is written. Look lays the palette under style.json's own
// colours; the views see this as `theme.themes`.
class ThemeChoice : public QObject {
    Q_OBJECT
    // style.json's choice, "auto" when it has none.
    Q_PROPERTY(QString choice READ choice NOTIFY changed)
    // What the picker is showing instead, or "".
    Q_PROPERTY(QString previewing READ previewing NOTIFY changed)
    // What the choice resolves to now: ThemeSource's fields.
    Q_PROPERTY(QVariantMap source READ sourceMap NOTIFY changed)
    // What can be picked, in the order the picker shows it: "auto", then
    // "omarchy" and "aether" where they are, then every theme by name, each
    // {choice, kind, id, name, light, swatch, source}.
    Q_PROPERTY(QVariantList options READ options NOTIFY optionsChanged)

public:
    ThemeChoice(Customization *custom, const ThemePlaces &places, QObject *parent = nullptr);

    QString choice() const { return m_choice; }
    QString previewing() const { return m_previewing; }
    QVariantMap sourceMap() const { return m_source.toMap(); }
    const ThemeSource &source() const noexcept { return m_source; }
    QVariantList options() const { return m_options; }
    const Theme::Palette &palette() const noexcept { return m_palette; }
    const ThemePlaces &places() const noexcept { return m_places; }

    // Shows a choice without keeping it; endPreview() puts the kept one back.
    Q_INVOKABLE void preview(const QString &choice);
    Q_INVOKABLE void endPreview();
    // Keeps a choice, in style.json. Empty on success, else why not.
    Q_INVOKABLE QString choose(const QString &choice);

signals:
    void changed();
    void optionsChanged();

private:
    void reload();
    void watch();

    Customization *m_custom;
    ThemePlaces m_places;
    QString m_choice = QStringLiteral("auto");
    QString m_previewing;
    ThemeSource m_source;
    Theme::Palette m_palette;
    QVariantList m_options;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};

// `omabook theme`: list, use, show, import. See themechoice.cpp.
int runThemeCli(const QStringList &arguments);
