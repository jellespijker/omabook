#include "ai.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <array>
#include <tuple>

#include "aiendpoint.h"
#include "omarchylocal.h"
#include "openaiprovider.h"
#include "secretstore.h"
#include "settings.h"

// Which AI answers: the choice of one, and the seam between which provider
// it is and how each one talks (ai.cpp, openaiprovider.cpp).
namespace {

// The harnesses Omarchy can make the default (omarchy-default-agent), by the
// names it gives them, and whether Omabook can run each for one answer with
// its tools off. The ones it can't either always run their tools unasked
// when not interactive, or have no documented way to be asked one question.
struct Harness {
    const char *id;
    const char *name;
    bool runs;
};
constexpr auto harnesses = std::to_array<Harness>({
    {"claude", "Claude Code", true},
    {"codex", "Codex", true},
    {"agy", "Antigravity", true},
    {"grok", "Grok", true},
    {"pi", "Pi", true},
    {"omp", "Oh My Pi", false},
    {"opencode", "OpenCode", false},
    {"ori", "Ori", false},
    {"openclaw", "OpenClaw", false},
    {"hermes", "Hermes", false},
    {"copilot", "GitHub Copilot", false},
    {"crush", "Crush", false},
    {"cursor-agent", "Cursor CLI", false},
    {"muse", "Muse Code", false},
    {"kimi", "Kimi", false},
    {"qwen", "Qwen Code", false},
    {"dsh", "DeepSeek Harness", false},
    {"mcode", "MiniMax Code", false},
    {"kiro-cli", "Kiro CLI", false},
    {"freebuff", "Freebuff", false},
    {"kilo", "Kilo", false},
    {"devin", "Devin CLI", false},
    {"amp", "Amp", false},
    {"commandcode", "Command Code", false},
});

AiChoice none(AiChoice choice, const QString &problem) {
    choice.source = AiChoice::None;
    choice.id.clear();
    choice.name.clear();
    choice.program.clear();
    choice.problem = problem;
    return choice;
}

AiChoice harness(AiChoice choice, AiChoice::Source source, const QString &id, const QString &program) {
    choice.source = source;
    choice.id = id;
    choice.name = Ai::agentName(id);
    choice.program = program;
    return choice;
}

// Why this one, as `omabook info --json` and the window say it.
constexpr const char *sourceName(AiChoice::Source source) noexcept {
    switch (source) {
    case AiChoice::None:
        return "none";
    case AiChoice::Setting:
        return "setting";
    case AiChoice::OmarchyDefault:
        return "omarchy";
    case AiChoice::Fallback:
        return "fallback";
    case AiChoice::Automatic:
        return "automatic";
    }
    return "none";
}

} // namespace

QJsonObject AiChoice::toJson() const {
    return {{"provider", id}, {"name", name}, {"source", sourceName(source)}, {"omarchy_default", omarchyDefault}, {"problem", problem}};
}

QString Ai::omarchyDefaultFile() { return QDir::homePath() + "/.config/omarchy/defaults/agent"; }

QString Ai::omarchyDefault(const QString &file) {
    QFile defaults(file.isEmpty() ? omarchyDefaultFile() : file);
    if (!defaults.open(QIODevice::ReadOnly))
        return {};
    // One word on the first line, as `read -r agent` takes it.
    return QString::fromUtf8(defaults.readLine(256)).trimmed();
}

QString Ai::agentName(const QString &id) {
    for (const Harness &known : harnesses)
        if (id == known.id)
            return known.name;
    return id;
}

bool Ai::canRun(const QString &id) {
    for (const Harness &known : harnesses)
        if (id == known.id)
            return known.runs;
    return false;
}

