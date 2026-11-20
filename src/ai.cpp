#include "ai.h"

#include "log.h"

#include <QDateTime>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <algorithm>
#include <array>

#include "settings.h"

namespace {

using namespace Qt::StringLiterals;

// Models that can hand a declined request to another model inside the same call.
bool supportsFallbacks(const QString &model) {
    static constexpr std::array models{"claude-opus-5-5"_L1, "claude-opus-5"_L1, "claude-sonnet-5-5"_L1, "claude-fable-5-2"_L1,
                                       "claude-fable-5-1"_L1};
    return std::ranges::any_of(models, [&model](QLatin1StringView known) { return model == known; });
}

// The model named for the whole run of a command or call of a tool; see
// Ai::ModelOverride. Asked from the threads that do the asking.
QMutex overrideLock;
QString overrideWord;

QString currentOverride() {
    const QMutexLocker locker(&overrideLock);
    return overrideWord;
}

} // namespace

QString AiProvider::modelFor(const AiRequest &request) const {
    if (!request.model.isEmpty())
        return request.model;
    const QString word = currentOverride();
    if (!word.isEmpty() && word != "main" && word != "quick")
        return word;
    const bool quick = word.isEmpty() ? request.tier == AiRequest::Quick : word == "quick";
    return quick ? m_models.quick : m_models.main;
}

std::optional<QString> AiProvider::modelToRetry(const QString &refused, const QString &error) const {
    // Only a failure that is about the model: one about the network or a
    // limit would fail the same way again, on a dearer model.
    if (refused.isEmpty() || !error.contains("model", Qt::CaseInsensitive))
        return std::nullopt;
    const QString instead = refused != m_models.main ? m_models.main : QString();
    if (instead == refused)
        return std::nullopt;
    return instead;
}

QString AiProvider::withModel(const QString &error, const QString &model, const AiRequest &request) {
    if (model.isEmpty() || !error.contains("model", Qt::CaseInsensitive))
        return error;
    return QString("%1 (The model was \"%2\", %3.)").arg(error, model, Ai::tierName(request.tier));
}

QString AiProvider::retryNote(const QString &refused, const QString &instead, const AiRequest &request) {
    return QString("The model \"%1\", chosen %2, was refused, so %3 answered instead. Check its name in the settings.")
        .arg(refused, Ai::tierName(request.tier), instead.isEmpty() ? QString("the default model") : "\"" + instead + "\"");
}

AnthropicProvider::AnthropicProvider(const QString &apiKey, const QString &model, const QString &baseUrl, QObject *parent)
    : AiProvider(parent),
      m_apiKey(apiKey),
      m_model(model.isEmpty() ? QString(defaultModel) : model),
      m_baseUrl(baseUrl.isEmpty() ? QStringLiteral("https://api.anthropic.com") : baseUrl) {}

AiProvider::AiProvider(QObject *parent) : QObject(parent) {
    connect(this, &AiProvider::delta, this, [this](const QString &text) { m_received += text.size(); });
    connect(this, &AiProvider::finished, this, [this] {
        qCInfo(lcAi).noquote() << name() << "answered:" << m_received << "characters in"
                               << (QDateTime::currentMSecsSinceEpoch() - m_began) << "ms";
    });
    connect(this, &AiProvider::failed, this, [this](const QString &error) {
        qCWarning(lcAi).noquote() << name() << "failed after" << (QDateTime::currentMSecsSinceEpoch() - m_began) << "ms:" << error;
    });
    connect(this, &AiProvider::noted, this, [this](const QString &note) { qCInfo(lcAi).noquote() << name() << note; });
}

void AiProvider::began(const AiRequest &request, const QString &model) {
    m_began = QDateTime::currentMSecsSinceEpoch();
    m_received = 0;
    m_sent = request.system.size();
    for (const AiMessage &message : request.messages)
        m_sent += message.text.size();
    qCInfo(lcAi).noquote() << name() << "asked:" << (request.tier == AiRequest::Quick ? "quick" : "main") << "tier,"
                           << (model.isEmpty() ? QString("its default model") : "model " + model) << "," << m_sent << "characters";
}

void AnthropicProvider::ask(const AiRequest &request) {
    cancel();
    m_request = request;
    m_answered = false;
    send(modelFor(request));
}

