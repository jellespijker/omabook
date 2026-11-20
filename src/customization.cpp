#include "customization.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QStandardPaths>
#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <span>
#include <string_view>

#include "commandtable.h"
#include "scriptapi.h"
#include "theme.h"
#include "themechoice.h"

namespace {

using namespace Qt::StringLiterals;
using CommandTable::BuiltIn;
using CommandTable::builtIns;
using CommandTable::keysOf;
using CommandTable::places;

[[nodiscard]] bool isPlace(QStringView name) {
    return std::ranges::any_of(places, [name](std::string_view place) { return name == QLatin1StringView(place); });
}

// A key with Ctrl, Alt or Meta, or a function key, can't be typed into a
// text field, so it stays a shortcut while one has the keyboard.
bool isChord(const QString &key) {
    const QKeySequence sequence(key);
    if (sequence.isEmpty())
        return false;
    const QKeyCombination combination = sequence[0];
    const Qt::KeyboardModifiers held = combination.keyboardModifiers();
    const int code = combination.key();
    return (held & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) || (code >= Qt::Key_F1 && code <= Qt::Key_F35);
}

QString normal(const QString &key) { return QKeySequence(key).toString(QKeySequence::PortableText); }

// A file that should hold one JSON object: what it holds, whether it is
// there at all, and what is wrong with it when it can't be used.
struct ObjectFile {
    QJsonObject object;
    bool exists = false;
    QString problem;
};

[[nodiscard]] ObjectFile readObject(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    QString problem;
    if (error.error != QJsonParseError::NoError)
        problem = QString("It isn't valid JSON: %1 (at character %2).").arg(error.errorString()).arg(error.offset);
    else if (!document.isObject())
        problem = "It should be a JSON object.";
    return {document.object(), true, problem};
}

} // namespace

const QList<Customization::Command> &Customization::commands() {
    static const QList<Command> commands = builtIns | std::views::transform([](const BuiltIn &command) {
        const auto text = [](std::string_view utf8) { return QString::fromUtf8(utf8); };
        return Command{text(command.id), keysOf(command) | std::views::transform(text) | std::ranges::to<QStringList>(), text(command.what)};
    }) | std::ranges::to<QList<Command>>();
    return commands;
}

double relativeLuminance(const QColor &color) noexcept {
    const auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(color.redF()) + .7152 * linear(color.greenF()) + .0722 * linear(color.blueF());
}

Customization::Customization(const QString &folder, QObject *parent)
    : QObject(parent),
      m_folder(folder.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/omabook" : folder) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(120);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Customization::reload);
    reload();
}

