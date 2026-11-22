#include "voices.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>

#include "aiendpoint.h"
#include "aifallback.h"
#include "aiprivacy.h"
#include "gemini.h"
#include "geminivoice.h"
#include "localvoices.h"
#include "openaivoice.h"
#include "realtimevoice.h"
#include "secretstore.h"
#include "servervoice.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString chosen(const Settings &settings, QLatin1StringView key) {
    return settings.readAloud().value(key).toString().trimmed();
}

// OpenAI's row of the servers: its address and the variable its key may be in.
[[nodiscard]] const AiEndpoint &openAiRow() {
    const AiEndpoint *row = AiEndpoints::find(u"openai"_s);
    Q_ASSERT(row != nullptr);
    return *row;
}

[[nodiscard]] QString piperProgram(const Settings &settings) {
    const QString named = chosen(settings, "piper_program"_L1);
    return named.isEmpty() ? PiperVoice::findProgram() : QStandardPaths::findExecutable(named);
}

[[nodiscard]] QString serverModel(const Settings &settings) {
    const QString model = chosen(settings, "model"_L1);
    return model.isEmpty() ? QString::fromLatin1(ServerVoice::defaultModel) : model;
}

[[nodiscard]] AiServer custom(const QString &base) {
    return {.preset = u"custom"_s,
            .name = QUrl(base).host(),
            .baseUrl = base,
            .key = AiEndpoints::keptKey(base, Secrets::shared()),
            .keyFile = {},
            .model = {}};
}

// The server chosen to read aloud, if it may be asked: the one named for it,
// else the AI server.
[[nodiscard]] std::expected<AiServer, Failure> server(const Settings &settings, AiPrivacy::HostedAsked asked) {
    if (settings.aiProvider() == "off"_L1)
        return std::unexpected(Failure(Failure::Code::VoiceAiOff));
    QString base = chosen(settings, "url"_L1);
    while (base.endsWith(u'/'))
        base.chop(1);
    std::expected<AiServer, QString> found =
        base.isEmpty() ? AiEndpoints::server(settings) : std::expected<AiServer, QString>(custom(base));
    if (!found)
        return std::unexpected(Failure::said(found.error()));
    const QString model = serverModel(settings);
    const QString where = AiEndpoints::runsElsewhere(model) ? model : found->host();
    switch (AiPrivacy::refusal(settings, Capability::TextToSpeech, AiPrivacy::hosted(settings, found->baseUrl, model), asked)) {
    case CapabilityLack::None:
        return *found;
    case CapabilityLack::HostedNotChosen:
        return std::unexpected(Failure(Failure::Code::HostedNotChosen, {where}));
    default:
        return std::unexpected(Failure(Failure::Code::VoiceServerHosted, {where}));
    }
}

// Gemini's voices (#145), if they may be asked: never with the AI off, and,
// as they are always someone else's computer, only where hosted is allowed.
[[nodiscard]] std::expected<Gemini::Api, Failure> gemini(const Settings &settings, AiPrivacy::HostedAsked asked) {
    if (settings.aiOff())
        return std::unexpected(Failure(Failure::Code::VoiceAiOff));
    switch (AiPrivacy::refusal(settings, Capability::TextToSpeech, true, asked)) {
    case CapabilityLack::None:
        return Gemini::api(settings);
    case CapabilityLack::HostedNotChosen:
        return std::unexpected(Failure(Failure::Code::HostedNotChosen, {u"Gemini"_s}));
    default:
        return std::unexpected(Failure(Failure::Code::VoiceServerHosted, {u"Gemini"_s}));
    }
}

[[nodiscard]] QString openAiKey(const Settings &settings) {
    if (const QString key = qEnvironmentVariable(openAiRow().keyVariable).trimmed(); !key.isEmpty())
        return key;
    return AiEndpoints::keptKey(Voices::openAiUrl(settings), Secrets::shared());
}

