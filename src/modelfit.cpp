#include "modelfit.h"

#include <QJsonObject>
#include <QUrl>
#include <algorithm>
#include <array>

#include "ai.h"
#include "aiprivacy.h"
#include "airegistry.h"
#include "embeddings.h"
#include "recognizers.h"
#include "secretstore.h"
#include "servervoice.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// What a model is, by the words its name and family are made of, for a
// server that doesn't say what it does (an older Ollama, an
// OpenAI-compatible /models).
constexpr auto embedding =
    std::to_array<QLatin1StringView>({"embed"_L1, "bert"_L1, "minilm"_L1, "bge-"_L1, "bge:"_L1, "e5-"_L1, "gte-"_L1});
constexpr auto seeing = std::to_array<QLatin1StringView>({"clip"_L1, "mllama"_L1, "llava"_L1, "vision"_L1, "-vl"_L1, "vl:"_L1, "vl-"_L1,
                                                          "qwen25vl"_L1, "qwen2.5vl"_L1, "qwen3vl"_L1, "moondream"_L1, "minicpm-v"_L1,
                                                          "pixtral"_L1, "gemma3:4b"_L1, "gemma3:12b"_L1, "gemma3:27b"_L1});
constexpr auto hearing = std::to_array<QLatin1StringView>({"whisper"_L1, "transcribe"_L1, "parakeet"_L1, "stt"_L1});
constexpr auto speaking = std::to_array<QLatin1StringView>({"tts"_L1, "kokoro"_L1, "piper"_L1, "orpheus"_L1, "speech"_L1});

template <std::size_t N> [[nodiscard]] bool namedAs(const ModelFit::Model &model, const std::array<QLatin1StringView, N> &words) {
    const QString all = (model.name + u' ' + model.family + u' ' + model.families.join(u' ')).toLower();
    return std::ranges::any_of(words, [&all](QLatin1StringView word) { return all.contains(word); });
}

[[nodiscard]] ModelFit::Fit fitOf(bool fits) { return fits ? ModelFit::Fit::Fits : ModelFit::Fit::Unfit; }

// By what the server says the model does (Ollama's /api/show).
[[nodiscard]] std::optional<ModelFit::Fit> byAbilities(Capability capability, const QStringList &abilities) {
    if (abilities.isEmpty())
        return std::nullopt;
    switch (capability) {
    case Capability::Embeddings:
        return fitOf(abilities.contains("embedding"_L1));
    case Capability::Handwriting:
        return fitOf(abilities.contains("vision"_L1));
    case Capability::Answers:
    case Capability::Quick:
        return fitOf(abilities.contains("completion"_L1));
    case Capability::SpeechToText:
    case Capability::TextToSpeech:
        break; // Ollama names no ability for these: by the name
    }
    return std::nullopt;
}

[[nodiscard]] QString trimmedUrl(QString url) {
    url = url.trimmed();
    while (url.endsWith(u'/'))
        url.chop(1);
    return url;
}

// A server given by its address for one capability, else the AI server.
[[nodiscard]] std::optional<AiServer> serverAt(const Settings &settings, const QString &address) {
    const QString url = trimmedUrl(address);
    if (url.isEmpty()) {
        auto server = AiEndpoints::server(settings);
        return server ? std::optional(*std::move(server)) : std::nullopt;
    }
    return AiServer{.preset = u"custom"_s,
                    .name = QUrl(url).host(),
                    .baseUrl = url,
                    .key = AiEndpoints::keptKey(url, Secrets::shared()),
                    .keyFile = {},
                    .model = {}};
}

[[nodiscard]] QString providerOf(const Settings &settings, Capability capability) {
    return AiPrivacy::chosen(settings, capability).value("provider"_L1).toString();
}

[[nodiscard]] std::optional<ModelFit::Place> answersPlace(const Settings &settings, Capability capability) {
    // Omarchy's local models are each a server of its own, found by asking Omarchy.
    if (settings.aiProvider() != "openai"_L1 || AiEndpoints::preset(settings) == "omarchy-local"_L1)
        return std::nullopt;
    const AiModels models = Ai::models(settings, AiEndpoints::modelsKey(settings));
    auto server = AiEndpoints::server(settings);
    if (!server)
        return std::nullopt;
    const QString model = capability == Capability::Quick ? models.quick : models.main;
    return ModelFit::Place{.server = *server, .model = model.isEmpty() ? server->model : model};
}

[[nodiscard]] std::optional<ModelFit::Place> handwritingPlace(const Settings &settings) {
    if (providerOf(settings, Capability::Handwriting) == "gemini"_L1)
        return std::nullopt;
    QString model = settings.recognition().value("ink_model"_L1).toString().trimmed();
    // Its own server (#170), with its own model or none yet (the first it lists is used then).
    if (const QString own = settings.recognition().value("ink_url"_L1).toString(); !own.trimmed().isEmpty()) {
        auto server = serverAt(settings, own);
        return server ? std::optional(ModelFit::Place{.server = *server, .model = model}) : std::nullopt;
    }
    if (model.isEmpty())
        model = Ai::models(settings, AiEndpoints::modelsKey(settings)).main;
    auto server = AiEndpoints::server(settings, model);
    if (!server)
        return std::nullopt;
    return ModelFit::Place{.server = *server, .model = model.isEmpty() ? server->model : model};
}

