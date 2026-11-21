#include "aiprivacy.h"

#include "ai.h"
#include "aiendpoint.h"
#include "recognition.h"
#include "settings.h"

using namespace Qt::StringLiterals;

bool AiPrivacy::hosted(const Settings &settings, const QString &baseUrl, const QString &model) {
    return AiEndpoints::runsElsewhere(model) || Recognition::reach(baseUrl, settings.ownAddresses()) == Recognition::Reach::Hosted;
}

QJsonObject AiPrivacy::carriedOver(const Settings &settings) {
    // Answers and quick keep the provider they have, and where it is hosted,
    // hosted is allowed for them: that is where the text already went.
    const QString provider = Capabilities::providerFor(settings.aiProvider());
    bool answersHosted = !provider.isEmpty();
    if (settings.aiProvider() == "openai"_L1) {
        const AiModels models = Ai::models(settings, AiEndpoints::modelsKey(settings));
        answersHosted = AiEndpoints::runsElsewhere(models.main) || AiEndpoints::runsElsewhere(models.quick)
                        || (AiEndpoints::preset(settings) != "omarchy-local"_L1 && hosted(settings, AiEndpoints::baseUrl(settings), {}));
    }
    const auto hostedAs = [](const QJsonObject &older) { return QJsonObject{{u"hosted"_s, older.value("hosted"_L1).toBool(false)}}; };
    QJsonObject answers{{u"hosted"_s, answersHosted}};
    if (!provider.isEmpty())
        answers.insert(u"provider"_s, provider);
    return {{Capabilities::name(Capability::Answers), answers},
            {Capabilities::name(Capability::Quick), answers},
            {Capabilities::name(Capability::Handwriting), hostedAs(settings.recognition())},
            {Capabilities::name(Capability::SpeechToText), hostedAs(settings.recognition())},
            {Capabilities::name(Capability::TextToSpeech), hostedAs(settings.readAloud())},
            {Capabilities::name(Capability::Embeddings), hostedAs(settings.meaning())}};
}

void AiPrivacy::carryOver(Settings &settings) {
    // Only a config.json written new starts local: one there already keeps what it chose.
    if (!settings.hasCapabilities())
        settings.setCapabilities(settings.written() ? carriedOver(settings) : Capabilities::fresh());
}

QJsonObject AiPrivacy::chosen(const Settings &settings, Capability capability) {
    if (settings.hasCapabilities())
        return settings.capability(capability);
    QJsonObject older = carriedOver(settings).value(Capabilities::name(capability)).toObject();
    // Until it is carried over, an answer goes wherever the older keys say,
    // as it always did: what is chosen there is chosen for answers and quick.
    if (older.contains("provider"_L1))
        older.insert(u"hosted"_s, true);
    return older;
}

bool AiPrivacy::hostedAllowed(const Settings &settings, Capability capability) {
    return chosen(settings, capability).value("hosted"_L1).toBool(false);
}

CapabilityLack AiPrivacy::refusal(const Settings &settings, Capability capability, bool hosted, HostedAsked asked) {
    if (!hosted)
        return CapabilityLack::None;
    if (!hostedAllowed(settings, capability))
        return CapabilityLack::HostedNotAllowed;
    return asked.value_or(true) ? CapabilityLack::None : CapabilityLack::HostedNotChosen;
}
