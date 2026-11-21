#pragma once

#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTimer>
#include <memory>
#include <optional>

#include "aicapability.h"
#include "failure.h"

class QNetworkReply;
class Settings;

struct AiMessage {
    QString role; // "user" or "assistant"
    QString text;
};

struct AiRequest {
    // What kind of work the request is, which decides the model that does it.
    // Main is for what a person reads: answers, catch-ups, suggestions, a
    // change to Omabook, whatever a plugin writes. Quick is for reading and
    // parsing into structure: the knowledge graph, judging look-ups, finding
    // series. A small, fast model does that well and for far less.
    enum class Tier { Main, Quick };
    using enum Tier; // AiRequest::Quick, as it always read

    QString system;
    QList<AiMessage> messages;
    Tier tier = Main;
    // A model named for this one request, over the tier's; see Ai::useModel.
    QString model;
    // The answer is for a person to read as it is written: the HTML-like
    // tags some models wrap one in are taken out (see AiCall, MarkupFilter).
    bool prose = false;
};

// The model each kind of work goes to, by the name the provider knows it
// under. Empty is the provider's own default.
struct AiModels {
    QString main;
    QString quick;
};

// A language model Omabook can ask, one request at a time. The answer arrives
// in pieces as it is written. Everything that uses AI (the reading companion,
// metadata from the book's own text, `omabook ask`) talks to this, so where
// the model runs is one setting and a new backend is one subclass.
class AiProvider : public QObject {
    Q_OBJECT

public:
    // Every request is written to the log when it is sent and when it ends:
    // which provider and model, its tier, how much was sent, how long it
    // took and how it ended. Never what was asked or answered.
    explicit AiProvider(QObject *parent = nullptr);

    // What to call it in the window, such as "Claude" or "ollama".
    virtual QString name() const = 0;
    virtual void ask(const AiRequest &request) = 0;
    virtual void cancel() = 0;

    // Which model each tier of request goes to; see Ai::models.
    void setModels(const AiModels &models) { m_models = models; }
    AiModels models() const { return m_models; }
    // The model this request is for: the one it names, else the one named
    // for the whole run (Ai::ModelOverride), else its tier's.
    QString modelFor(const AiRequest &request) const;

signals:
    void delta(const QString &text);
    void finished();
    // Why there is no answer, by a code (ADR 0005), worded where it is shown.
    void failed(const Failure &failure);
    // Something the asker should know although the answer came: a model that
    // was refused and which one answered instead.
    void noted(const Failure &note);

protected:
    // Called by each provider as it sends a request (also a retry).
    void began(const AiRequest &request, const QString &model);

    // A model that was refused must never break reading. After a failure
    // that is about the model (AiFailures::aboutTheModel), before anything was
    // answered, this is the model to ask once more: the main one, or the
    // provider's default when it was the main one that was refused. Nothing
    // when there is none to try.
    [[nodiscard]] std::optional<QString> modelToRetry(const QString &refused, const Failure &failure) const;
    // What is noted when another model answered in place of a refused one.
    static Failure retryNote(const QString &refused, const QString &instead, const AiRequest &request);

private:
    AiModels m_models;
    qint64 m_began = 0;     // when the request under way was sent, in ms
    qsizetype m_sent = 0;   // how many characters it held
    qsizetype m_received = 0;
};

// Claude through the Anthropic API, with the key from ANTHROPIC_API_KEY.
class AnthropicProvider : public AiProvider {
    Q_OBJECT

public:
    static constexpr auto defaultModel = "claude-opus-5-5";

    AnthropicProvider(const QString &apiKey, const QString &model = {}, const QString &baseUrl = {},
                      QObject *parent = nullptr);

    QString name() const override { return "Claude"; }
    void ask(const AiRequest &request) override;
    void cancel() override;

private:
    void read();
    void done();

    void send(const QString &model);

    QString m_apiKey, m_model, m_baseUrl;
    AiRequest m_request;
    QString m_sent; // the model the request in hand went to
    bool m_answered = false;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer, m_body;
    QString m_stopReason;
    QJsonObject m_streamError; // an error event in the stream
};

// Any program that reads a prompt on standard input and writes its answer to
// standard output: `claude -p`, `ollama run llama3`, `llm`, a script. A
// program that takes its prompt from a file instead names it "{prompt-file}"
// among its arguments.
class CommandProvider : public AiProvider {
    Q_OBJECT

public:
    // The program and its arguments; a command typed in the settings is
    // split the way a shell would with QProcess::splitCommand.
    explicit CommandProvider(const QStringList &command, QObject *parent = nullptr);