// OpenAI's voices (#147), if they may be asked: as Gemini's, never with the
// AI off and only where hosted is allowed, with the key OpenAI answers with
// (OPENAI_API_KEY, else the keyring's for its address).
[[nodiscard]] std::expected<AiServer, Failure> openAi(const Settings &settings, AiPrivacy::HostedAsked asked) {
    if (settings.aiOff())
        return std::unexpected(Failure(Failure::Code::VoiceAiOff));
    switch (AiPrivacy::refusal(settings, Capability::TextToSpeech, true, asked)) {
    case CapabilityLack::None:
        break;
    case CapabilityLack::HostedNotChosen:
        return std::unexpected(Failure(Failure::Code::HostedNotChosen, {u"OpenAI"_s}));
    default:
        return std::unexpected(Failure(Failure::Code::VoiceServerHosted, {u"OpenAI"_s}));
    }
    const QString key = openAiKey(settings);
    if (key.isEmpty())
        return std::unexpected(Failure(Failure::Code::VoiceOpenAiNoKey));
    return AiServer{
        .preset = u"openai"_s, .name = u"OpenAI"_s, .baseUrl = Voices::openAiUrl(settings), .key = key, .keyFile = {}, .model = {}};
}

[[nodiscard]] QStringList retiredModels(const Settings &settings) {
    QStringList models;
    for (const QJsonValue model : settings.readAloud().value("openai_retired"_L1).toArray())
        models.append(model.toString());
    return models;
}

void keepRetired(Settings &settings, const QStringList &models) {
    QJsonObject setUp = settings.readAloud();
    if (models.isEmpty())
        setUp.remove("openai_retired"_L1);
    else
        setUp.insert(u"openai_retired"_s, QJsonArray::fromStringList(models));
    settings.setReadAloud(setUp);
}

// The model chosen, unless OpenAI has retired it: then its default.
[[nodiscard]] QString openAiModel(const Settings &settings) {
    const QString model = chosen(settings, "openai_model"_L1);
    return model.isEmpty() || retiredModels(settings).contains(model) ? Voices::openAiDefault(settings) : model;
}

// The engine whose voice and pace are kept: the one in use, else the one chosen.
[[nodiscard]] QString speaking(const Settings &settings) {
    const auto which = Voices::engine(settings);
    if (which)
        return *which;
    const QString wanted = chosen(settings, "engine"_L1);
    return wanted.isEmpty() ? u"auto"_s : wanted;
}

[[nodiscard]] bool piperReady(const Settings &settings) { return !piperProgram(settings).isEmpty() && !PiperVoice::findModels().isEmpty(); }
[[nodiscard]] bool dispatcherReady() {
    return !QStandardPaths::findExecutable(QString::fromLatin1(SpeechDispatcherVoice::program)).isEmpty();
}

// What reads in place of a server that can't be reached (#174): the
// fallback's server, else this computer's voice, Piper or speech-dispatcher
// as automatic has them; none where it answers, or nothing else may read.
[[nodiscard]] std::unique_ptr<Voice> fallbackFor(const Settings &settings, const AiServer &first, const QString &model,
                                                 AiPrivacy::HostedAsked asked) {
    auto instead = AiFallback::instead(settings, Capability::TextToSpeech, first.baseUrl, model, asked);
    if (!instead)
        return nullptr;
    if (instead->to == AiFallback::To::Server)
        return std::make_unique<ServerVoice>(std::move(instead->server),
                                             instead->model.isEmpty() ? QString::fromLatin1(ServerVoice::defaultModel) : instead->model);
    if (piperReady(settings))
        return std::make_unique<PiperVoice>(piperProgram(settings), PiperVoice::findModels());
    if (dispatcherReady())
        return std::make_unique<SpeechDispatcherVoice>(QStandardPaths::findExecutable(QString::fromLatin1(SpeechDispatcherVoice::program)));
    return nullptr;
}

} // namespace

