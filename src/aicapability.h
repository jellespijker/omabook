#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringView>
#include <array>
#include <cstdint>
#include <optional>

// The six things Omabook asks a model to do, each chosen on its own (ADR
// 0008): its provider, and whether a hosted one may be used for it. This is
// only the vocabulary and the shape config.json keeps it in; the registry
// that resolves a capability to a provider is AiRegistry (airegistry.h).
//
// config.json's "capabilities", one object per capability by its name:
//   "hosted"     whether a provider on someone else's computer may be used for it
//   "provider"   for answers and quick only: the provider's id, as `ai_provider`
//                says it ("claude", "anthropic", "openai", "command", ...), or
//                "omarchy-default" (Omarchy's default agent, today's "auto")
//                or "local" (Omarchy's local models, else Ollama)
// The other capabilities' providers stay set up where they always were
// ("recognition", "meaning", "read_aloud"), as each provider is set up once.
enum class Capability : std::uint8_t { Answers, Quick, Handwriting, SpeechToText, TextToSpeech, Embeddings };

// Why a capability can't be used, as a code (ADR 0005): what `status`
// answers, worded where it is shown.
enum class CapabilityLack : std::uint8_t {
    None,
    AiOff,            // the AI is set to Off
    NoServer,         // no provider is set up for it
    HostedNotAllowed, // only a hosted provider, and hosted isn't allowed for it in the settings
    HostedNotChosen,  // only a hosted provider, allowed, but the one asking didn't ask for one
    NoModel,          // the provider has no model for it (learnt by asking)
    NotInstalled,     // the program or the models it needs aren't installed
    NoKey,            // a key is needed and there is none
    Unreachable,      // it was asked and didn't answer
};

namespace Capabilities {

inline constexpr std::array all{Capability::Answers,      Capability::Quick,        Capability::Handwriting,
                                Capability::SpeechToText, Capability::TextToSpeech, Capability::Embeddings};

// "answers", "quick", "handwriting", "speech-to-text", "text-to-speech", "embeddings".
[[nodiscard]] QString name(Capability capability);
[[nodiscard]] std::optional<Capability> named(QStringView name);
// "ai-off", "no-server", "hosted-not-allowed", "hosted-not-chosen", "no-model",
// "not-installed", "no-key", "unreachable"; empty for none.
[[nodiscard]] QString lackName(CapabilityLack lack);

// What a config.json written new starts with: local for every capability,
// hosted allowed for none (answers and quick from Omarchy's local models,
// else Ollama).
[[nodiscard]] QJsonObject fresh();
// The provider id `capabilities` keeps for what `ai_provider` says: its own,
// with "auto" as "omarchy-default". Off is no provider: empty.
[[nodiscard]] QString providerFor(const QString &aiProvider);
// The other way: what `ai_provider` would say for a capability's provider.
[[nodiscard]] QString aiProviderFor(const QString &provider);

} // namespace Capabilities
