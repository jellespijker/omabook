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

#include "scriptapi.h"
#include "theme.h"
#include "themechoice.h"

namespace {

using namespace Qt::StringLiterals;

// Where keys work: what a command's id begins with, and what keys.json is divided into.
constexpr std::array<std::string_view, 4> places{"global", "library", "reader", "graph"};

[[nodiscard]] constexpr std::string_view placeOf(std::string_view command) noexcept { return command.substr(0, command.find('.')); }

[[nodiscard]] bool isPlace(QStringView name) {
    return std::ranges::any_of(places, [name](std::string_view place) { return name == QLatin1StringView(place); });
}

// A built-in command as it is written down below, all of it known when the
// program is built.
struct BuiltIn {
    std::string_view id;
    // The keys it has by default: three at most, the room left over empty.
    std::array<std::string_view, 3> keys;
    const char *what;
};

[[nodiscard]] constexpr auto keysOf(const BuiltIn &command) {
    return command.keys | std::views::filter([](std::string_view key) { return !key.empty(); });
}

// The descriptions stay in English here, for AGENTS.md; marking each
// with tr() puts it in the catalogs, and the help translates it when
// it is shown (see Manual::commands).
constexpr auto tr = [](const char *text) noexcept { return text; };

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

constexpr BuiltIn builtIns[] = {
        {"global.help", {"?", "F1"}, tr("Show or hide the help: every key, and how things work")},
        {"global.quit", {"Q", "Ctrl+Q"}, tr("Quit")},
        {"global.settings", {","}, tr("Open the settings")},
        {"global.change", {"Ctrl+K"}, tr("Ask the AI to change how Omabook looks or works")},
        {"global.pane_wider", {">"}, tr("Make the pane beside the shelf or the page wider")},
        {"global.pane_narrower", {"<"}, tr("Make the pane beside the shelf or the page narrower")},
        {"library.read", {"Space"}, tr("Read the selected book")},
        {"library.left", {"H"}, tr("Select the book to the left")},
        {"library.right", {"L"}, tr("Select the book to the right")},
        {"library.down", {"J"}, tr("Select the book below")},
        {"library.up", {"K"}, tr("Select the book above")},
        {"library.search", {"/", "Ctrl+F"}, tr("Search the library")},
        {"library.search_ai", {"Shift+W"}, tr("Have the AI find books for a question, such as “the books where a mentor dies”")},
        {"library.found_graph", {"Shift+I"}, tr("Show in the graph what the search found the selected book by")},
        {"library.found_moment", {"Shift+Space"}, tr("Go to where in the book the search found the selected book by")},
        {"library.add", {"A"}, tr("Add books")},
        {"library.add_folder", {"Shift+A"}, tr("Add a folder: of books, a Calibre library, or one book kept as a folder of Markdown files")},
        {"library.new", {"B"}, tr("Add a book that has no file: one on your shelf, or one to read some day")},
        {"library.edit", {"E"}, tr("Edit the selected book's details")},
        {"library.lookup", {"M"}, tr("Look the selected book's details up")},
        {"library.export", {"X"}, tr("Save a copy of the selected book elsewhere")},
        {"library.remove", {"D", "Delete"}, tr("Remove the selected book (asks first)")},
        {"library.sort", {"S"}, tr("Change the order of the shelf")},
        {"library.sort_reverse", {"Shift+S"}, tr("Turn the order of the shelf round")},
        {"library.filter", {"F"}, tr("Filter the shelf: by status, paper or digital, format, language, tag, author, series or rating")},
        {"library.filter_clear", {"Shift+F"}, tr("Take every filter off the shelf")},
        {"library.view_covers", {"1"}, tr("Show the library as covers")},
        {"library.view_spines", {"2"}, tr("Show the library as spines on a bookshelf")},
        {"library.view_carousel", {"3"}, tr("Show the library as a carousel")},
        {"library.view_list", {"4"}, tr("Show the library as a list")},
        {"library.view_stacks", {"5"}, tr("Show the library as stacks by series, author, tag or status")},
        {"library.view_next", {"V"}, tr("Show the library the next way")},
        {"library.group", {"G"}, tr("Stack by the next of series, author, tag and status")},
        {"library.libraries", {"O"}, tr("Open another library")},
        {"library.collections", {"C"}, tr("Show one collection of the library, or all books; make, change or sort collections")},
        {"library.collection_next", {"]"}, tr("Show the next collection")},
        {"library.collection_previous", {"["}, tr("Show the previous collection")},
        {"library.collection_book", {"Shift+C"}, tr("Put the selected book in collections, or take it out")},
        {"library.sync", {"Y"}, tr("Sync with the computer named in the settings")},
        {"library.plugins", {"P"}, tr("Show the plugins")},
        {"library.details", {"I"}, tr("Show or hide the details beside the shelf")},
        {"library.tidy", {"Shift+T"}, tr("Look the whole library over for things to tidy")},
        {"library.sites", {"R"}, tr("Sync with Goodreads and other reading sites")},
        {"library.ask", {"W"}, tr("Ask the AI about the library")},
        {"library.suggest", {"N"}, tr("Have the AI suggest what to read next")},
        {"library.graph", {"6"}, tr("Show the library as a graph of what is known: books, people, places, ideas and how they relate")},
        {"library.graph_book", {"U"}, tr("Show what is known of the selected book as a graph")},
        {"library.graph_build", {"Shift+U"}, tr("Build the knowledge graph for books you have already read")},
        {"reader.back", {"Escape", "Backspace"}, tr("Close a pane, or go back to the library")},
        {"reader.page_down", {"Space", "Right"}, tr("Turn the page, or scroll on a screen, on into the next chapter")},
        {"reader.page_up", {"Shift+Space", "Left"}, tr("Turn the page back, or scroll back a screen")},
        {"reader.line_down", {"J", "Down"}, tr("Scroll down a few lines")},
        {"reader.line_up", {"K", "Up"}, tr("Scroll up a few lines")},
        {"reader.next", {"PgDown", "L", "N"}, tr("Go to the next chapter")},
        {"reader.previous", {"PgUp", "H", "P"}, tr("Go to the start of the chapter, or from there to the previous chapter")},
        {"reader.start", {"G", "Home"}, tr("Go to the start of the chapter")},
        {"reader.end", {"Shift+G", "End"}, tr("Go to the end of the chapter")},
        {"reader.larger", {"+", "="}, tr("Larger text, or zoom in")},
        {"reader.smaller", {"-"}, tr("Smaller text, or zoom out")},
        {"reader.contents", {"T"}, tr("Show or hide the contents beside the page")},
        {"reader.find", {"/", "Ctrl+F"}, tr("Find words in the book")},
        {"reader.bookmark", {"B"}, tr("Bookmark this place")},
        {"reader.ask", {"A"}, tr("Ask the AI about what is being read")},
        {"reader.catch_up", {"C"}, tr("Have the AI catch you up")},
        {"reader.cast", {"W"}, tr("Have the AI say who is who so far")},
        {"reader.expand", {"F", "F11"}, tr("The book and nothing else, on the whole screen")},
        {"reader.graph", {"M"}, tr("Show or hide the map of what the book has told so far, beside the page")},
        {"reader.marks", {"Shift+M"}, tr("Mark the graph's people, places and things in the text: while the map is open, always, or never")},
        {"reader.select", {"V"}, tr("Select words with the keyboard: the arrows or H J K L move, Shift widens, Enter shows what to do with them")},
        {"reader.format", {"Shift+A"}, tr("Show or hide how the text is set: the font, its size, the longest line, the pages side by side, the spacing, the margins and the alignment, for this book or every book")},
        {"reader.pages", {"Shift+P"}, tr("Pages side by side: as many as fit, one, two or three, and round again")},
        {"reader.layout", {"Shift+L"}, tr("Read as pages that turn, as one column that scrolls on through the chapters, or automatically, and round again")},
        // The graph view, wherever it is open: over the shelf or beside the page.
        {"graph.left", {"H", "Left"}, tr("Select the nearest related thing to the left")},
        {"graph.right", {"L", "Right"}, tr("Select the nearest related thing to the right")},
        {"graph.up", {"K", "Up"}, tr("Select the nearest related thing above")},
        {"graph.down", {"J", "Down"}, tr("Select the nearest related thing below")},
        {"graph.next", {"Tab"}, tr("Select the next thing in the list")},
        {"graph.previous", {"Shift+Tab"}, tr("Select the previous thing in the list")},
        {"graph.expand", {"Return", "Enter"}, tr("Centre on the selected thing and show what is around it")},
        {"graph.back", {"Backspace"}, tr("Go back to what the graph showed before")},
        {"graph.close", {"Escape"}, tr("Close the graph")},
        {"graph.find", {"/", "Ctrl+F"}, tr("Find a thing in the graph by name")},
        {"graph.filter", {"F"}, tr("Filter the graph: by kind, book, series or world, or only what was asked about")},
        {"graph.list", {"V"}, tr("Switch between the graph as a picture and as a list")},
        {"graph.details", {"I"}, tr("Show or hide the details of the selected thing")},
        {"graph.open", {"O"}, tr("Read the selected book, or go to where the selected thing was first met")},
        {"graph.moments", {"R"}, tr("Go through when what is known of the selected thing was revealed: Tab moves on, Enter goes to that moment in the text")},
        {"graph.further", {"]"}, tr("Show one step further around the centre of the graph")},
        {"graph.nearer", {"["}, tr("Show one step less around the centre of the graph")},
        {"graph.zoom_in", {"+", "="}, tr("Zoom in on the graph")},
        {"graph.zoom_out", {"-"}, tr("Zoom out of the graph")},
        {"graph.fit", {"0"}, tr("Fit the whole graph in the view")},
        {"graph.pan_left", {"Shift+H", "Shift+Left"}, tr("Move the view of the graph left")},
        {"graph.pan_right", {"Shift+L", "Shift+Right"}, tr("Move the view of the graph right")},
        {"graph.pan_up", {"Shift+K", "Shift+Up"}, tr("Move the view of the graph up")},
        {"graph.pan_down", {"Shift+J", "Shift+Down"}, tr("Move the view of the graph down")},
        {"graph.beyond", {"X"}, tr("Show or hide what a book's things connect to in other books")},
        {"graph.add", {"N"}, tr("Add a thing to the graph, related to the selected one")},
        {"graph.edit", {"E"}, tr("Rename the selected thing, or rewrite what is said of it")},
        {"graph.merge", {"Shift+M"}, tr("Fold another thing into the selected one: they are the same")},
        {"graph.remove", {"D", "Delete"}, tr("Remove the selected thing from the graph (asks first)")},
};

// A command is found by its id, which begins with the place it works in.
[[nodiscard]] constexpr bool namedApart(std::span<const BuiltIn> commands) {
    for (std::size_t one = 0; one < commands.size(); ++one) {
        if (!std::ranges::contains(places, placeOf(commands[one].id)))
            return false;
        for (std::size_t other = one + 1; other < commands.size(); ++other)
            if (commands[one].id == commands[other].id)
                return false;
    }
    return true;
}

// A key does one thing where it is pressed: no two commands of one place
// have the same key by default, and none has a key that works everywhere.
// (The graph's keys are its own: it lies over the shelf or beside the page.)
[[nodiscard]] constexpr bool keyedApart(std::span<const BuiltIn> commands) {
    for (std::size_t one = 0; one < commands.size(); ++one)
        for (std::size_t other = one + 1; other < commands.size(); ++other) {
            const std::string_view here = placeOf(commands[one].id), there = placeOf(commands[other].id);
            const bool together = here == there || here == "global" || there == "global";
            const auto shared = [&](std::string_view key) { return std::ranges::contains(keysOf(commands[other]), key); };
            if (together && std::ranges::any_of(keysOf(commands[one]), shared))
                return false;
        }
    return true;
}

static_assert(namedApart(builtIns), "every command has an id of its own, beginning with a place keys work in");
static_assert(keyedApart(builtIns), "two commands of one place have the same default key");

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
            problem = "\"" + place.key() + "\" isn't a place keys work in: use \"global\", \"library\", \"reader\" or \"graph\".";
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
| `ai_provider` | `auto`, `claude`, `agy`, `anthropic`, `command` or `off` | `auto` |
| `ai_command` | The command for `command`; it is given `OMABOOK_AI_TIER` (`main` or `quick`) and `OMABOOK_AI_MODEL` | |
| `ai_models` | The model for each kind of work, per AI: `{"claude": {"main": "opus", "quick": "haiku"}}`. `main` is for answers and writing, `quick` for reading and parsing (the knowledge graph, judging look-ups, finding series); a missing or empty name is the default. `ai_model` is the older name of the Anthropic API's main model | |
| `plugins` | `{"<plugin id>": true or false}`; see `omabook plugins` | |
| `plugin_order` | The order the sources of book details are asked in, as a list of plugin ids | as shipped |

## keys.json: which key does what

`{"reader": {"Space": "next", "N": ""}, "library": {"T": "read"}, "global": {"F2": "settings"}, "graph": {"X": "remove"}}`

A key and the command it runs in that place (`graph` is the knowledge graph,
over the shelf or beside the page); `""` frees the key. Whatever
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