void AnthropicProvider::send(const QString &named) {
    const AiRequest &request = m_request;
    m_sent = named;
    // No model named for this kind of work is the one this was made with.
    const QString model = named.isEmpty() ? m_model : named;
    began(request, model);
    QJsonArray messages;
    for (const AiMessage &message : request.messages)
        messages.append(QJsonObject{{"role", message.role}, {"content", message.text}});
    QJsonObject body{
        {"model", model},
        // Streamed, so a long answer has room and no request times out.
        {"max_tokens", 64000},
        {"stream", true},
        {"system", request.system},
        {"messages", messages},
        // Thinking is adaptive by default; a reading companion is a chat, not
        // a research task, so it gets the effort between the two.
        {"output_config", QJsonObject{{"effort", "medium"}}},
        // A follow-up question re-sends the same excerpt: cache it.
        {"cache_control", QJsonObject{{"type", "ephemeral"}}},
    };
    QNetworkRequest http(QUrl(m_baseUrl + "/v1/messages"));
    http.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    http.setRawHeader("x-api-key", m_apiKey.toUtf8());
    http.setRawHeader("anthropic-version", "2026-10-21");
    if (supportsFallbacks(model)) {
        // A request a safety classifier declines is answered by another model.
        body.insert("fallbacks", "default");
        http.setRawHeader("anthropic-beta", "server-side-fallback-2026-07-01");
    }
    m_buffer.clear();
    m_body.clear();
    m_stopReason.clear();
    m_streamError.clear();
    m_reply = m_network.post(http, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &AnthropicProvider::read);
    connect(m_reply, &QNetworkReply::finished, this, &AnthropicProvider::done);
}

void AnthropicProvider::cancel() {
    if (!m_reply)
        return;
    QNetworkReply *reply = m_reply;
    m_reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

// Server-sent events: each `data:` line is one JSON event.
void AnthropicProvider::read() {
    const QByteArray chunk = m_reply->readAll();
    if (m_body.size() < 65536)
        m_body += chunk; // an error arrives as a plain JSON body instead
    m_buffer += chunk;
    for (qsizetype end; (end = m_buffer.indexOf('\n')) >= 0;) {
        const QByteArray line = m_buffer.left(end).trimmed();
        m_buffer.remove(0, end + 1);
        if (!line.startsWith("data:"))
            continue;
        const QJsonObject event = QJsonDocument::fromJson(line.mid(5)).object();
        const QString type = event.value("type").toString();
        if (type == "content_block_delta") {
            const QJsonObject change = event.value("delta").toObject();
            if (change.value("type").toString() == "text_delta") {
                m_answered = true;
                emit delta(change.value("text").toString());
            }
        } else if (type == "message_delta") {
            const QString reason = event.value("delta").toObject().value("stop_reason").toString();
            if (!reason.isEmpty())
                m_stopReason = reason;
        } else if (type == "error") {
            m_streamError = event.value("error").toObject().value("message").toString();
        }
    }
}

void AnthropicProvider::done() {
    QNetworkReply *reply = m_reply;
    if (!reply)
        return;
    read();
    m_reply = nullptr;
    reply->deleteLater();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString problem;
    if (status != 200) {
        const QJsonObject error = QJsonDocument::fromJson(m_body).object().value("error").toObject();
        const QString message = error.value("message").toString();
        if (status == 401)
            problem = "The Anthropic API didn't accept the key in ANTHROPIC_API_KEY.";
        else if (status == 404 && error.value("type").toString() == "not_found_error")
            problem = "The Anthropic API knows no such model: " + message;
        else if (!message.isEmpty())
            problem = "The Anthropic API answered: " + message;
        else if (status == 0)
            problem = "The Anthropic API can't be reached: " + reply->errorString();
        else
            problem = QString("The Anthropic API answered with an error (%1).").arg(status);
    } else if (!m_streamError.isEmpty()) {
        problem = "The Anthropic API answered: " + m_streamError;
    } else if (m_stopReason == "refusal") {
        // Not about which model it was, so not something another try mends.
        emit failed("The model declined to answer this.");
        return;
    }
    if (problem.isEmpty()) {
        emit finished();
        return;
    }
    if (const auto next = !m_answered && status != 401 ? modelToRetry(m_sent, problem) : std::nullopt) {
        emit noted(retryNote(m_sent, *next, m_request));
        send(*next);
        return;
    }
    emit failed(withModel(problem, m_sent, m_request));
}

CommandProvider::CommandProvider(const QStringList &command, QObject *parent) : AiProvider(parent), m_command(command) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] {
        const QString text = QString::fromUtf8(m_process.readAllStandardOutput());
        if (m_model.isEmpty() || m_answered) {
            m_answered = m_answered || !text.trimmed().isEmpty();
            emit delta(text);
            return;
        }
        // Longer than any refusal: it is an answer, and from here it streams.
        m_held += text;
        if (m_held.size() > 600) {
            m_answered = true;
            emit delta(m_held);
            m_held.clear();
        }
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_cancelled)
            return;
        const QString errors = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
        const QString held = m_held;
        m_held.clear();
        if (code == 0 && status == QProcess::NormalExit) {
            if (!held.isEmpty())
                emit delta(held);
            emit finished();
            return;
        }
        // What it said in place of an answer is the reason, where it gave one.
        const QString said = held.trimmed().isEmpty() ? errors : held.trimmed();
        const QString problem = said.isEmpty() ? name() + " stopped with an error." : name() + ": " + said.right(400);
        // A model the harness doesn't take is asked for once more without it.
        if (const auto next = !m_answered ? modelToRetry(m_model, problem) : std::nullopt) {
            emit noted(retryNote(m_model, *next, m_request));
            m_model = *next;
            begin();
            return;
        }
        emit failed(withModel(problem, m_model, m_request));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit failed("The AI command can't be started: " + m_command.value(0));
    });
    // The account is asked for first, and the answer starts the program. A
    // helper that fails or says nothing means the harness's own default.
    connect(&m_account, &QProcess::finished, this, [this] {
        if (!m_cancelled)
            start(QString::fromUtf8(m_account.readAllStandardOutput()).trimmed());
    });
    connect(&m_account, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && !m_cancelled)
            start({});
    });
}

