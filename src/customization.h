#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

class Theme;

// Omabook's interface as files. Everything in ~/.config/omabook/ that isn't
// the settings is read here and followed while the app runs:
//
//   keys.json    which key does what, per place ("library", "reader", "global")
//   style.json   colors, corners and the interface font, over the desktop's theme
//   ui/*.qml     a view of your own in place of the built-in one of that name
//   mods/<name>  a panel of your own (mod.json + Main.qml) in a named place
//
// A file that can't be used leaves the last good state in place and says why
// in <name>.error beside it, so a typo never takes the window down.
class Customization : public QObject {
    Q_OBJECT
    // Command to its keys: those pressed alone ("A", "Space"), which are off
    // while typing, and those with a modifier or a function key, which aren't.
    Q_PROPERTY(QVariantMap plainKeys READ plainKeys NOTIFY keysChanged)
    Q_PROPERTY(QVariantMap chordKeys READ chordKeys NOTIFY keysChanged)
    // Command to its keys as the help shows them: "Space  PgDown".
    Q_PROPERTY(QVariantMap keyText READ keyText NOTIFY keysChanged)
    Q_PROPERTY(QVariantMap style READ style NOTIFY styleChanged)
    Q_PROPERTY(QVariantList mods READ mods NOTIFY modsChanged)
    // The commands script plugins added, each {id, title, place, key, plugin}.
    Q_PROPERTY(QVariantList pluginCommands READ pluginCommands NOTIFY keysChanged)

public:
    struct Command {
        QString id;         // "reader.page_down"
        QStringList keys;   // the keys it has when keys.json says nothing
        QString what;       // what it does, in English, for AGENTS.md
    };
    static const QList<Command> &commands();

    // Tests pass a temp folder.
    explicit Customization(const QString &folder = {}, QObject *parent = nullptr);

    QString folder() const { return m_folder; }
    QVariantMap plainKeys() const { return m_plain; }
    QVariantMap chordKeys() const { return m_chord; }
    QVariantMap keyText() const { return m_text; }
    QVariantMap style() const { return m_style; }
    QVariantList mods() const { return m_mods + m_pluginMods; }
    // Those that apply now: one a plugin switched off, or that is only for
    // the plugin's own books while another is in front of the reader, is
    // not offered (it keeps its key, which then does nothing).
    QVariantList pluginCommands() const;
    // What script plugins bring to the interface (see scriptplugins.h):
    // their commands join the built-in ones, each with the key it asked for
    // unless something has it, and can be bound in keys.json; their panels
    // join the mods.
    void setPlugins(const QVariantList &commands, const QVariantList &panels);
    // The views replaced by files in ui/, by name ("ReaderView.qml").
    QStringList views() const { return m_views; }

    // Where the window should be loaded from: the built-in views, or a folder
    // of them with the user's own laid over. Empty `problem` on success.
    QUrl windowUrl(QString *problem = nullptr) const;
    // Says a view of the user's could not be loaded, in ui.error.
    void reportUi(const QString &problem) const;

    // Writes AGENTS.md, and the built-in view of that name into ui/ to start
    // from when one is named. Returns what was written, or an empty string
    // and `problem`.
    QString writeGuide() const;
    QString exportView(const QString &name, QString *problem) const;
    static QStringList builtInViews();

    // Reads the files now, without waiting to be told they changed, and
    // says what is wrong with keys.json or style.json, if anything is.
    QString check();

signals:
    void keysChanged();
    void styleChanged();
    void modsChanged();
    // A view in ui/ changed: the window should be loaded again.
    void viewsChanged();

private:
    void reload();
    void loadKeys();
    void loadStyle();
    void loadMods();
    void loadViews();
    void report(const QString &name, const QString &problem) const;
    // The built-in commands and the plugins', in that order.
    QList<Command> allCommands() const;

    QString m_folder;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QVariantMap m_plain, m_chord, m_text, m_style;
    QVariantList m_mods, m_pluginMods, m_pluginCommands;
    QStringList m_views;
    QByteArray m_viewsStamp;
    bool m_viewsSeen = false;
};

// The desktop's theme with style.json laid over it: the same face as Theme
// (accent, background, colors, radius, ...), which is what the views see as
// `theme`. With no style.json it is the theme, untouched.
class Look : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)

public:
    Look(Theme *theme, Customization *custom, QObject *parent = nullptr);

    QColor accent() const { return color("accent"); }
    QColor accentForeground() const;
    QColor background() const { return color("background"); }
    QColor foreground() const { return color("foreground"); }
    QVariantMap colors() const;
    bool light() const;
    int radius() const;

signals:
    void changed();

private:
    QColor color(const QString &key) const;

    Theme *m_theme;
    Customization *m_custom;
};
