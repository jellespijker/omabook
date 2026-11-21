#include "airegistry.h"

#include <QMetaObject>
#include <utility>

#include "ai.h"
#include "aiendpoint.h"
#include "settings.h"
#include "voice.h"
#include "voices.h"

using namespace Qt::StringLiterals;

namespace {

// One provider serves answers and quick work. Where only one of the two may
// use it (hosted allowed for answers, not for quick), a request of the other
// is refused here, before anything is sent.
class OneTierOnly : public AiProvider {
public:
    OneTierOnly(std::unique_ptr<AiProvider> serving, AiRequest::Tier refused, Failure why)
        : m_serving(std::move(serving)), m_refused(refused), m_why(std::move(why)) {
        setModels(m_serving->models());
        connect(m_serving.get(), &AiProvider::delta, this, &AiProvider::delta);
        connect(m_serving.get(), &AiProvider::finished, this, &AiProvider::finished);
        connect(m_serving.get(), &AiProvider::failed, this, &AiProvider::failed);
        connect(m_serving.get(), &AiProvider::noted, this, &AiProvider::noted);
    }

    [[nodiscard]] QString name() const override { return m_serving->name(); }
    void ask(const AiRequest &request) override {
        if (request.tier != m_refused) {
            m_serving->ask(request);
            return;
        }
        // Later, as a provider's answer always comes: the asker connects first.
        QMetaObject::invokeMethod(this, [this] { emit failed(m_why); }, Qt::QueuedConnection);
    }
    void cancel() override { m_serving->cancel(); }

private:
    std::unique_ptr<AiProvider> m_serving;
    AiRequest::Tier m_refused;
    Failure m_why;
};

// The model a request of `word` goes to: the capability's own for none.
[[nodiscard]] QString modelOf(const AiModels &models, Capability capability, const QString &word) {
    if (word.isEmpty())
        return capability == Capability::Quick ? models.quick : models.main;
    if (word == "main"_L1)
        return models.main;
    return word == "quick"_L1 ? models.quick : word;
}

// Claude's API, the agents and a command are someone else's computers; a
// server is by where it is and what its model says.
[[nodiscard]] bool answersHosted(const Settings &settings, const AiChoice &choice, const QString &model) {
    if (choice.id != "openai"_L1 || AiEndpoints::runsElsewhere(model))
        return true;
    // Omarchy's local models are found by asking Omarchy, on this computer.
    if (AiEndpoints::preset(settings) == "omarchy-local"_L1)
        return false;
    return AiPrivacy::hosted(settings, AiEndpoints::baseUrl(settings), model);
}

[[nodiscard]] CapabilityStatus answersStatus(const Settings &settings, Capability capability, const AiRegistry::Ask &ask,
                                             const QString &defaultsFile) {
    CapabilityStatus status{.capability = capability};
    const AiChoice choice = Ai::choose(settings, defaultsFile);
    status.provider = choice.name;
    if (!choice.available()) {
        // Where it would go all the same: anything but a server is someone else's computer.
        status.hosted = settings.aiProvider() != "openai"_L1 && !settings.aiOff();
        status.why = choice.lack;
        status.failure = choice.lack == CapabilityLack::AiOff ? Failure(Failure::Code::AiOff) : Failure::said(choice.problem);
        return status;
    }
    status.model = modelOf(Ai::models(settings, choice.modelsKey()), capability, ask.model.trimmed());
    status.hosted = answersHosted(settings, choice, status.model);
    status.why = AiPrivacy::refusal(settings, capability, status.hosted, ask.hosted);
    if (status.why == CapabilityLack::HostedNotAllowed)
        status.failure = Failure(
            capability == Capability::Quick ? Failure::Code::AiQuickHostedNotAllowed : Failure::Code::AiHostedNotAllowed, {choice.name});
    else if (status.why == CapabilityLack::HostedNotChosen)
        status.failure = Failure(Failure::Code::HostedNotChosen, {choice.name});
    status.available = status.why == CapabilityLack::None;
    return status;
}

// What a failure below says, as the registry's code.
[[nodiscard]] CapabilityLack lackOf(const Settings &settings, const Failure &failure) {
    switch (failure.code()) {
    case Failure::Code::AiOff:
    case Failure::Code::VoiceAiOff:
        return CapabilityLack::AiOff;
    case Failure::Code::MeaningHosted:
    case Failure::Code::VoiceServerHosted:
        return CapabilityLack::HostedNotAllowed;
    case Failure::Code::HostedNotChosen:
        return CapabilityLack::HostedNotChosen;
    case Failure::Code::NoVoice:
    case Failure::Code::PiperMissing:
    case Failure::Code::PiperNoVoice:
    case Failure::Code::DispatcherMissing:
        return CapabilityLack::NotInstalled;
    default: {
        const CapabilityLack server = AiEndpoints::lacking(settings);
        return server == CapabilityLack::None ? CapabilityLack::NoServer : server;
    }
    }
}

[[nodiscard]] CapabilityStatus refused(const Settings &settings, Capability capability, const Failure &failure) {
    return {.capability = capability,
            .available = false,
            .hosted = false,
            .provider = {},
            .model = {},
            .why = lackOf(settings, failure),
            .failure = failure};
}

[[nodiscard]] CapabilityStatus recognitionStatus(const Settings &settings, Capability capability, const AiRegistry::Ask &ask) {
    const Recognition::Kind kind = capability == Capability::Handwriting ? Recognition::Kind::Ink : Recognition::Kind::Audio;
    // The person's own use counts what they allowed; one asking, what it asked for.
    const bool asked = ask.hosted.value_or(true);
    if (const auto refusal = RecognitionRoute::refusal(settings, kind, asked))
        return {.capability = capability,
                .available = false,
                .hosted = false,
                .provider = {},
                .model = {},
                .why = refusal->lack,
                .failure = Failure::said(refusal->message)};
    const auto used = RecognitionRoute::ways(settings, kind, asked);
    const auto own = RecognitionRoute::ways(settings, kind, false);
    if (!used || used->empty())
        return refused(settings, capability, Failure::said(used ? QString() : used.error()));
    const Recognition::Recognizer &first = *used->front();
    // Hosted when keeping to one's own would go another way, or none.
    const bool hosted = !own || own->empty() || own->front()->where() != first.where();
    return {.capability = capability,
            .available = true,
            .hosted = hosted,
            .provider = first.where(),
            .model = first.model(),
            .why = CapabilityLack::None,
            .failure = {}};
}

[[nodiscard]] CapabilityStatus voiceStatus(const Settings &settings, const AiRegistry::Ask &ask) {
    const auto engine = Voices::engine(settings, ask.hosted);
    if (!engine)
        return refused(settings, Capability::TextToSpeech, engine.error());
    const auto made = Voices::choose(settings, ask.hosted);
    if (!made)
        return refused(settings, Capability::TextToSpeech, made.error());
    const bool server = *engine == "server"_L1;
    return {.capability = Capability::TextToSpeech,
            .available = true,
            // A server that keeping to one's own would refuse is someone else's.
            .hosted = server && !Voices::engine(settings, false),
            .provider = (*made)->name(),
            .model = server ? settings.readAloud().value("model"_L1).toString() : QString(),
            .why = CapabilityLack::None,
            .failure = {}};
}

[[nodiscard]] CapabilityStatus embeddingsStatus(const Settings &settings, const AiRegistry::Ask &ask) {
    const auto route = Embeddings::route(settings, ask.hosted);
    if (!route)
        return refused(settings, Capability::Embeddings, route.error());
    return {.capability = Capability::Embeddings,
            .available = true,
            .hosted = route->hosted,
            .provider = route->server.name,
            .model = route->model,
            .why = CapabilityLack::None,
            .failure = {}};
}

} // namespace

