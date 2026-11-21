#include "aicapability.h"

#include <algorithm>

using namespace Qt::StringLiterals;

QString Capabilities::name(Capability capability) {
    switch (capability) {
    case Capability::Answers:
        return u"answers"_s;
    case Capability::Quick:
        return u"quick"_s;
    case Capability::Handwriting:
        return u"handwriting"_s;
    case Capability::SpeechToText:
        return u"speech-to-text"_s;
    case Capability::TextToSpeech:
        return u"text-to-speech"_s;
    case Capability::Embeddings:
        return u"embeddings"_s;
    }
    return {};
}

std::optional<Capability> Capabilities::named(QStringView name) {
    const auto *const found = std::ranges::find_if(all, [name](Capability one) { return Capabilities::name(one) == name; });
    return found == all.end() ? std::nullopt : std::optional<Capability>(*found);
}

QString Capabilities::lackName(CapabilityLack lack) {
    switch (lack) {
    case CapabilityLack::None:
        return {};
    case CapabilityLack::AiOff:
        return u"ai-off"_s;
    case CapabilityLack::NoServer:
        return u"no-server"_s;
    case CapabilityLack::HostedNotAllowed:
        return u"hosted-not-allowed"_s;
    case CapabilityLack::HostedNotChosen:
        return u"hosted-not-chosen"_s;
    case CapabilityLack::NoModel:
        return u"no-model"_s;
    case CapabilityLack::NotInstalled:
        return u"not-installed"_s;
    case CapabilityLack::NoKey:
        return u"no-key"_s;
    case CapabilityLack::Unreachable:
        return u"unreachable"_s;
    }
    return {};
}

QJsonObject Capabilities::fresh() {
    QJsonObject chosen;
    for (const Capability capability : all) {
        QJsonObject one{{u"hosted"_s, false}};
        if (capability == Capability::Answers || capability == Capability::Quick)
            one.insert(u"provider"_s, u"local"_s);
        chosen.insert(name(capability), one);
    }
    return chosen;
}

QString Capabilities::providerFor(const QString &aiProvider) {
    if (aiProvider == "off"_L1)
        return {};
    return aiProvider == "auto"_L1 || aiProvider.isEmpty() ? u"omarchy-default"_s : aiProvider;
}

QString Capabilities::aiProviderFor(const QString &provider) {
    if (provider == "omarchy-default"_L1)
        return u"auto"_s;
    // Omarchy's local models and Ollama are both servers of the OpenAI-compatible API.
    return provider == "local"_L1 ? u"openai"_s : provider;
}