    // The agent harnesses Omarchy knows, each run for one answer and nothing
    // else: the book's text is material to talk about and must never be able
    // to make an agent act. Claude Code and Pi have a switch for "no tools"
    // and one for "keep no session". Codex has no such switch, so it runs
    // without the user's config (which is where its MCP servers are), in its
    // read-only sandbox, keeping no session. Grok has neither, so it runs in
    // plan mode, where it may not change anything, with no subagents or web.
    // Those two are also started in an empty folder (see agent()).
    static QStringList claudeCommand(const QString &program);
    static QStringList codexCommand(const QString &program);
    static QStringList grokCommand(const QString &program);
    static QStringList piCommand(const QString &program);

    // The provider for one of those harnesses by its Omarchy id ("claude",
    // "codex", "grok", "pi"), set up as above; null for any other id.
    static std::unique_ptr<CommandProvider> agent(const QString &id, const QString &program);

    // What to call it, when the program's file name isn't it.
    void setDisplayName(const QString &name) { m_name = name; }
    // Runs the program in a folder of its own with nothing in it, for
    // harnesses that can read files whatever they are told.
    void setEmptyFolder(bool empty) { m_emptyFolder = empty; }
    // Omarchy lets several subscriptions of one harness be signed in and
    // picks one for new sessions. `helper` (omarchy-agent-account-home) is
    // asked which, and its answer goes to the program as `variable`
    // (CLAUDE_CONFIG_DIR, CODEX_HOME, GROK_HOME) unless that is already set.
    void setAccount(const QString &helper, const QString &agentId, const QString &variable);
    // How the program is told which model to use: this flag and the name
    // are added to its arguments ("--model" for every harness Omabook runs).
    // Without one, as for a command of the user's own, the model and the tier
    // reach the program as OMABOOK_AI_MODEL and OMABOOK_AI_TIER.
    void setModelFlag(const QString &flag) { m_modelFlag = flag; }

    QString name() const override;
    void ask(const AiRequest &request) override;
    void cancel() override;

private:
    void start(const QString &accountHome);
    void begin();

    QStringList m_command;
    QString m_name;
    QString m_prompt;
    QString m_modelFlag;
    AiRequest m_request;
    QString m_model;
    // With a model named, the first words are held back until it is plain
    // they are an answer: a harness that refuses a model says so on standard
    // output, briefly, and then stops with an error.
    QString m_held;
    bool m_answered = false;
    bool m_emptyFolder = false;
    QString m_accountHelper, m_accountAgent, m_accountVariable;
    std::unique_ptr<QTemporaryDir> m_folder;
    std::unique_ptr<QTemporaryFile> m_promptFile;
    QProcess m_account;
    QProcess m_process;
    bool m_cancelled = false;
};

// Google's Antigravity CLI (agy), signed in with its own subscription. It
// has no switch for "no tools", so it runs in plan mode (it may not change
// anything), sandboxed, in an empty folder, with nobody to approve a tool.
class AntigravityProvider : public AiProvider {
    Q_OBJECT

public:
    explicit AntigravityProvider(const QString &program, QObject *parent = nullptr);

    QString name() const override { return "Antigravity"; }
    void ask(const AiRequest &request) override;
    void cancel() override;

private:
    void read();
    void start(const QString &model);

    AiRequest m_request;
    QString m_model;
    QString m_program;
    QTemporaryDir m_emptyFolder;
    QProcess m_process;
    QByteArray m_buffer;
    QString m_error;
    bool m_answered = false;
    bool m_cancelled = false;
};