QJsonObject CapabilityStatus::toJson() const {
    return {{u"capability"_s, Capabilities::name(capability)},
            {u"available"_s, available},
            {u"hosted"_s, hosted},
            {u"provider"_s, provider},
            {u"model"_s, model},
            {u"why"_s, Capabilities::lackName(why)},
            {u"message"_s, available ? QString() : failure.text()}};
}

CapabilityStatus AiRegistry::status(const Settings &settings, Capability capability, const Ask &ask, const QString &defaultsFile) {
    switch (capability) {
    case Capability::Answers:
    case Capability::Quick:
        return answersStatus(settings, capability, ask, defaultsFile);
    case Capability::Handwriting:
    case Capability::SpeechToText:
        return recognitionStatus(settings, capability, ask);
    case Capability::TextToSpeech:
        return voiceStatus(settings, ask);
    case Capability::Embeddings:
        return embeddingsStatus(settings, ask);
    }
    return {};
}

QJsonObject AiRegistry::describe(const Settings &settings, const QString &defaultsFile) {
    QJsonObject all;
    for (const Capability capability : Capabilities::all)
        all.insert(Capabilities::name(capability), status(settings, capability, {}, defaultsFile).toJson());
    return all;
}

std::expected<std::unique_ptr<AiProvider>, Failure> AiRegistry::answers(const Settings &settings, const Ask &ask,
                                                                        const QString &defaultsFile) {
    const CapabilityStatus main = status(settings, Capability::Answers, ask, defaultsFile);
    const CapabilityStatus quick = status(settings, Capability::Quick, ask, defaultsFile);
    if (!main.available && !quick.available)
        return std::unexpected(main.failure);
    QString why;
    std::unique_ptr<AiProvider> made = makeAiProvider(settings, &why, defaultsFile);
    if (!made)
        return std::unexpected(Failure::said(why));
    if (!main.available)
        return std::make_unique<OneTierOnly>(std::move(made), AiRequest::Main, main.failure);
    if (!quick.available)
        return std::make_unique<OneTierOnly>(std::move(made), AiRequest::Quick, quick.failure);
    return made;
}

AiRegistry::Answerer AiRegistry::answerer(const Settings &settings, const QString &defaultsFile) {
    return [&settings, defaultsFile](QString *why) -> std::unique_ptr<AiProvider> {
        auto made = answers(settings, {}, defaultsFile);
        if (made)
            return std::move(*made);
        if (why)
            *why = made.error().text();
        return nullptr;
    };
}

std::expected<RecognitionRoute::Ways, QString> AiRegistry::recognizers(const Settings &settings, Recognition::Kind kind, bool hostedAsked) {
    return RecognitionRoute::ways(settings, kind, hostedAsked);
}

std::expected<Embeddings::Route, Failure> AiRegistry::embeddings(const Settings &settings, AiPrivacy::HostedAsked asked,
                                                                 const QString &omarchyProgram) {
    return Embeddings::route(settings, asked, omarchyProgram);
}

std::expected<std::unique_ptr<Voice>, Failure> AiRegistry::voice(const Settings &settings, AiPrivacy::HostedAsked asked) {
    return Voices::choose(settings, asked);
}