// Editors replace files rather than write into them, which drops a file from
// the watch, so everything is watched anew after every change.
void Customization::reload() {
    const QStringList watched = m_watcher.files() + m_watcher.directories();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    QString ancestor = m_folder;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList paths{ancestor, m_folder, m_folder + "/keys.json", m_folder + "/style.json", m_folder + "/ui", m_folder + "/mods"};
    for (const QFileInfo &entry : QDir(m_folder + "/ui").entryInfoList({"*.qml"}, QDir::Files))
        paths << entry.filePath();
    for (const QFileInfo &entry : QDir(m_folder + "/mods").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        paths << entry.filePath() << entry.filePath() + "/mod.json" << entry.filePath() + "/Main.qml";
    paths.removeDuplicates();
    for (const QString &path : std::as_const(paths))
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    loadKeys();
    loadStyle();
    loadMods();
    loadViews();
}

QString Customization::check() {
    reload();
    QString problems;
    for (const auto name : {"keys"_L1, "style"_L1}) {
        QFile error(m_folder + "/" + name + ".error");
        if (error.open(QIODevice::ReadOnly))
            problems += QString::fromUtf8(error.readAll()).section('\n', 0, 0) + " ";
    }
    return problems.trimmed();
}

void Customization::report(const QString &name, const QString &problem) const {
    const QString path = m_folder + "/" + name + ".error";
    if (problem.isEmpty()) {
        QFile::remove(path);
        return;
    }
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write((problem + "\nOmabook is using the last version that worked.\n").toUtf8());
}

void Customization::reportUi(const QString &problem) const { report("ui", problem); }

// keys.json is {"reader": {"Space": "next", "N": ""}, "library": {...}, "global": {...}}:
// a key and the command it runs in that place, or "" to free the key. What
// the file doesn't mention keeps its default.
void Customization::loadKeys() {
    const QList<Command> known = allCommands();
    const auto order = known | std::views::transform(&Command::id) | std::ranges::to<QStringList>();
    // Every command with the keys it has when keys.json says nothing.
    const auto defaults = [&known] {
        QHash<QString, QStringList> bound;
        for (const Command &command : known)
            for (const QString &key : command.keys)
                bound[command.id] << normal(key);
        return bound;
    };
    QHash<QString, QStringList> bound = defaults();
    auto [file, exists, problem] = readObject(m_folder + "/keys.json");
    for (auto place = file.begin(); place != file.end() && problem.isEmpty(); ++place) {
        if (!isPlace(place.key()) || !place.value().isObject()) {
            problem = "\"" + place.key() + R"(" isn't a place keys work in: use "global", "library", "reader", "graph" or "chronology".)";
            break;
        }
        const QJsonObject keys = place.value().toObject();
        for (auto key = keys.begin(); key != keys.end(); ++key) {
            const QString sequence = normal(key.key());
            const QString name = key.value().toString();
            const QString command = name.isEmpty() ? QString() : name.contains('.') ? name : place.key() + "." + name;
            if (sequence.isEmpty()) {
                problem = "\"" + key.key() + "\" isn't a key Omabook knows (write it like \"Ctrl+F\", \"Shift+A\", \"PgDown\").";
                break;
            }
            if (!command.isEmpty() && !order.contains(command)) {
                problem = "There is no command \"" + command + "\" (see AGENTS.md for the list).";
                break;
            }
            // The key leaves whatever had it in that place, and everywhere when it is global.
            for (const QString &other : std::as_const(order))
                if (place.key() == "global" || other.startsWith(place.key() + ".") || other.startsWith("global."))
                    bound[other].removeAll(sequence);
            if (!command.isEmpty())
                bound[command] << sequence;
        }
    }
    report("keys", exists ? problem : QString());
    // A broken file changes nothing; with nothing loaded yet, the defaults stand.
    if (!problem.isEmpty() && !m_text.isEmpty())
        return;
    if (!problem.isEmpty())
        bound = defaults();
    QVariantMap plain, chord, text;
    for (const QString &command : std::as_const(order)) {
        QStringList plainKeys, chordKeys;
        for (const QString &key : bound.value(command))
            (isChord(key) ? chordKeys : plainKeys) << key;
        plain.insert(command, plainKeys);
        chord.insert(command, chordKeys);
        text.insert(command, bound.value(command).join("  "));
    }
    if (plain == m_plain && chord == m_chord)
        return;
    m_plain = plain;
    m_chord = chord;
    m_text = text;
    emit keysChanged();
}

QList<Customization::Command> Customization::allCommands() const {
    QList<Command> all = commands();
    for (const QVariant &value : m_pluginCommands) {
        const QVariantMap plugin = value.toMap();
        const QString place = plugin.value("place").toString();
        QStringList keys;
        const QString key = normal(plugin.value("key").toString());
        // A plugin asks for a key; it gets it when nothing in that place,
        // built in or added before it, has it by default.
        const bool taken = std::ranges::any_of(all, [&place, &key](const Command &other) {
            const bool samePlace = place == "global"_L1 || other.id.startsWith(place + ".") || other.id.startsWith("global."_L1);
            return samePlace && std::ranges::contains(other.keys, key, normal);
        });
        if (!key.isEmpty() && !taken)
            keys << key;
        all.append({plugin.value("id").toString(), keys, plugin.value("title").toString()});
    }
    return all;
}

QVariantList Customization::pluginCommands() const {
    return m_pluginCommands | std::views::filter([](const QVariant &command) { return command.toMap().value("applies", true).toBool(); })
           | std::ranges::to<QVariantList>();
}

void Customization::setPlugins(const QVariantList &commands, const QVariantList &panels) {
    if (panels != m_pluginMods) {
        m_pluginMods = panels;
        emit modsChanged();
    }
    if (commands == m_pluginCommands)
        return;
    m_pluginCommands = commands;
    const QVariantMap before = m_text;
    loadKeys();
    // The keys may be as they were while a command's title is not.
    if (before == m_text)
        emit keysChanged();
}

// style.json is {"theme": "auto", "colors": {"accent": "#c96", "background": ...},
// "mode": "light", "radius": 8, "font": "Inter", "font_size": 14}; every key is
// optional. "theme" is which palette the colors start from (see themechoice.h).
void Customization::loadStyle() {
    auto [file, exists, problem] = readObject(m_folder + "/style.json");
    QVariantMap style;
    QVariantMap colors;
    const QJsonObject wanted = file.value("colors").toObject();
    for (auto color = wanted.begin(); color != wanted.end() && problem.isEmpty(); ++color) {
        if (!QColor::isValidColorName(color.value().toString()))
            problem = "\"" + color.value().toString() + "\" for " + color.key() + " isn't a color (write it like \"#ffd60a\").";
        colors.insert(color.key(), QColor(color.value().toString()));
    }
    if (problem.isEmpty() && file.contains("mode") && !QStringList{"light", "dark"}.contains(file.value("mode").toString()))
        problem = "\"mode\" is \"light\" or \"dark\".";
    if (problem.isEmpty() && file.contains("theme"))
        problem = file.value("theme").isString()
                      ? Themes::problem(file.value("theme").toString(), ThemePlaces::here(m_folder))
                      : "\"theme\" is a name: \"auto\", \"omarchy\", \"aether\", one from `omabook theme list`, or a colors.toml.";
    report("style", exists ? problem : QString());
    if (!problem.isEmpty())
        return;
    if (!colors.isEmpty()) style.insert("colors", colors);
    if (!file.value("theme").toString().trimmed().isEmpty()) style.insert("theme", file.value("theme").toString().trimmed());
    if (file.contains("mode")) style.insert("mode", file.value("mode").toString());
    if (file.value("radius").isDouble()) style.insert("radius", std::clamp(file.value("radius").toInt(), 0, 40));
    if (!file.value("font").toString().isEmpty()) style.insert("font", file.value("font").toString());
    if (file.value("font_size").isDouble()) style.insert("font_size", std::clamp(file.value("font_size").toInt(), 8, 32));
    if (style == m_style)
        return;
    m_style = style;
    emit styleChanged();
}

// A mod is mods/<name>/ with a Main.qml and a mod.json:
//   {"place": "library.side" | "reader.side", "title": "...", "width": 280}
void Customization::loadMods() {
    QVariantList mods;
    for (const QFileInfo &entry : QDir(m_folder + "/mods").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const auto [manifest, exists, problem] = readObject(entry.filePath() + "/mod.json");
        const QString place = manifest.value("place").toString();
        if (!exists || !problem.isEmpty() || !QFileInfo::exists(entry.filePath() + "/Main.qml")
            || !QStringList{"library.side", "reader.side"}.contains(place))
            continue;
        // The file's time in the address makes an edited mod a new component.
        QUrl url = QUrl::fromLocalFile(entry.filePath() + "/Main.qml");
        url.setQuery(QString::number(QFileInfo(entry.filePath() + "/Main.qml").lastModified().toMSecsSinceEpoch()));
        mods << QVariantMap{{"name", entry.fileName()}, {"place", place}, {"title", manifest.value("title").toString(entry.fileName())},
                            {"width", std::clamp(manifest.value("width").toInt(280), 120, 900)}, {"url", url}};
    }
    if (mods == m_mods)
        return;
    m_mods = mods;
    emit modsChanged();
}

void Customization::loadViews() {
    QStringList views;
    QByteArray stamp;
    for (const QFileInfo &entry : QDir(m_folder + "/ui").entryInfoList({"*.qml"}, QDir::Files, QDir::Name)) {
        views << entry.fileName();
        stamp += entry.fileName().toUtf8() + QByteArray::number(entry.lastModified().toMSecsSinceEpoch()) + QByteArray::number(entry.size());
    }
    m_views = views;
    // The first look only notes what is there; after that, any difference
    // means the window should be loaded again.
    const bool changed = m_viewsSeen && stamp != m_viewsStamp;
    m_viewsSeen = true;
    m_viewsStamp = stamp;
    if (changed)
        emit viewsChanged();
}

QStringList Customization::builtInViews() { return QDir(":/").entryList({"*.qml"}, QDir::Files, QDir::Name); }

// With views of the user's own, the window is loaded from a folder holding
// every built-in view with theirs laid over, so each finds the others by
// name whichever of them were replaced.
QUrl Customization::windowUrl(QString *problem) const {
    if (m_views.isEmpty())
        return QUrl("qrc:/Main.qml");
    const QString merged = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/omabook/ui";
    QDir(merged).removeRecursively();
    bool ok = QDir().mkpath(merged);
    for (const QString &view : builtInViews())
        ok = ok && QFile::copy(":/" + view, merged + "/" + view);
    for (const QString &view : m_views) {
        QFile::remove(merged + "/" + view);
        ok = ok && QFile::copy(m_folder + "/ui/" + view, merged + "/" + view);
    }
    if (!ok) {
        if (problem)
            *problem = "The views can't be put together in " + merged + ".";
        return QUrl("qrc:/Main.qml");
    }
    return QUrl::fromLocalFile(merged + "/Main.qml");
}

QString Customization::exportView(const QString &name, QString *problem) const {
    const QString view = name.endsWith(".qml") ? name : name + ".qml";
    const QString target = m_folder + "/ui/" + view;
    if (!builtInViews().contains(view)) {
        *problem = "There is no built-in view " + view + "; there are: " + builtInViews().join(", ");
        return {};
    }
    if (QFileInfo::exists(target)) {
        *problem = "There already is a " + target + ".";
        return {};
    }
    if (!QDir().mkpath(m_folder + "/ui") || !QFile::copy(":/" + view, target)) {
        *problem = "The view can't be written to " + target + ".";
        return {};
    }
    // A copy out of the program is read-only until told otherwise.
    QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    return target;
}

QString Customization::writeGuide() const {
    QString keys;
    for (const Command &command : commands())
        keys += QString("| `%1` | %2 | %3 |\n").arg(command.id, "`" + command.keys.join("` `") + "`", command.what);
    const QString guide = QString(R"guide(# Omabook

<!-- Written by Omabook on every start. Edits here are overwritten. -->

This folder is **Omabook's interface, as files**. Omabook is an ebook library
and reader, and it follows these files while it runs: change a key, a color
or a view here and the window follows within a moment. When someone asks for
something ("give Space to next chapter", "make it warmer", "put my reading
list beside the shelf", "bigger covers"), you do it by editing these files.
For the books themselves (adding, details, reading text, syncing) use the
`omabook` command line instead: `omabook --help`, or `omabook skill`.

If Omabook's MCP server is connected to you (tools such as `books_search` and
`customize_get`; `omabook mcp install` connects it), prefer it for keys,
colors and settings: `customize_get` returns `keys.json`, `style.json` and the
look settings as they are, `customize_set` rewrites them with the same checks
as below and refuses a change that doesn't pass, and `customize_undo` puts
back what the last change replaced. Views and mods are still files to edit
here.

## Changing things safely

Edits apply at once. If a file can't be used, Omabook keeps the last version
that worked and writes why to `<name>.error` beside it (`keys.error`,
`style.error`, `ui.error`); the file goes away once the problem is fixed.
Check for it after every edit.

## config.json: the settings

One flat JSON object, also changed by the Settings dialog. Followed live.
The reading format keys (`font_size` to `align`) are the defaults: a book
can have its own, kept in the library (`omabook format <id> --align
center`, or the reader's "Aa" panel), which win over these.

| Key | Meaning | Default |
|---|---|---|
| `library` | The library folder in use | `~/Books` |
| `sort` | `recent`, `title`, `author`, `series`, `added`, `progress`, `rating`, `published` or `finished` | `recent` |
| `sort_reversed` | `true` to turn that order round | `false` |
| `shelves` | Per library folder, how its shelf is shown: `{"view": "covers", "spines", "carousel", "list" or "stacks", "group": "series", "author", "tag" or "status", "filters": {"status": "reading", "held": "paper", "format": "epub", "language": "en", "tag": "...", "author": "...", "series": "...", "rating": 4}}` | |
| `cover_size` | Width of a book on the shelf, in pixels | 176 |
| `font_size` | The reader's type size, in pixels (10 to 48) | 19 |
| `font_family` | The reader's font, empty for the desktop's | |
| `measure` | The longest line, in characters of the reader's font (40 to 90); its width in pixels follows the font and its size. An older `line_width` in pixels is carried over | 66 |
| `line_height` | Line spacing, in percent (100 to 300) | 150 |
| `paragraph_spacing` | Space after a paragraph, in pixels (0 to 80) | 12 |
| `indent` | Indent of a paragraph's first line, in pixels (0 to 64) | 0 |
| `margins` | Room around the text, in pixels (0 to 160) | 32 |
| `align` | `start`, `justify` (filled out to both sides), `center` or `end`; start and end turn round in a right-to-left language. The older `justify: true` still reads as `justify` | `start` |
| `pages` | Pages side by side in the reader: `"auto"` (as many as the window has room for, each with lines of at least 45 characters, up to three), or `1`, `2` or `3`, shown where each page keeps 45 characters (the most that do otherwise). An older `spread` is carried over | `"auto"` |
| `pane_widths` | How wide each side pane was dragged, in pixels, by name (`details`, `contents`, `assistant`, `graph`, `graph_details`, `plugins`): `{"assistant": 440}`. A pane that isn't named is as wide as it comes | |
| `sync_target` | The computer `sync` talks to: `name` or `name:folder` | |
| `ai_provider` | `auto`, `claude`, `agy`, `anthropic`, `openai` (a server: `ai_endpoint` says which, `{"preset": "omarchy-local", "ollama", "openai" or "custom", "base_url": "..."}`; its key is kept in the keyring, never here), `command` or `off` | `auto` |
| `ai_command` | The command for `command`; it is given `OMABOOK_AI_TIER` (`main` or `quick`) and `OMABOOK_AI_MODEL` | |
| `ai_models` | The model for each kind of work, per AI: `{"claude": {"main": "opus", "quick": "haiku"}}`. `main` is for answers and writing, `quick` for reading and parsing (the knowledge graph, judging look-ups, finding series); a missing or empty name is the default. `ai_model` is the older name of the Anthropic API's main model | |
| `plugins` | `{"<plugin id>": true or false}`; see `omabook plugins` | |
| `plugin_order` | The order the sources of book details are asked in, as a list of plugin ids | as shipped |

## keys.json: which key does what

`{"reader": {"Space": "next", "N": ""}, "library": {"T": "read"}, "global": {"F2": "settings"}, "graph": {"X": "remove"}}`

A key and the command it runs in that place (`graph` is the knowledge graph,
over the shelf or beside the page, `chronology` when its events happen); `""` frees the key. Whatever
the file doesn't mention keeps its default. A key given to a command is
taken from whatever had it in that place. Keys are written the way Qt does:
`A`, `Shift+A`, `Ctrl+F`, `PgDown`, `Escape`, `F11`. Keys pressed alone are
off while something is being typed; keys with Ctrl, Alt or Meta, and
function keys, always work.

| Command | Default keys | What it does |
|---|---|---|
%1
## style.json: the look

`{"theme": "auto", "colors": {"accent": "#c9954f", "background": "#1d1a16"}, "mode": "dark", "radius": 8, "font": "Inter", "font_size": 14}`

Every key is optional. `theme` is where the palette comes from: `auto` (the
default: Omarchy's current theme where Omarchy is, else Aether's, else
Tokyo Night), `omarchy`, `aether`, one of Omarchy's themes by name
(`omabook theme list`: `tokyo-night`, `catppuccin-latte`, ...), a theme of
your own in `themes/<name>/colors.toml` here, or the path of any
`colors.toml`. The rest lies over that theme. Colors: `accent`, `background`, `foreground`,
`selection`, `muted`, `red`, `green`, `yellow`, `orange`, `blue`, `magenta`,
`cyan`. `mode` says whether the result is `light` or `dark`. `radius` rounds
corners; `font` and `font_size` are the interface's (the reader's are in
config.json).

## ui/: views of your own

The window is QML. Every built-in view can be replaced by a file of the same
name in `ui/`: %2.
Start from the real one: `omabook customize ReaderView` writes the built-in
`ReaderView.qml` into `ui/` for you to edit. A view sees these objects:

- `backend`: the library. `books` (the shelf model: `query`, `sort`, `reversed`,
  `filters`, `setFilter(kind, value)`, `count`, `get(row)`), `view`, `group`,
  `book(id)`, `addBooks()`, `addFolder()`, `remove(id)`, `saveDetails(id, {...})`,
  `lookUp(id)`, `candidates`, `applyCandidate(i)`, `sync()`, `plugins`, `say(text)`.
- `reader`: the open book. `open`, `title`, `authors`, `kind` (`flow` or `pages`),
  `section`, `count`, `fraction`, `percent`, `toc`, `html`, `bookmarks`,
  `openBook(id)`, `close()`, `goTo(section, anchor)`, `find(words)`, `addBookmark(text)`.
- `assistant`: the AI. `available`, `messages`, `streaming`, `busy`,
  `ask(question, scope, selection)`, `catchUp()`, `cancel()`.
- `settings`: config.json as properties (`fontSize`, `measure`, `pages`, `coverSize`, ...).
- `theme`: the look (`accent`, `accentForeground`, `background`, `foreground`,
  `colors.<name>`, `light`, `radius`).
- `custom`: these files (`plainKeys`, `chordKeys`, `keyText`, `style`, `mods`).

A key is bound with `Key { command: "reader.next"; when: ...; onActivated: ... }`,
and every visible string is wrapped in the `qsTr` translation function.

## mods/: panels of your own

`mods/<name>/` with a `Main.qml` and a `mod.json`:

`{"place": "library.side", "title": "Reading list", "width": 280}`

`place` is `library.side` (beside the shelf) or `reader.side` (beside the
page). `Main.qml` is any QML item; it sees the same objects as a view. A mod
adds to the window without replacing any of it, so it survives updates that
a copied view would have to be merged with.
)guide").arg(keys, "`" + builtInViews().join("`, `") + "`");
    const QString path = m_folder + "/AGENTS.md";
    QFile file(path);
    if (!QDir().mkpath(m_folder) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write((guide + ScriptApi::guide()).toUtf8()) < 0)
        return {};
    return path;
}

Look::Look(Theme *theme, ThemeChoice *choice, Customization *custom, QObject *parent) : Look(theme, custom, parent) {
    m_choice = choice;
    if (choice)
        connect(choice, &ThemeChoice::changed, this, &Look::changed);
}

Look::Look(Theme *theme, Customization *custom, QObject *parent) : QObject(parent), m_theme(theme), m_custom(custom) {
    connect(theme, &Theme::changed, this, &Look::changed);
    // The interface font is the application's; style.json may name another.
    const QFont desktop = QGuiApplication::font();
    auto restyle = [this, desktop] {
        QFont font = desktop;
        const QVariantMap style = m_custom->style();
        if (style.contains("font")) font.setFamily(style.value("font").toString());
        if (style.contains("font_size")) font.setPixelSize(style.value("font_size").toInt());
        if (font != QGuiApplication::font())
            QGuiApplication::setFont(font);
        emit changed();
    };
    connect(custom, &Customization::styleChanged, this, restyle);
    restyle();
}

QObject *Look::themes() const noexcept { return m_choice; }

// Per colour: style.json's, then the chosen theme's (over bundled Tokyo
// Night), then Theme's, whose built-in fallback has every name.
QColor Look::color(const QString &key) const {
    const QVariantMap colors = m_custom->style().value("colors").toMap();
    if (colors.contains(key))
        return colors.value(key).value<QColor>();
    if (m_choice && m_choice->palette().colors.contains(key))
        return m_choice->palette().colors.value(key);
    return m_theme->colors().value(key, m_theme->foreground()).value<QColor>();
}

QVariantMap Look::colors() const {
    QVariantMap colors = m_theme->colors();
    if (m_choice) {
        // Only the built-in names from Theme: the rest of the desktop's
        // palette isn't the chosen theme's.
        QVariantMap base;
        for (const char *name : {"accent", "background", "foreground", "selection", "muted", "red", "green", "yellow",
                                 "orange", "blue", "magenta", "cyan"})
            base.insert(name, colors.value(name));
        colors = base;
        for (auto it = m_choice->palette().colors.begin(); it != m_choice->palette().colors.end(); ++it)
            colors.insert(it.key(), it.value());
    }
    colors.insert(m_custom->style().value("colors").toMap());
    return colors;
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor Look::accentForeground() const {
    return relativeLuminance(accent()) > .179 ? QColor("black") : QColor("white");
}

bool Look::light() const {
    const QVariantMap style = m_custom->style();
    if (style.contains("mode"))
        return style.value("mode").toString() == "light";
    // A background of the user's own decides, as Omarchy decides for a theme.
    if (style.value("colors").toMap().contains("background")) {
        const QColor background = this->background();
        return background.red() + background.green() + background.blue() > 382;
    }
    return m_choice ? m_choice->palette().light : m_theme->light();
}

int Look::radius() const {
    const QVariantMap style = m_custom->style();
    return style.contains("radius") ? style.value("radius").toInt() : m_theme->radius();
}