std::expected<QString, Failure> Voices::engine(const Settings &settings, AiPrivacy::HostedAsked asked) {
    const QString wanted = chosen(settings, "engine"_L1);
    if (wanted == "server"_L1) {
        if (auto reachable = server(settings, asked); !reachable)
            return std::unexpected(reachable.error());
        return u"server"_s;
    }
    if (wanted == "gemini"_L1) {
        if (auto reachable = gemini(settings, asked); !reachable)
            return std::unexpected(reachable.error());
        return u"gemini"_s;
    }
    if (wanted == "openai"_L1) {
        if (auto reachable = openAi(settings, asked); !reachable)
            return std::unexpected(reachable.error());
        return u"openai"_s;
    }
    if (wanted == "piper"_L1) {
        if (piperProgram(settings).isEmpty())
            return std::unexpected(Failure(Failure::Code::PiperMissing));
        if (PiperVoice::findModels().isEmpty())
            return std::unexpected(Failure(Failure::Code::PiperNoVoice));
        return u"piper"_s;
    }
    if (wanted == "speech-dispatcher"_L1 || !piperReady(settings)) {
        if (!dispatcherReady())
            return std::unexpected(
                Failure(wanted.isEmpty() || wanted == "auto"_L1 ? Failure::Code::NoVoice : Failure::Code::DispatcherMissing));
        return u"speech-dispatcher"_s;
    }
    return u"piper"_s;
}

std::expected<std::unique_ptr<Voice>, Failure> Voices::choose(const Settings &settings, AiPrivacy::HostedAsked asked) {
    const auto which = engine(settings, asked);
    if (!which)
        return std::unexpected(which.error());
    if (*which == "piper"_L1)
        return std::make_unique<PiperVoice>(piperProgram(settings), PiperVoice::findModels());
    if (*which == "gemini"_L1) {
        auto reachable = gemini(settings, asked);
        if (!reachable)
            return std::unexpected(reachable.error());
        return std::make_unique<GeminiVoice>(*std::move(reachable), Gemini::modelFor(settings, Capability::TextToSpeech));
    }
    if (*which == "openai"_L1) {
        auto reachable = openAi(settings, asked);
        if (!reachable)
            return std::unexpected(reachable.error());
        if (auto instead = fallbackFor(settings, *reachable, openAiModel(settings), asked))
            return instead;
        return std::make_unique<OpenAiVoice>(*std::move(reachable), Voices::openAiRealtimeUrl(settings), openAiModel(settings));
    }
    if (*which == "server"_L1) {
        auto reachable = server(settings, asked);
        if (!reachable)
            return std::unexpected(reachable.error());
        if (auto instead = fallbackFor(settings, *reachable, serverModel(settings), asked))
            return instead;
        return std::make_unique<ServerVoice>(*std::move(reachable), serverModel(settings));
    }
    return std::make_unique<SpeechDispatcherVoice>(QStandardPaths::findExecutable(QString::fromLatin1(SpeechDispatcherVoice::program)));
}

std::optional<AiFallback::Instead> Voices::insteadOf(const Settings &settings, AiPrivacy::HostedAsked asked) {
    const auto which = engine(settings, asked);
    std::expected<AiServer, Failure> first = std::unexpected(Failure());
    QString model;
    if (which && *which == "server"_L1) {
        first = server(settings, asked);
        model = serverModel(settings);
    } else if (which && *which == "openai"_L1) {
        first = openAi(settings, asked);
        model = openAiModel(settings);
    }
    if (!first)
        return std::nullopt;
    auto instead = AiFallback::instead(settings, Capability::TextToSpeech, first->baseUrl, model, asked);
    // This computer's voice reads only where there is one.
    if (instead && instead->to == AiFallback::To::ThisComputer && !piperReady(settings) && !dispatcherReady())
        return std::nullopt;
    return instead;
}

QStringList Voices::offered(const Settings &settings) {
    const auto which = engine(settings);
    if (!which)
        return {};
    if (*which == "piper"_L1) {
        QStringList names;
        std::ranges::transform(PiperVoice::findModels(), std::back_inserter(names),
                               [](const QString &file) { return QFileInfo(file).completeBaseName(); });
        return names;
    }
    if (*which == "gemini"_L1)
        return Gemini::voices();
    if (*which == "openai"_L1)
        return RealtimeVoice::realtimeOnly(openAiModel(settings)) ? RealtimeVoice::voices() : ServerVoice::openAiVoices();
    return *which == "server"_L1 ? ServerVoice::voices() : SpeechDispatcherVoice::voices();
}