namespace {

AiChoice api(AiChoice choice, AiChoice::Source source) {
    choice.source = source;
    choice.id = "anthropic";
    choice.name = "Claude";
    return choice;
}

// The AI named in the settings, which is used or nothing is.
AiChoice named(AiChoice choice, const Settings &settings, const QString &wanted) {
    if (wanted == "anthropic")
        return qEnvironmentVariable("ANTHROPIC_API_KEY").isEmpty() ? none(choice, "Set ANTHROPIC_API_KEY to use the Anthropic API.")
                                                                   : api(choice, AiChoice::Setting);
    if (wanted == "openai") {
        // A server is used only when it is chosen, and then nothing else is.
        if (const QString missing = AiEndpoints::missing(settings); !missing.isEmpty())
            return none(choice, missing);
        choice.source = AiChoice::Setting;
        choice.id = "openai";
        choice.name = AiEndpoints::name(settings);
        choice.modelsUnder = AiEndpoints::modelsKey(settings);
        return choice;
    }
    if (wanted == "command") {
        if (settings.aiCommand().isEmpty())
            return none(choice, "No AI command is set in the settings.");
        choice.source = AiChoice::Setting;
        choice.id = "command";
        choice.program = QProcess::splitCommand(settings.aiCommand()).value(0);
        choice.name = QFileInfo(choice.program).fileName();
        return choice;
    }
    // A harness named outright.
    if (!Ai::canRun(wanted))
        return none(choice, "Omabook doesn't know an AI called \"" + wanted + "\"; see the settings.");
    const QString program = QStandardPaths::findExecutable(wanted);
    if (program.isEmpty())
        return none(choice, QString("%1 isn't installed (there is no %2 command).").arg(Ai::agentName(wanted), wanted));
    return harness(choice, AiChoice::Setting, wanted, program);
}

// Automatic: Omarchy's default agent, else the harnesses and the API. Never
// a server, so where the text goes doesn't change by itself.
AiChoice automatic(const AiChoice &choice) {
    const QString key = qEnvironmentVariable("ANTHROPIC_API_KEY");
    // Without Omarchy, as it always was: the API when there is a key, then
    // the harnesses.
    const QString claude = QStandardPaths::findExecutable("claude");
    const QString agy = QStandardPaths::findExecutable("agy");
    const QString nothing = "No AI is set up: install Claude Code or Antigravity and sign in, set ANTHROPIC_API_KEY, or "
                            "name a command in the settings.";
    if (choice.omarchyDefault.isEmpty()) {
        if (!key.isEmpty())
            return api(choice, AiChoice::Automatic);
        if (!claude.isEmpty())
            return harness(choice, AiChoice::Automatic, "claude", claude);
        if (!agy.isEmpty())
            return harness(choice, AiChoice::Automatic, "agy", agy);
        return none(choice, nothing);
    }

    // With Omarchy, its default agent is the companion.
    const QString named = Ai::agentName(choice.omarchyDefault);
    const QString program = Ai::canRun(choice.omarchyDefault) ? QStandardPaths::findExecutable(choice.omarchyDefault) : QString();
    if (!program.isEmpty())
        return harness(choice, AiChoice::OmarchyDefault, choice.omarchyDefault, program);
    const QString why = Ai::canRun(choice.omarchyDefault)
                            ? QString("Omarchy's default agent, %1, isn't installed.").arg(named)
                            : QString("Omabook can't run Omarchy's default agent, %1, with its tools off.").arg(named);
    AiChoice fallback = choice;
    if (!claude.isEmpty())
        fallback = harness(choice, AiChoice::Fallback, "claude", claude);
    else if (!agy.isEmpty())
        fallback = harness(choice, AiChoice::Fallback, "agy", agy);
    else if (!key.isEmpty())
        fallback = api(choice, AiChoice::Fallback);
    else
        return none(choice, why + " " + nothing);
    fallback.problem = why + " " + fallback.name + " answers instead.";
    return fallback;
}

} // namespace

AiChoice Ai::choose(const Settings &settings, const QString &defaultsFile) {
    AiChoice choice;
    choice.omarchyDefault = omarchyDefault(defaultsFile);
    const QString wanted = settings.aiProvider();
    if (wanted == "off")
        return none(choice, "AI is turned off in the settings.");
    return wanted == "auto" ? automatic(choice) : named(choice, settings, wanted);
}

std::unique_ptr<AiProvider> makeAiProvider(const Settings &settings, QString *why, const QString &defaultsFile) {
    const AiChoice choice = Ai::choose(settings, defaultsFile);
    if (why)
        *why = choice.problem;
    if (!choice.available())
        return nullptr;
    const AiModels models = Ai::models(settings, choice.modelsKey());
    std::unique_ptr<AiProvider> provider;
    if (choice.id == "openai") {
        // Omarchy's local models each have a server of their own: the main
        // model's is the one asked.
        const auto server = AiEndpoints::server(settings, models.main);
        if (!server) {
            if (why)
                *why = server.error();
            return nullptr;
        }
        provider = std::make_unique<OpenAiProvider>(*server);
    } else if (choice.id == "anthropic") {
        provider = std::make_unique<AnthropicProvider>(qEnvironmentVariable("ANTHROPIC_API_KEY"));
    } else if (choice.id == "command") {
        provider = std::make_unique<CommandProvider>(QProcess::splitCommand(settings.aiCommand()));
    } else if (choice.id == "agy") {
        provider = std::make_unique<AntigravityProvider>(choice.program);
    } else {
        provider = CommandProvider::agent(choice.id, choice.program);
    }
    if (provider)
        provider->setModels(models);
    return provider;
}

