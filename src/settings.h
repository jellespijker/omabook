#pragma once

#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QVariantMap>
#include <algorithm>

// The user's choices, kept in ~/.config/omabook/config.json and written
// atomically on every change. Tests pass their own file.
class Settings : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString libraryPath READ libraryPath WRITE setLibraryPath NOTIFY changed)
    Q_PROPERTY(QStringList libraries READ libraries NOTIFY changed)
    Q_PROPERTY(int fontSize READ fontSize WRITE setFontSize NOTIFY changed)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY changed)
    Q_PROPERTY(int lineWidth READ lineWidth WRITE setLineWidth NOTIFY changed)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY changed)
    Q_PROPERTY(int coverSize READ coverSize NOTIFY changed)
    Q_PROPERTY(int lineHeight READ lineHeight NOTIFY changed)
    Q_PROPERTY(int paragraphSpacing READ paragraphSpacing NOTIFY changed)
    Q_PROPERTY(bool justify READ justify NOTIFY changed)
    Q_PROPERTY(QString spread READ spread NOTIFY changed)
    Q_PROPERTY(QVariantMap paneWidths READ paneWidths NOTIFY changed)
    Q_PROPERTY(QString syncTarget READ syncTarget WRITE setSyncTarget NOTIFY changed)
    Q_PROPERTY(QString aiProvider READ aiProvider WRITE setAiProvider NOTIFY changed)
    Q_PROPERTY(QString aiCommand READ aiCommand WRITE setAiCommand NOTIFY changed)
    Q_PROPERTY(QString aiModel READ aiModel WRITE setAiModel NOTIFY changed)
    Q_PROPERTY(QString graph READ graph WRITE setGraph NOTIFY changed)
    Q_PROPERTY(QString marks READ marks WRITE setMarks NOTIFY changed)
    Q_PROPERTY(QString pluginUpdates READ pluginUpdates WRITE setPluginUpdates NOTIFY changed)
    Q_PROPERTY(QString goodreadsUser READ goodreadsUser WRITE setGoodreadsUser NOTIFY changed)
    Q_PROPERTY(QString openLibraryUser READ openLibraryUser WRITE setOpenLibraryUser NOTIFY changed)

