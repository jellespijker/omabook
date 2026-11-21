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
    Q_PROPERTY(int measure READ measure WRITE setMeasure NOTIFY changed)
    Q_PROPERTY(QString sort READ sort WRITE setSort NOTIFY changed)
    Q_PROPERTY(int coverSize READ coverSize NOTIFY changed)
    Q_PROPERTY(int lineHeight READ lineHeight WRITE setLineHeight NOTIFY changed)
    Q_PROPERTY(int paragraphSpacing READ paragraphSpacing WRITE setParagraphSpacing NOTIFY changed)
    Q_PROPERTY(int indent READ indent WRITE setIndent NOTIFY changed)
    Q_PROPERTY(int margins READ margins WRITE setMargins NOTIFY changed)
    Q_PROPERTY(QString align READ align WRITE setAlign NOTIFY changed)
    // Whether the text is justified: what `align` was before it was there.
    Q_PROPERTY(bool justify READ justify NOTIFY changed)
    // Whether Omabook shows its libraries, what is being read and "Add to
    // Library" in Folio, the file manager.
    Q_PROPERTY(bool folio READ folio WRITE setFolio NOTIFY changed)
    // Whether a book's details go into its EPUB whenever they are edited (#74).
    Q_PROPERTY(bool writeDetails READ writeDetails WRITE setWriteDetails NOTIFY changed)
    Q_PROPERTY(int pages READ pages WRITE setPages NOTIFY changed)
    Q_PROPERTY(QVariantMap paneWidths READ paneWidths NOTIFY changed)
    Q_PROPERTY(QVariantMap highlightNames READ highlightNames NOTIFY changed)
    Q_PROPERTY(QString syncTarget READ syncTarget WRITE setSyncTarget NOTIFY changed)
    Q_PROPERTY(QString aiProvider READ aiProvider WRITE setAiProvider NOTIFY changed)
    Q_PROPERTY(QString aiCommand READ aiCommand WRITE setAiCommand NOTIFY changed)
    Q_PROPERTY(QString aiModel READ aiModel WRITE setAiModel NOTIFY changed)
    Q_PROPERTY(QString graph READ graph WRITE setGraph NOTIFY changed)
    Q_PROPERTY(bool recognitionHosted READ recognitionHosted WRITE setRecognitionHosted NOTIFY changed)
    Q_PROPERTY(bool meaningHosted READ meaningHosted WRITE setMeaningHosted NOTIFY changed)
    Q_PROPERTY(bool meaningAll READ meaningAll WRITE setMeaningAll NOTIFY changed)
    Q_PROPERTY(bool graphOneTenant READ graphOneTenant WRITE setGraphOneTenant NOTIFY changed)
    Q_PROPERTY(QString marks READ marks WRITE setMarks NOTIFY changed)
    Q_PROPERTY(QString inkFingers READ inkFingers WRITE setInkFingers NOTIFY changed)
    Q_PROPERTY(QString pluginUpdates READ pluginUpdates WRITE setPluginUpdates NOTIFY changed)
    Q_PROPERTY(QString goodreadsUser READ goodreadsUser WRITE setGoodreadsUser NOTIFY changed)
    Q_PROPERTY(QString openLibraryUser READ openLibraryUser WRITE setOpenLibraryUser NOTIFY changed)