QStringList CommandProvider::claudeCommand(const QString &program) {
    // The empty argument is what turns the tools off, so this is a list: a
    // command line would lose it when split.
    return {program, "-p", "--tools", "", "--strict-mcp-config", "--no-session-persistence"};
}

QStringList CommandProvider::codexCommand(const QString &program) {
    // "-" has the prompt read from standard input. Only the last message is
    // written to standard output; what Codex does on the way goes to
    // standard error.
    return {program, "exec", "--skip-git-repo-check", "--ephemeral", "--ignore-user-config", "--sandbox", "read-only",
            "--color", "never", "-"};
}

QStringList CommandProvider::grokCommand(const QString &program) {
    // Grok takes a single-turn prompt as an argument or from a file, and a
    // chapter is far longer than an argument may be.
    return {program, "--prompt-file", "{prompt-file}", "--output-format", "plain", "--permission-mode", "plan",
            "--no-subagents", "--disable-web-search"};
}

QStringList CommandProvider::piCommand(const QString &program) {
    return {program, "-p", "--no-session", "--no-tools", "--no-mcp", "--no-extensions", "--no-skills",
            "--no-prompt-templates", "--no-context-files", "@{prompt-file}"};
}

std::unique_ptr<CommandProvider> CommandProvider::agent(const QString &id, const QString &program) {
    const QStringList command = id == "claude"  ? claudeCommand(program)
                                : id == "codex" ? codexCommand(program)
                                : id == "grok"  ? grokCommand(program)
                                : id == "pi"    ? piCommand(program)
                                                : QStringList();
    if (command.isEmpty())
        return nullptr;
    auto provider = std::make_unique<CommandProvider>(command);
    // The four of them take a model the same way, by alias or full name.
    provider->setModelFlag("--model");
    provider->setDisplayName(Ai::agentName(id));
    provider->setEmptyFolder(id == "codex" || id == "grok");
    const QString variable = id == "claude" ? "CLAUDE_CONFIG_DIR" : id == "codex" ? "CODEX_HOME" : id == "grok" ? "GROK_HOME" : "";
    if (!variable.isEmpty())
        provider->setAccount(QStandardPaths::findExecutable("omarchy-agent-account-home"), id, variable);
    return provider;
}

void CommandProvider::setAccount(const QString &helper, const QString &agentId, const QString &variable) {
    m_accountHelper = helper;
    m_accountAgent = agentId;
    m_accountVariable = variable;
}