AiCatalog Ai::catalog(const QString &provider) {
    // Claude Code takes these aliases for the newest model of each family
    // (and any full model name); the API takes the names themselves.
    if (provider == "claude")
        return {{"haiku", "sonnet", "opus", "fable"}, {}, "haiku"};
    if (provider == "anthropic")
        return {{"claude-haiku-5-5", "claude-sonnet-5-5", "claude-opus-5-5", "claude-fable-5-2"},
                AnthropicProvider::defaultModel,
                "claude-haiku-5-5"};
    // From Codex's own list of models.
    if (provider == "codex")
        return {{"gpt-6-luna", "gpt-6.1-terra", "gpt-6.1-sol", "gpt-6-astra"}, {}, "gpt-6-luna"};
    // As `agy models` lists them: the effort is part of the name.
    if (provider == "agy")
        return {{"gemini-3.8-flash-low", "gemini-3.8-flash-medium", "gemini-3.8-flash-high", "gemini-3.1-pro-high", "gemini-4-argon-medium",
                 "gemini-4-hydrogen-medium", "gemini-4-carbon-medium"},
                {},
                "gemini-3.8-flash-medium"};
    // As `grok models` lists them; neither is known to be the small one.
    if (provider == "grok")
        return {{"grok-4.5", "grok-4.6"}, {}, {}};
    // Pi's models are those of whatever the user signed it in to, and a
    // command's are its own business.
    return {};
}

AiModels Ai::models(const Settings &settings, const QString &provider) {
    const AiCatalog known = catalog(provider);
    AiModels models;
    models.main = settings.aiModelFor(provider, "main");
    if (models.main.isEmpty())
        models.main = known.main;
    models.quick = settings.aiModelFor(provider, "quick");
    if (models.quick.isEmpty())
        models.quick = known.quick;
    // No small model known: quick work is done by the main one, not by a guess.
    if (models.quick.isEmpty())
        models.quick = models.main;
    return models;
}

QJsonObject Ai::describe(const Settings &settings, const QString &defaultsFile) {
    const AiChoice choice = choose(settings, defaultsFile);
    const AiModels chosen = models(settings, choice.modelsKey());
    QJsonObject object = choice.toJson();
    // Empty is the provider's own default.
    object.insert("main_model", choice.available() ? chosen.main : QString());
    object.insert("quick_model", choice.available() ? chosen.quick : QString());
    // Where a server is, and whether a key is set: never the key.
    if (settings.aiProvider() == "openai") {
        const QString url = AiEndpoints::baseUrl(settings);
        object.insert("server", QJsonObject{{"preset", AiEndpoints::preset(settings)},
                                            {"base_url", url},
                                            {"key", AiEndpoints::hasKey(settings)},
                                            {"unencrypted", AiEndpoints::inTheClear(url)}});
    }
    return object;
}

AiAgent::AiAgent(Settings *settings, const QString &defaultsFile, QObject *parent)
    : QObject(parent), m_settings(settings), m_file(defaultsFile.isEmpty() ? Ai::omarchyDefaultFile() : defaultsFile) {
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &AiAgent::reload);
    connect(m_settings, &Settings::changed, this, &AiAgent::reload);
    reload();
}

void AiAgent::reload() {
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);
    // Picking a default replaces the file, which drops it from the watch, so
    // re-arm after every change. Watching the nearest folder that exists
    // means choosing a first default, or installing Omarchy, is seen too.
    const QString folder = QFileInfo(m_file).absolutePath();
    QString ancestor = folder;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, folder, m_file};
    candidates.removeDuplicates();
    for (const QString &path : std::as_const(candidates))
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const AiChoice choice = Ai::choose(*m_settings, m_file);
    const QString key = choice.modelsKey();
    const AiModels models = Ai::models(*m_settings, key);
    const QStringList now{choice.program,
                          key,
                          models.main,
                          models.quick,
                          m_settings->aiModelFor(key, "main"),
                          m_settings->aiModelFor(key, "quick"),
                          serverPreset(),
                          serverUrl(),
                          serverHasKey() ? "key" : ""};
    if (choice.toJson() == m_choice.toJson() && now == m_shown)
        return;
    m_choice = choice;
    m_models = models;
    m_shown = now;
    // A server's models are its own: asked for whenever it is another one.
    if (choice.id == "openai" && m_listedFor != key)
        listModels();
    emit changed();
}