Speech Voices::speech(const Settings &settings, const QString &language) {
    const QJsonObject chosenNow = settings.readAloud();
    const QJsonObject perEngine = chosenNow.value("voices"_L1).toObject();
    const QJsonObject own = perEngine.value(speaking(settings)).toObject();
    // Without "voices" (an older config.json), the one voice stands for every engine.
    const QJsonValue voice = chosenNow.contains("voices"_L1) ? own.value("voice"_L1) : chosenNow.value("voice"_L1);
    return {.rate = std::clamp(own.value("rate"_L1).toDouble(chosenNow.value("rate"_L1).toDouble(1.0)), 0.5, 2.0),
            .voice = voice.toString(),
            .language = language};
}

void Voices::keep(Settings &settings, const QString &field, const QJsonValue &value) {
    QJsonObject chosenNow = settings.readAloud();
    QJsonObject perEngine = chosenNow.value("voices"_L1).toObject();
    const QString which = speaking(settings);
    QJsonObject own = perEngine.value(which).toObject();
    if (chosenNow.contains("voices"_L1) && own.value(field) == value && chosenNow.value(field) == value)
        return;
    // The first time, every engine's speed is what it was; the voice only the engine's own.
    if (!chosenNow.contains("voices"_L1) && chosenNow.contains("rate"_L1))
        own.insert(u"rate"_s, chosenNow.value("rate"_L1));
    own.insert(field, value);
    perEngine.insert(which, own);
    chosenNow.insert(u"voices"_s, perEngine);
    // As an older Omabook reads it: what the engine in use reads with.
    chosenNow.insert(field, value);
    settings.setReadAloud(chosenNow);
}

QString Voices::reader(const Settings &settings) {
    const auto which = engine(settings);
    if (!which)
        return {};
    QJsonObject setUp = settings.readAloud();
    // What OpenAI retired is learnt while it reads, which reads on with the default.
    for (const auto *field : {"voice", "rate", "voices", "hosted", "openai_retired"})
        setUp.remove(QLatin1StringView(field));
    if (*which == "gemini"_L1)
        setUp.insert(u"gemini"_s, settings.gemini());
    return *which + u' ' + QString::fromUtf8(QJsonDocument(setUp).toJson(QJsonDocument::Compact));
}

QString Voices::openAiUrl(const Settings &settings) {
    QString base = chosen(settings, "openai_url"_L1);
    if (base.isEmpty())
        base = QString::fromLatin1(openAiRow().baseUrl);
    while (base.endsWith(u'/'))
        base.chop(1);
    return base;
}

QUrl Voices::openAiRealtimeUrl(const Settings &settings) {
    QString named = chosen(settings, "openai_realtime_url"_L1);
    while (named.endsWith(u'/'))
        named.chop(1);
    return named.isEmpty() ? RealtimeVoice::addressOf(openAiUrl(settings)) : QUrl(named);
}

QString Voices::openAiDefault(const Settings &settings) {
    // /audio/speech's, until OpenAI says it has shut it down (2027-01-06): then its successor's, over the Realtime API.
    return retiredModels(settings).contains(QString::fromLatin1(ServerVoice::openAiModel)) ? QString::fromLatin1(RealtimeVoice::openAiModel)
                                                                                           : QString::fromLatin1(ServerVoice::openAiModel);
}

bool Voices::openAiHasKey(const Settings &settings) { return !openAiKey(settings).isEmpty(); }

QString Voices::retiredChoice(const Settings &settings) {
    const QString model = chosen(settings, "openai_model"_L1);
    return !model.isEmpty() && retiredModels(settings).contains(model) ? model : QString();
}

void Voices::retire(Settings &settings, const QString &model) {
    QStringList models = retiredModels(settings);
    if (model.isEmpty() || models.contains(model))
        return;
    models.append(model);
    keepRetired(settings, models);
}

void Voices::forget(Settings &settings, const QString &model) {
    QStringList models = retiredModels(settings);
    if (models.removeAll(model.trimmed()) > 0)
        keepRetired(settings, models);
}

QString Voices::nameOf(const QString &engine) {
    if (engine == "piper"_L1)
        return u"Piper"_s;
    if (engine == "openai"_L1)
        return u"OpenAI"_s;
    return engine == "gemini"_L1 ? u"Gemini"_s : engine;
}

bool Voices::paced(const Settings &settings) { return speaking(settings) != "gemini"_L1; }