QString CommandProvider::name() const { return m_name.isEmpty() ? QFileInfo(m_command.value(0)).fileName() : m_name; }

void CommandProvider::ask(const AiRequest &request) {
    cancel();
    m_cancelled = false;
    if (m_command.isEmpty()) {
        emit failed("No AI command is set.");
        return;
    }
    // One prompt: the instructions, then the conversation.
    m_prompt = request.system + "\n";
    for (const AiMessage &message : request.messages)
        m_prompt += (request.messages.size() > 1 ? (message.role == "user" ? "\nReader:\n" : "\nYou answered:\n") : "\n")
                    + message.text + "\n";
    m_request = request;
    m_model = modelFor(request);
    m_answered = false;
    m_held.clear();
    begin();
}

void CommandProvider::begin() {
    // An account the user's environment already names is theirs to choose.
    if (m_accountHelper.isEmpty() || qEnvironmentVariableIsSet(qPrintable(m_accountVariable)))
        start({});
    else
        m_account.start(m_accountHelper, {m_accountAgent});
}

void CommandProvider::start(const QString &accountHome) {
    began(m_request, m_model);
    QStringList arguments = m_command;
    const QString program = arguments.takeFirst();
    // Before the last argument, which for some is where the prompt comes from.
    if (!m_modelFlag.isEmpty() && !m_model.isEmpty()) {
        const qsizetype at = qMax<qsizetype>(0, arguments.size() - 1);
        arguments.insert(at, m_model);
        arguments.insert(at, m_modelFlag);
    }
    const QString placeholder = "{prompt-file}";
    bool fromFile = false;
    for (QString &argument : arguments) {
        if (!argument.contains(placeholder))
            continue;
        if (!fromFile) {
            // Named .md so a harness that goes by the extension reads it as text.
            m_promptFile = std::make_unique<QTemporaryFile>(QDir::tempPath() + "/omabook-prompt-XXXXXX.md");
            if (!m_promptFile->open() || m_promptFile->write(m_prompt.toUtf8()) < 0 || !m_promptFile->flush()) {
                emit failed("The question can't be written to a temporary file.");
                return;
            }
            fromFile = true;
        }
        argument.replace(placeholder, m_promptFile->fileName());
    }
    if (m_emptyFolder) {
        m_folder = std::make_unique<QTemporaryDir>();
        m_process.setWorkingDirectory(m_folder->path());
    }
    // Set afresh for every question: the account may have been switched since.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!accountHome.isEmpty())
        environment.insert(m_accountVariable, accountHome);
    // For a program of the user's own, which has no flag Omabook knows of.
    environment.insert("OMABOOK_AI_TIER", m_request.tier == AiRequest::Quick ? "quick" : "main");
    environment.insert("OMABOOK_AI_MODEL", m_model);
    m_process.setProcessEnvironment(environment);
    m_process.start(program, arguments);
    if (!fromFile)
        m_process.write(m_prompt.toUtf8());
    m_process.closeWriteChannel();
}

void CommandProvider::cancel() {
    m_cancelled = true;
    for (QProcess *process : {&m_account, &m_process}) {
        if (process->state() == QProcess::NotRunning)
            continue;
        process->kill();
        process->waitForFinished(1000);
    }
}

AntigravityProvider::AntigravityProvider(const QString &program, QObject *parent) : AiProvider(parent), m_program(program) {
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &AntigravityProvider::read);
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (m_cancelled)
            return;
        read();
        const QString problem = QString::fromUtf8(m_process.readAllStandardError()).trimmed();
        // A turn that only reached for a tool says so in Antigravity's terms
        // (settings files and allow-rules), which mean nothing here.
        const bool reachedForTool = m_error.contains("permission", Qt::CaseInsensitive) && !m_answered;
        const QString failure = reachedForTool ? QString("Antigravity tried to use one of its tools instead of answering, and Omabook doesn't "
                                                         "let it. Asking again usually works; choosing another AI in the settings always does.")
                                : !m_error.isEmpty() ? "Antigravity: " + m_error
                                : code == 0 && status == QProcess::NormalExit && m_answered ? QString()
                                : problem.isEmpty() ? QString("Antigravity gave no answer. Run agy once in a terminal to sign in.")
                                                    : "Antigravity: " + problem.right(400);
        if (failure.isEmpty()) {
            emit finished();
            return;
        }
        if (const auto next = !m_answered ? modelToRetry(m_model, failure) : std::nullopt) {
            emit noted(retryNote(m_model, *next, m_request));
            start(*next);
            return;
        }
        emit failed(withModel(failure, m_model, m_request));
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            emit failed("Antigravity can't be started: " + m_program);
    });
}