[[nodiscard]] std::optional<ModelFit::Place> speechPlace(const Settings &settings) {
    const QString provider = providerOf(settings, Capability::SpeechToText);
    const QString address = settings.recognition().value("transcription_url"_L1).toString();
    // voxtype and whisper.cpp come first unless a server was chosen (#146).
    if (provider != "server"_L1 && (address.trimmed().isEmpty() || !provider.isEmpty()))
        return std::nullopt;
    auto server = serverAt(settings, address);
    if (!server)
        return std::nullopt;
    const QString model = settings.recognition().value("transcription_model"_L1).toString().trimmed();
    return ModelFit::Place{.server = *server,
                           .model = model.isEmpty() ? QString::fromLatin1(Recognizers::defaultTranscriptionModel) : model};
}

[[nodiscard]] std::optional<ModelFit::Place> voicePlace(const Settings &settings) {
    const QJsonObject chosen = settings.readAloud();
    if (chosen.value("engine"_L1).toString() != "server"_L1)
        return std::nullopt;
    auto server = serverAt(settings, chosen.value("url"_L1).toString());
    if (!server)
        return std::nullopt;
    const QString model = chosen.value("model"_L1).toString().trimmed();
    return ModelFit::Place{.server = *server, .model = model.isEmpty() ? QString::fromLatin1(ServerVoice::defaultModel) : model};
}

[[nodiscard]] std::optional<ModelFit::Place> embeddingsPlace(const Settings &settings) {
    // Whether it may be sent a book's text is not the question: only which model it has.
    const auto route = AiRegistry::embeddings(settings, true);
    if (!route || route->gemini)
        return std::nullopt;
    return ModelFit::Place{.server = route->server, .model = route->model};
}

[[nodiscard]] QJsonObject with(QJsonObject object, QLatin1StringView key, const QString &value) {
    object.insert(key, value.trimmed());
    return object;
}

} // namespace

ModelFit::Fit ModelFit::fit(Capability capability, const Model &model) {
    if (const auto said = byAbilities(capability, model.abilities))
        return *said;
    switch (capability) {
    case Capability::Embeddings:
        return fitOf(namedAs(model, embedding));
    case Capability::Handwriting:
        return namedAs(model, seeing) ? Fit::Fits : Fit::Unknown;
    case Capability::Answers:
    case Capability::Quick:
        return fitOf(!namedAs(model, embedding) && !namedAs(model, hearing) && !namedAs(model, speaking));
    case Capability::SpeechToText:
        return fitOf(namedAs(model, hearing));
    case Capability::TextToSpeech:
        return fitOf(namedAs(model, speaking));
    }
    return Fit::Unknown;
}

std::optional<ModelFit::Suggestion> ModelFit::suggested(Capability capability) {
    // Sizes as Ollama's library gives them for download, checked 2026-11-22.
    switch (capability) {
    case Capability::Embeddings:
        return Suggestion{.name = QString::fromLatin1(Embeddings::defaultModel), .bytes = 274'000'000};
    case Capability::Handwriting:
        return Suggestion{.name = u"qwen2.5vl:3b"_s, .bytes = 3'200'000'000};
    case Capability::Answers:
        return Suggestion{.name = u"llama3.2:3b"_s, .bytes = 2'000'000'000};
    case Capability::Quick:
        return Suggestion{.name = u"llama3.2:1b"_s, .bytes = 1'300'000'000};
    case Capability::SpeechToText:
    case Capability::TextToSpeech:
        break;
    }
    return std::nullopt;
}

bool ModelFit::same(const QString &one, const QString &other) {
    const auto plain = [](QString name) {
        name = name.trimmed();
        if (name.endsWith(":latest"_L1))
            name.chop(7);
        return name;
    };
    return !one.trimmed().isEmpty() && plain(one) == plain(other);
}

std::optional<ModelFit::Place> ModelFit::placeOf(const Settings &settings, Capability capability) {
    if (settings.aiOff())
        return std::nullopt;
    switch (capability) {
    case Capability::Answers:
    case Capability::Quick:
        return answersPlace(settings, capability);
    case Capability::Handwriting:
        return handwritingPlace(settings);
    case Capability::SpeechToText:
        return speechPlace(settings);
    case Capability::TextToSpeech:
        return voicePlace(settings);
    case Capability::Embeddings:
        return embeddingsPlace(settings);
    }
    return std::nullopt;
}

QString ModelFit::ollamaRoot(const AiServer &server) {
    QString root = trimmedUrl(server.baseUrl);
    if (root.endsWith("/v1"_L1))
        root.chop(3);
    return root;
}

void ModelFit::choose(Settings &settings, Capability capability, const QString &model) {
    switch (capability) {
    case Capability::Answers:
        settings.setAiModelFor(AiEndpoints::modelsKey(settings), u"main"_s, model);
        break;
    case Capability::Quick:
        settings.setAiModelFor(AiEndpoints::modelsKey(settings), u"quick"_s, model);
        break;
    case Capability::Handwriting:
        settings.setRecognition(with(settings.recognition(), "ink_model"_L1, model));
        break;
    case Capability::SpeechToText:
        settings.setRecognition(with(settings.recognition(), "transcription_model"_L1, model));
        break;
    case Capability::TextToSpeech:
        settings.setReadAloud(with(settings.readAloud(), "model"_L1, model));
        break;
    case Capability::Embeddings:
        settings.setMeaning(with(settings.meaning(), "model"_L1, model));
        break;
    }
}