// Which model answers, and why that one. Omabook's own setting decides:
//
//   "auto"       Omarchy's default agent (~/.config/omarchy/defaults/agent,
//                set with `omarchy default agent <name>`), when Omabook can
//                run it for one answer with its tools off. Where it can't,
//                or it isn't installed: Claude Code, then Antigravity, then
//                the Anthropic API when ANTHROPIC_API_KEY is set. Without
//                Omarchy (no such file) the order is the one Omabook always
//                had: the API with a key, then Claude Code, then Antigravity.
//   "claude", "codex", "agy", "grok", "pi"   that harness, with the
//                subscription its own command line is signed in with
//   "anthropic"  the API, with the key from ANTHROPIC_API_KEY
//   "openai"     a server that speaks the OpenAI-compatible API, the one
//                `ai_endpoint` names (see AiEndpoints). Automatic never picks
//                one by itself: where the text goes changes only when asked.
//   "command"    the command in the settings
//   "off"        none
struct AiChoice {
    enum class Source {
        None,           // no model: `problem` says why
        Setting,        // named in Omabook's settings
        OmarchyDefault, // automatic, and Omarchy's default agent
        Fallback,       // automatic, but Omarchy's default couldn't be used: `problem` says why
        Automatic,      // automatic, and Omarchy names no default
    };
    using enum Source; // AiChoice::Fallback, as it always read
    Source source = None;
    QString id;             // "claude", "codex", "agy", "grok", "pi", "anthropic", "openai", "command"; empty for none
    QString name;           // "Claude Code"
    QString program;        // the harness's executable, when it is one
    QString omarchyDefault; // the id Omarchy names, when it names one
    QString problem;        // English
    CapabilityLack lack = CapabilityLack::None; // why there is none, as a code
    QString modelsUnder;    // for "openai": the server's own, as AiEndpoints::modelsKey says

    bool available() const noexcept { return source != None; }
    // What `ai_models` keeps this AI's two models under: its id, or the server's.
    QString modelsKey() const { return modelsUnder.isEmpty() ? id : modelsUnder; }
    // {"provider", "name", "source", "omarchy_default", "problem"}, as `omabook info --json` reports it.
    QJsonObject toJson() const;
};

// What is known of the models a provider serves: the names to offer in the
// settings, and the one for quick work when the user named none. Every name
// here was read from the provider's own list or documentation; where no
// small model is known, `quick` is empty and quick work goes to the main one.
struct AiCatalog {
    QStringList known; // from the smallest up
    QString main;      // what is used when none is chosen; empty for the provider's own default
    QString quick;
};

namespace Ai {

// Where Omarchy keeps its default agent's id.
QString omarchyDefaultFile();
// The id in that file, or empty when there is no file or it names none.
QString omarchyDefault(const QString &file = {});
// "Claude Code" for "claude": the names Omarchy gives its harnesses. An id it
// doesn't know is its own name.
QString agentName(const QString &id);
// Whether Omabook can run this harness for one answer with its tools off.
bool canRun(const QString &id);

// Tests pass their own defaults file; the harnesses are looked for on PATH.
AiChoice choose(const Settings &settings, const QString &defaultsFile = {});

// The models known for a provider id ("claude", "anthropic", "codex", ...).
AiCatalog catalog(const QString &provider);
// The model each tier goes to with this provider: what the settings name for
// it, else the catalog's. Quick with nothing named and nothing known is main.
AiModels models(const Settings &settings, const QString &provider);
// "main" and "quick" choose a tier; anything else names a model outright.
// This is what --model, a tool's "model" and a plugin's {model} all take.
void useModel(AiRequest &request, const QString &word);
// What `omabook info --json` says of the AI: the choice, and both models.
QJsonObject describe(const Settings &settings, const QString &defaultsFile = {});
// "for answers and writing" or "for reading and parsing".
QString tierName(AiRequest::Tier tier);

// For one run of a command or one call of a tool: every request made while
// this lives uses the model it names ("main", "quick" or a model's name),
// unless the request names its own. Empty changes nothing.
class ModelOverride {
public:
    explicit ModelOverride(const QString &word);
    ~ModelOverride();
    ModelOverride(const ModelOverride &) = delete;
    ModelOverride &operator=(const ModelOverride &) = delete;

private:
    QString m_before;
};

} // namespace Ai

// The provider for that choice. Null, with `why` in English, when AI is off
// or nothing is set up.
std::unique_ptr<AiProvider> makeAiProvider(const Settings &settings, QString *why = nullptr, const QString &defaultsFile = {});