void AntigravityProvider::ask(const AiRequest &request) {
    cancel();
    m_cancelled = false;
    m_request = request;
    start(modelFor(request));
}

void AntigravityProvider::start(const QString &model) {
    const AiRequest &request = m_request;
    m_model = model;
    began(request, model);
    m_answered = false;
    m_buffer.clear();
    m_error.clear();
    // Antigravity is an agent: given a task it reaches for its tools (a
    // shell, files), which headless mode refuses, and a turn that only tried
    // a tool ends with no answer at all. Omabook hands it everything it
    // needs, so it is told it has no tools and is to answer in words.
    QString prompt = "You have no tools in this conversation: do not run commands, read or write files, or call any tool. "
                     "Everything you need is in this message; answer in text only.\n\n"
                     + request.system + "\n";
    for (const AiMessage &message : request.messages)
        prompt += (request.messages.size() > 1 ? (message.role == "user" ? "\nReader:\n" : "\nYou answered:\n") : "\n")
                  + message.text + "\n";
    // The prompt goes in as one line of JSON on standard input: a chapter is
    // far longer than a command line argument may be.
    const QJsonObject turn{{"event", "user"}, {"message", QJsonObject{{"role", "user"}, {"content", prompt}}}};
    m_process.setWorkingDirectory(m_emptyFolder.path());
    // Antigravity's models carry their effort in their names
    // (gemini-3.8-flash-medium), so naming one replaces the effort asked for.
    const QStringList which = model.isEmpty() ? QStringList{"--effort", "low"} : QStringList{"--model", model};
    m_process.start(m_program, QStringList{"--mode", "plan", "--sandbox"} + which
                                   + QStringList{"--input-format", "stream-json", "--output-format", "stream-json", "-p="});
    m_process.write(QJsonDocument(turn).toJson(QJsonDocument::Compact) + "\n");
    m_process.closeWriteChannel();
}

// One JSON event per line: the answer arrives as text deltas of its response
// steps, and the last event says whether the turn worked.
void AntigravityProvider::read() {
    m_buffer += m_process.readAllStandardOutput();
    for (qsizetype end; (end = m_buffer.indexOf('\n')) >= 0;) {
        const QJsonObject event = QJsonDocument::fromJson(m_buffer.left(end)).object();
        m_buffer.remove(0, end + 1);
        const QString kind = event.value("event").toString();
        if (kind == "step_update") {
            const QJsonObject step = event.value("step_update").toObject();
            const QString text = step.value("text_delta").toString();
            if (step.value("step_type").toString() == "agent_response" && !text.isEmpty()) {
                m_answered = true;
                emit delta(text);
            }
        } else if (kind == "result") {
            const QJsonObject result = event.value("result").toObject();
            if (result.value("status").toString() != "SUCCESS")
                m_error = result.value("error").toString("the turn failed");
        }
    }
}

void AntigravityProvider::cancel() {
    if (m_process.state() == QProcess::NotRunning)
        return;
    m_cancelled = true;
    m_process.kill();
    m_process.waitForFinished(1000);
}

void Ai::useModel(AiRequest &request, const QString &word) {
    const QString named = word.trimmed();
    if (named == "main" || named == "quick") {
        request.tier = named == "quick" ? AiRequest::Quick : AiRequest::Main;
        request.model.clear();
    } else {
        request.model = named;
    }
}

QString Ai::tierName(AiRequest::Tier tier) {
    return tier == AiRequest::Quick ? "for reading and parsing" : "for answers and writing";
}

Ai::ModelOverride::ModelOverride(const QString &word) {
    const QMutexLocker locker(&overrideLock);
    m_before = overrideWord;
    if (!word.trimmed().isEmpty())
        overrideWord = word.trimmed();
}

Ai::ModelOverride::~ModelOverride() {
    const QMutexLocker locker(&overrideLock);
    overrideWord = m_before;
}