public:
    explicit Settings(const QString &file = {}, QObject *parent = nullptr);

    // The library in use, and every library that has been.
    QString libraryPath() const;
    void setLibraryPath(const QString &path);
    QStringList libraries() const;

    // The reader's type size in pixels, its font ("" for the desktop's), and
    // the widest a line of text gets, in pixels.
    int fontSize() const { return m_values.value("font_size").toInt(19); }
    void setFontSize(int size) { set("font_size", std::clamp(size, 10, 48)); }
    QString fontFamily() const { return m_values.value("font_family").toString(); }
    void setFontFamily(const QString &family) { set("font_family", family); }
    int lineWidth() const { return m_values.value("line_width").toInt(720); }
    void setLineWidth(int width) { set("line_width", std::clamp(width, 320, 1600)); }

    // How the shelf and the page are set, for those who want it otherwise:
    // the width of a book on the shelf, line spacing in percent, the space
    // after a paragraph, flush text, and when two pages show side by side
    // ("auto" for wide windows, "always", "never").
    int coverSize() const { return std::clamp(m_values.value("cover_size").toInt(176), 96, 480); }
    int lineHeight() const { return std::clamp(m_values.value("line_height").toInt(150), 100, 300); }
    int paragraphSpacing() const { return std::clamp(m_values.value("paragraph_spacing").toInt(12), 0, 80); }
    bool justify() const { return m_values.value("justify").toBool(false); }
    QString spread() const { return m_values.value("spread").toString("auto"); }

    // How wide each pane beside the shelf or the page was dragged, by its
    // name ("details", "contents", "assistant", "graph", "graph_details",
    // "plugins"), in
    // pixels. Pixels and not a share of the window: a pane holds text, which
    // reads at one width whether the window is a narrow tile or a whole 4K
    // screen, so the choice is kept as made and only held within what the
    // window has room for while it is shown (see SidePane.qml). A pane that
    // isn't named is as wide as it comes.
    QVariantMap paneWidths() const { return m_values.value("pane_widths").toObject().toVariantMap(); }
    Q_INVOKABLE int paneWidth(const QString &pane) const { return m_values.value("pane_widths").toObject().value(pane).toInt(); }
    // A width of 0 forgets the choice.
    Q_INVOKABLE void setPaneWidth(const QString &pane, int width);

    // "recent", "title", "author", "series", "added", "progress", "rating",
    // "published" or "finished", and whether that order is turned round.
    QString sort() const { return m_values.value("sort").toString("recent"); }
    void setSort(const QString &sort) { set("sort", sort); }
    bool sortReversed() const { return m_values.value("sort_reversed").toBool(false); }
    void setSortReversed(bool reversed) { set("sort_reversed", reversed); }
    // How a library's shelf was left: {"view": "spines", "group": "series",
    // "filters": {"status": "reading"}}, kept per library folder.
    QJsonObject shelf(const QString &library) const;
    void setShelf(const QString &library, const QJsonObject &shelf);

    // The computer to sync with, as ssh knows it: "laptop" or "laptop:Books".
    QString syncTarget() const { return m_values.value("sync_target").toString(); }
    void setSyncTarget(const QString &target) { set("sync_target", target.trimmed()); }
    // Takes a library off the list; its folder is left as it is.
    Q_INVOKABLE void forgetLibrary(const QString &path);

    // Sets one of the settings that shape the look, by its name in
    // config.json; false for a name that isn't one or a value out of range.
    bool setOption(const QString &key, const QJsonValue &value);
    // Those settings as they are now.
    QJsonObject options() const;

    // Whether a plugin is on: the user's choice, or `byDefault` where they made none.
    bool pluginEnabled(const QString &id, bool byDefault) const;
    void setPluginEnabled(const QString &id, bool enabled);
    // The order the sources of book details are asked in, by plugin id; one
    // that isn't named comes after those that are. Empty for the order as shipped.
    QStringList pluginOrder() const { return m_values.value("plugin_order").toVariant().toStringList(); }
    void setPluginOrder(const QStringList &ids) { set("plugin_order", QJsonArray::fromStringList(ids)); }
    // What is done about plugins that came from a GitHub repository (see
    // PluginSources): "auto" (an update that asks for nothing new installs
    // itself), "tell" (it is only said that there is one) or "never" (the
    // repositories aren't asked unless the person does).
    QString pluginUpdates() const {
        const QString mode = m_values.value("plugin_updates").toString();
        return mode == "tell" || mode == "never" ? mode : QStringLiteral("auto");
    }
    void setPluginUpdates(const QString &mode) {
        if (mode == "auto" || mode == "tell" || mode == "never")
            set("plugin_updates", mode);
    }
    // Whether a token for GitHub was put in the keyring (under "github"),
    // for plugins kept in private repositories. Only that it is there is
    // written here; the token itself never is.
    bool pluginToken() const { return m_values.value("plugin_github_token").toBool(false); }
    void setPluginToken(bool kept) { set("plugin_github_token", kept); }

    // What was answered when a Calibre library was offered for import, by
    // its folder: {"answer": "imported", "later" or "never", "at": seconds}.
    // An empty answer forgets the folder. See CalibrePlugin.
    QJsonObject calibreLibraries() const { return m_values.value("calibre_libraries").toObject(); }
    void setCalibreLibrary(const QString &folder, const QString &answer);

    // "auto" (Omarchy's default agent), a harness by name ("claude", "codex",
    // "agy", "grok", "pi"), "anthropic", "command" or "off"; see AiChoice in ai.h.
    QString aiProvider() const { return m_values.value("ai_provider").toString("auto"); }
    void setAiProvider(const QString &provider) { set("ai_provider", provider); }
    QString aiCommand() const { return m_values.value("ai_command").toString(); }
    void setAiCommand(const QString &command) { set("ai_command", command.trimmed()); }
    QString aiModel() const { return m_values.value("ai_model").toString(); }
    void setAiModel(const QString &model) { set("ai_model", model.trimmed()); }
    // The model chosen for a kind of work ("main" or "quick") with one
    // provider, kept under "ai_models" by provider id, since a name that
    // suits one harness means nothing to the next; empty for none chosen.
    // The older "ai_model" is still read as the Anthropic API's main model.
    QString aiModelFor(const QString &provider, const QString &tier) const;
    void setAiModelFor(const QString &provider, const QString &tier, const QString &model);

    // How the knowledge graph of a book grows: "auto" (by itself, as chapters
    // are finished and with every catch-up, which costs AI calls), "ask"
    // (only when asked to: `omabook graph build`, or from the window) or
    // "off" (no graph is kept or used while reading).
    QString graph() const;
    void setGraph(const QString &mode);
    // When the things of the graph are marked in the text: "map" (while the
    // map of the book is beside the page), "always" or "never".
    QString marks() const;
    void setMarks(const QString &mode);

    // Whose shelves the reading sites are asked for: a Goodreads user id,
    // profile address or shelf feed address, and an Open Library user name.
    // Each is sent only to its own site.
    QString goodreadsUser() const { return m_values.value("goodreads_user").toString(); }
    void setGoodreadsUser(const QString &user) { set("goodreads_user", user.trimmed()); }
    QString openLibraryUser() const { return m_values.value("openlibrary_user").toString(); }
    void setOpenLibraryUser(const QString &user) { set("openlibrary_user", user.trimmed()); }
    // The same for any reading site, by the name its books go by.
    QString siteIdentity(const QString &scheme) const;
    void setSiteIdentity(const QString &scheme, const QString &identity);
    // What is remembered of a connection to a reading site, and never a
    // token or key (those are in the keyring; "secret" only says there is
    // one): {"name": who the site says it is, "state": "connected" or
    // "attention", "problem", "why", "books", "shelves", "checked" and
    // "synced" (seconds), "write" (changes go to the site too), "sent"
    // (the first such write was looked over and agreed to)}.
    QJsonObject site(const QString &scheme) const { return m_values.value("sites").toObject().value(scheme).toObject(); }
    void setSite(const QString &scheme, const QJsonObject &connection);
    // Whether the site's plugin is on; a site without a plugin always is.
    bool siteOn(const QString &scheme) const;

    // How far the feature tour got ("" for never offered, "library" once the
    // shelf's half was taken, "done"), and the version whose new features
    // were last shown or turned down. See Manual.
    QString tour() const { return m_values.value("tour").toString(); }
    void setTour(const QString &state) { set("tour", state); }
    QString newsSeen() const { return m_values.value("news_seen").toString(); }
    void setNewsSeen(const QString &version) { set("news_seen", version); }

signals:
    void changed();

private:
    void set(const QString &key, const QJsonValue &value);
    void save();
    // An older config.json carried forward to the shape this Omabook writes.
    static QJsonObject upgraded(QJsonObject values);
    void read();
    void watch();

    // The file is the settings: edited by hand, or by an agent, it is
    // followed while the app runs.
    QFileSystemWatcher m_watcher;

    QString m_file;
    QJsonObject m_values;
};