QString AiAgent::mainChoice() const { return m_settings->aiModelFor(m_choice.modelsKey(), "main"); }

QString AiAgent::quickChoice() const { return m_settings->aiModelFor(m_choice.modelsKey(), "quick"); }

void AiAgent::setModel(const QString &tier, const QString &name) {
    if (!m_choice.available() || (tier != "main" && tier != "quick"))
        return;
    // The settings say they changed, which is what brings this up to date.
    m_settings->setAiModelFor(m_choice.modelsKey(), tier, name);
}

QString AiAgent::source() const { return m_choice.toJson().value("source").toString(); }

QString AiAgent::omarchyDefaultName() const { return Ai::agentName(m_choice.omarchyDefault); }

bool AiAgent::omarchyDefaultRuns() const { return Ai::canRun(m_choice.omarchyDefault); }

std::unique_ptr<AiProvider> AiAgent::makeProvider(QString *why) const { return makeAiProvider(*m_settings, why, m_file); }

QStringList AiAgent::knownModels() const {
    // A server's are the ones it listed; the others' are known beforehand.
    return m_choice.id == "openai" ? m_listed : Ai::catalog(m_choice.id).known;
}

QString AiAgent::serverPreset() const { return AiEndpoints::preset(*m_settings); }

QString AiAgent::serverUrl() const { return AiEndpoints::baseUrl(*m_settings); }

bool AiAgent::serverHasKey() const { return m_settings->aiProvider() == "openai" && AiEndpoints::hasKey(*m_settings); }

bool AiAgent::serverInTheClear() const { return AiEndpoints::inTheClear(serverUrl()); }

bool AiAgent::omarchyLocal() const { return OmarchyLocal::installed(); }

QStringList AiAgent::elsewhereModels() const {
    QStringList elsewhere;
    std::ranges::copy_if(m_listed, std::back_inserter(elsewhere), &AiEndpoints::runsElsewhere);
    return elsewhere;
}

void AiAgent::setServer(const QString &preset, const QString &url) {
    QJsonObject endpoint{{"preset", preset}};
    // The preset's own address isn't kept, so a preset that moves is followed.
    const AiEndpoint *row = AiEndpoints::find(preset);
    if (const QString typed = url.trimmed(); !typed.isEmpty() && (!row || typed != QLatin1StringView(row->baseUrl)))
        endpoint.insert("base_url", typed);
    m_settings->setAiEndpoint(endpoint);
}

QString AiAgent::setServerKey(const QString &key) {
    const auto kept = AiEndpoints::keepKey(serverUrl(), key, Secrets::shared());
    // The keyring isn't the settings, so nothing else says it changed; with
    // another key the server may list other models.
    m_shown.clear();
    m_listedFor.clear();
    reload();
    return kept ? QString() : kept.error();
}

void AiAgent::listModels() {
    const int listing = ++m_listing;
    m_listedFor = m_choice.modelsKey();
    m_listed.clear();
    m_modelsProblem.clear();
    // The thread is handed all it needs: it never reads the settings. Each
    // of Omarchy's local models is a server of its own, so what is offered
    // there is the models it runs.
    const bool local = AiEndpoints::preset(*m_settings) == "omarchy-local";
    const auto server = local ? std::expected<AiServer, QString>() : AiEndpoints::server(*m_settings);
    // The future needn't be kept: its continuation is all that is wanted,
    // and it is dropped with this object.
    std::ignore = QtConcurrent::run([local, server] -> std::expected<QStringList, QString> {
                      if (!local)
                          return server ? OpenAiProvider::listModels(*server).transform_error(&Failure::text)
                                        : std::unexpected(server.error());
                      const auto running = OmarchyLocal::running();
                      if (!running)
                          return std::unexpected(running.error());
                      QStringList names;
                      for (const OmarchyLocal::Model &model : *running)
                          names.append(model.model);
                      names.removeDuplicates();
                      return names;
                  }).then(this, [this, listing](const std::expected<QStringList, QString> &listed) {
        if (listing != m_listing)
            return;
        m_listed = listed.value_or(QStringList());
        m_modelsProblem = listed ? QString() : listed.error();
        emit changed();
    });
}