// Which model answers, for the window: it follows Omabook's settings and
// Omarchy's default agent while the app runs, so `omarchy default agent codex`
// switches the companion without a restart.
class AiAgent : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id NOTIFY changed)
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // Why this one: "omarchy" (Omarchy's default agent), "setting" (named in
    // the settings), "fallback" (Omarchy's default couldn't be used),
    // "automatic" (Omarchy names none) or "none". The window words it.
    Q_PROPERTY(QString source READ source NOTIFY changed)
    // What Omarchy calls its default agent, when it names one.
    Q_PROPERTY(QString omarchyDefaultName READ omarchyDefaultName NOTIFY changed)
    // Whether Omarchy's default is a harness Omabook can run with its tools off.
    Q_PROPERTY(bool omarchyDefaultRuns READ omarchyDefaultRuns NOTIFY changed)
    // The model picker. `knownModels` are the names to offer for the provider
    // in use; a choice is what the user picked ("" for the default); a
    // default is what is used then ("" for the provider's own); and
    // `mainModel` and `quickModel` are what each kind of work goes to.
    Q_PROPERTY(QStringList knownModels READ knownModels NOTIFY changed)
    Q_PROPERTY(QString mainChoice READ mainChoice NOTIFY changed)
    Q_PROPERTY(QString quickChoice READ quickChoice NOTIFY changed)
    Q_PROPERTY(QString mainDefault READ mainDefault NOTIFY changed)
    Q_PROPERTY(QString quickDefault READ quickDefault NOTIFY changed)
    Q_PROPERTY(QString mainModel READ mainModel NOTIFY changed)
    Q_PROPERTY(QString quickModel READ quickModel NOTIFY changed)
    // A server (the "openai" choice): its preset and address, whether a key
    // is set for it, whether text goes to it unencrypted, and whether
    // Omarchy's local models are here to be offered. `elsewhereModels` are
    // those it lists that send the text on to another service (`...:cloud`),
    // and `modelsProblem` why it listed none.
    Q_PROPERTY(QString serverPreset READ serverPreset NOTIFY changed)
    Q_PROPERTY(QString serverUrl READ serverUrl NOTIFY changed)
    Q_PROPERTY(bool serverHasKey READ serverHasKey NOTIFY changed)
    Q_PROPERTY(bool serverInTheClear READ serverInTheClear NOTIFY changed)
    Q_PROPERTY(bool omarchyLocal READ omarchyLocal NOTIFY changed)
    Q_PROPERTY(QStringList elsewhereModels READ elsewhereModels NOTIFY changed)
    Q_PROPERTY(QString modelsProblem READ modelsProblem NOTIFY changed)

public:
    explicit AiAgent(Settings *settings, const QString &defaultsFile = {}, QObject *parent = nullptr);

    AiChoice choice() const { return m_choice; }
    QString id() const { return m_choice.id; }
    QString name() const { return m_choice.name; }
    bool available() const { return m_choice.available(); }
    QString source() const;
    QString omarchyDefaultName() const;
    bool omarchyDefaultRuns() const;
    QStringList knownModels() const;
    QString mainChoice() const;
    QString quickChoice() const;
    QString mainDefault() const { return Ai::catalog(m_choice.modelsKey()).main; }
    QString quickDefault() const { return Ai::catalog(m_choice.modelsKey()).quick; }
    QString mainModel() const { return m_models.main; }
    QString quickModel() const { return m_models.quick; }
    // Chooses the model for "main" or "quick" with the provider in use; an
    // empty name goes back to the default.
    Q_INVOKABLE void setModel(const QString &tier, const QString &name);

    QString serverPreset() const;
    QString serverUrl() const;
    bool serverHasKey() const;
    bool serverInTheClear() const;
    bool omarchyLocal() const;
    QStringList elsewhereModels() const;
    QString modelsProblem() const { return m_modelsProblem; }
    // Chooses the server: a preset, and the address typed in ("" for the
    // preset's own). Its models are asked for again.
    Q_INVOKABLE void setServer(const QString &preset, const QString &url);
    // Keeps the key for the server in use in the keyring; "" forgets it.
    // Says why when it couldn't be kept, else "".
    Q_INVOKABLE QString setServerKey(const QString &key);

    std::unique_ptr<AiProvider> makeProvider(QString *why = nullptr) const;

signals:
    void changed();

private:
    void reload();
    // Asks the server in use for its models, on a thread of its own.
    void listModels();

    Settings *m_settings;
    QString m_file;
    AiChoice m_choice;
    AiModels m_models;
    QStringList m_shown; // what the picker shows, to tell when it changed
    QString m_listedFor; // the server `m_listed` came from
    QStringList m_listed;
    QString m_modelsProblem;
    int m_listing = 0; // which listing is the latest, so an older one's answer is dropped
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
};