public:
    explicit Settings(const QString &file = {}, QObject *parent = nullptr);
    // Where config.json is when no file is given.
    static QString defaultFile();

    // The library in use, and every library that has been. Setting it makes
    // it the one in use from now on, for the window, the command line and
    // agents alike, and ends any library used for this session only.
    QString libraryPath() const;
    void setLibraryPath(const QString &path);
    // A library for as long as this Settings lives (`omabook open --library`,
    // a book opened from another library in the file manager): libraryPath()
    // and libraries() say it, config.json keeps the one in use.
    void useLibraryForSession(const QString &path);
    QStringList libraries() const;

    // How the text of a book is set unless the book has its own (see
    // readingformat.h, which holds the bounds): the type size in pixels, its
    // font ("" for the desktop's), the longest line in characters, line spacing in
    // percent, the space after a paragraph, the indent of a paragraph's
    // first line, the room around the text, and the alignment ("start",
    // "justify", "center" or "end"). A value out of bounds is held to them.
    int fontSize() const { return number(u"font_size"); }
    void setFontSize(int size) { setNumber(u"font_size", size); }
    QString fontFamily() const { return m_values.value("font_family").toString(); }
    void setFontFamily(const QString &family) { set("font_family", family.trimmed()); }
    int measure() const { return number(u"measure"); }
    void setMeasure(int characters) { setNumber(u"measure", characters); }
    int lineHeight() const { return number(u"line_height"); }
    void setLineHeight(int percent) { setNumber(u"line_height", percent); }
    int paragraphSpacing() const { return number(u"paragraph_spacing"); }
    void setParagraphSpacing(int pixels) { setNumber(u"paragraph_spacing", pixels); }
    int indent() const { return number(u"indent"); }
    void setIndent(int pixels) { setNumber(u"indent", pixels); }
    int margins() const { return number(u"margins"); }
    void setMargins(int pixels) { setNumber(u"margins", pixels); }
    QString align() const;
    void setAlign(const QString &align);
    bool justify() const { return align() == "justify"; }
    // All of those by their keys in config.json.
    QJsonObject format() const;

    // How the shelf is set, for those who want it otherwise: the width of a
    // book on the shelf.
    int coverSize() const { return std::clamp(m_values.value("cover_size").toInt(176), 96, 480); }
    bool folio() const { return m_values.value("folio").toBool(true); }
    void setFolio(bool on) { set("folio", on); }
    // Whether a book's details are written into its EPUB, the library's own
    // copy, whenever they are edited here (#74): in the window, with `omabook
    // set` and by the tools. Off unless the person switches it on.
    bool writeDetails() const { return m_values.value("write_details").toBool(false); }
    void setWriteDetails(bool on) { set("write_details", on); }
    // How many pages of a flowing book stand side by side: 0 for as many as
    // the window has room for (see PageLayout::columns), else 1, 2 or 3.
    // In config.json, "pages": "auto", 1, 2 or 3.
    int pages() const;
    void setPages(int pages);

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
    // What the reader calls each highlight colour (#81), by its name
    // ("yellow": "Important"); one not named is called by its colour.
    QVariantMap highlightNames() const { return m_values.value("highlight_names").toObject().toVariantMap(); }
    // An empty name forgets it.
    Q_INVOKABLE void setHighlightName(const QString &color, const QString &name);
    // Whether others' annotations are shown beside one's own (#85), and the
    // people (by person id) whose aren't, each switched off by hand.
    bool othersAnnotations() const { return m_values.value("others_annotations").toBool(true); }
    void setOthersAnnotations(bool shown) { set("others_annotations", shown); }
    QStringList hiddenAnnotators() const;
    void setAnnotatorHidden(const QString &person, bool hidden);

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
    // "pages" takes "auto", 1, 2 or 3.
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
    // Whether plugins are looked for on GitHub, by their topic and in the
    // registry kept there (see PluginFinder); off, only the registry this
    // Omabook was built with is shown, and nothing is asked of the network.
    bool pluginDiscovery() const { return m_values.value("plugin_discovery").toBool(true); }
    void setPluginDiscovery(bool on) { set("plugin_discovery", on); }

    // What was answered when a Calibre library was offered for import, by
    // its folder: {"answer": "imported", "later" or "never", "at": seconds}.
    // An empty answer forgets the folder. See CalibrePlugin.
    QJsonObject calibreLibraries() const { return m_values.value("calibre_libraries").toObject(); }
    void setCalibreLibrary(const QString &folder, const QString &answer);

    // "auto" (Omarchy's default agent), a harness by name ("claude", "codex",
    // "agy", "grok", "pi"), "anthropic", "openai" (a server), "command" or
    // "off"; see AiChoice in ai.h.
    QString aiProvider() const { return m_values.value("ai_provider").toString("auto"); }
    void setAiProvider(const QString &provider) { set("ai_provider", provider); }
    // Off: nothing is asked of any model, on whatever server (#133): no
    // answers, no recognition, no embeddings.
    bool aiOff() const { return aiProvider() == "off"; }
    QString aiCommand() const { return m_values.value("ai_command").toString(); }
    void setAiCommand(const QString &command) { set("ai_command", command.trimmed()); }
    // The server "openai" asks: {"preset": "ollama", "base_url": "..."}; see
    // AiEndpoints. Never its key, which is kept in the keyring.
    QJsonObject aiEndpoint() const { return m_values.value("ai_endpoint").toObject(); }
    void setAiEndpoint(const QJsonObject &endpoint) { set("ai_endpoint", endpoint); }
    QString aiModel() const { return m_values.value("ai_model").toString(); }
    void setAiModel(const QString &model) { set("ai_model", model.trimmed()); }
    // The model chosen for a kind of work ("main" or "quick") with one
    // provider, kept under "ai_models" by provider id, since a name that
    // suits one harness means nothing to the next; empty for none chosen.
    // The older "ai_model" is still read as the Anthropic API's main model.
    QString aiModelFor(const QString &provider, const QString &tier) const;
    void setAiModelFor(const QString &provider, const QString &tier, const QString &model);

    // How handwriting and speech are recognised (#84): the models and servers
    // chosen for it, which addresses are one's own, and whether a hosted
    // server may be sent them at all; see RecognitionRoute.
    QJsonObject recognition() const { return m_values.value("recognition").toObject(); }
    void setRecognition(const QJsonObject &chosen) { set("recognition", chosen); }
    bool recognitionHosted() const { return recognition().value("hosted").toBool(false); }
    void setRecognitionHosted(bool on) {
        QJsonObject chosen = recognition();
        chosen.insert("hosted", on);
        setRecognition(chosen);
    }
    // How a book's text is found by what it means (#72): "url", the server
    // of the OpenAI-compatible API that makes the embeddings (else the AI
    // server), "model" (else nomic-embed-text), "hosted", whether a hosted
    // server may be sent a book's text at all, and "all", whether every book
    // is indexed rather than only those asked for; see Embeddings.
    QJsonObject meaning() const { return m_values.value("meaning").toObject(); }
    void setMeaning(const QJsonObject &chosen) { set("meaning", chosen); }
    bool meaningHosted() const { return meaning().value("hosted").toBool(false); }
    void setMeaningHosted(bool on) { setMeaningChoice("hosted", on); }
    bool meaningAll() const { return meaning().value("all").toBool(false); }
    void setMeaningAll(bool on) { setMeaningChoice("all", on); }
    // How a book is read aloud (#73): "engine" ("auto", "piper",
    // "speech-dispatcher" or "server"), "rate", "voice", "piper_program",
    // and the server's "url", "model" and "hosted"; see Voices.
    QJsonObject readAloud() const { return m_values.value("read_aloud").toObject(); }
    void setReadAloud(const QJsonObject &chosen) { set("read_aloud", chosen); }
    void setMeaningChoice(const QString &key, const QJsonValue &value) {
        QJsonObject chosen = meaning();
        if (value.isNull() || (value.isString() && value.toString().trimmed().isEmpty()))
            chosen.remove(key);
        else
            chosen.insert(key, value);
        setMeaning(chosen);
    }

    // How the knowledge graph of a book grows: "auto" (by itself, as chapters
    // are finished and with every catch-up, which costs AI calls), "ask"
    // (only when asked to: `omabook graph build`, or from the window) or
    // "off" (no graph is kept or used while reading).
    QString graph() const;
    void setGraph(const QString &mode);
    // Whether the knowledge graph is one tenant for the whole library, not
    // one per collection at its top (ADR 0003, Tenancy). Off by default: a
    // library split into, say, study and leisure keeps the two apart. Until
    // 0.1.8 it was "graph_across_collections", which still says it.
    bool graphOneTenant() const {
        return m_values.value("graph_one_tenant").toBool(m_values.value("graph_across_collections").toBool(false));
    }
    void setGraphOneTenant(bool on) { set("graph_one_tenant", on); }
    // The tenant the graph window showed last, by its global id
    // (Tenant::globalId), kept per library folder in "graph_tenants".
    QString graphTenant(const QString &library) const;
    void setGraphTenant(const QString &library, const QString &tenant);
    // When the things of the graph are marked in the text: "always" (the
    // default), "map" (only while the map of the book is beside the page)
    // or "never". Kept in config.json, so a choice lasts.
    QString marks() const;
    void setMarks(const QString &mode);
    // Whether a finger writes on book ink and a margin note by hand (#134):
    // "auto" (the default: until a pen is used, then it scrolls), "on" or
    // "off" (it always scrolls).
    QString inkFingers() const;
    void setInkFingers(const QString &mode);

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

    // How Omabook meets e-readers and phones (#71): the server, KOReader's
    // sync and sending books, as DeviceOptions reads them; all off unless set.
    QJsonObject devices() const { return m_values.value("devices").toObject(); }
    void setDevices(const QJsonObject &devices) { set("devices", devices); }

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
    int number(QAnyStringView key) const;
    void setNumber(QAnyStringView key, int value);
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
    QString m_sessionLibrary;
};
