#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>
#include <functional>
#include <memory>

#include "ai.h"
#include "aicapability.h"
#include "aiprivacy.h"
#include "embeddings.h"
#include "failure.h"
#include "recognitionroute.h"

class Settings;
class Voice;

// Where a capability stands (ADR 0008): whether it may be used now, whether
// what it is sent goes to someone else's computer, the provider and model it
// resolves to, and why not, as a code and its failure, worded where shown.
struct CapabilityStatus {
    Capability capability = Capability::Answers;
    bool available = false;
    bool hosted = false;
    QString provider; // what to call it: "Claude Code", "Ollama", "Piper", "the AI server at ..."
    QString model;    // empty for the provider's own default, or none
    CapabilityLack why = CapabilityLack::None;
    Failure failure; // why not, when it isn't available

    // {capability, available, hosted, provider, model, why, message}.
    [[nodiscard]] QJsonObject toJson() const;
};

// The one registry every use of a model goes through (ADR 0008): the
// companion, the graph, recognition, the meaning index, reading aloud, the
// command line, the MCP server and plugins. It resolves a capability to its
// provider and model by the person's settings and the one privacy rule
// (AiPrivacy), and says why one isn't available. Nothing else makes a
// provider (the harness's `ai-only-through-the-registry` rule): a new
// provider is a class the registry makes, a new capability a name here.
namespace AiRegistry {

// What the one asking wants: a model by its word ("main", "quick" or a
// name; empty for the capability's own), and what they said of hosted
// providers (AiPrivacy::HostedAsked).
struct Ask {
    QString model;
    AiPrivacy::HostedAsked hosted;
};

[[nodiscard]] CapabilityStatus status(const Settings &settings, Capability capability, const Ask &ask = {},
                                      const QString &defaultsFile = {});
// Every capability's status by its name, as `omabook info --json` says it.
[[nodiscard]] QJsonObject describe(const Settings &settings, const QString &defaultsFile = {});

// The model for answers and quick work, one provider serving both (each
// request names its tier, AiRequest::Tier); where only one of the two may
// use it, a request of the other fails with why. Fails when neither may.
[[nodiscard]] std::expected<std::unique_ptr<AiProvider>, Failure> answers(const Settings &settings, const Ask &ask = {},
                                                                          const QString &defaultsFile = {});
// The same as the factory the window, the plugins and the command line are
// handed (Backend::AiFactory): null with why, in words.
using Answerer = std::function<std::unique_ptr<AiProvider>(QString *why)>;
[[nodiscard]] Answerer answerer(const Settings &settings, const QString &defaultsFile = {});

// The ways handwriting or speech is recognised, in order (RecognitionRoute).
[[nodiscard]] std::expected<RecognitionRoute::Ways, QString> recognizers(const Settings &settings, Recognition::Kind kind,
                                                                         bool hostedAsked);
// Where embeddings are made (Embeddings::route).
[[nodiscard]] std::expected<Embeddings::Route, Failure> embeddings(const Settings &settings, AiPrivacy::HostedAsked asked = std::nullopt,
                                                                   const QString &omarchyProgram = {});
// The voice that reads aloud (Voices::choose): a local one even with the AI
// Off, which only reads the text; a server's only where it may be asked.
[[nodiscard]] std::expected<std::unique_ptr<Voice>, Failure> voice(const Settings &settings, AiPrivacy::HostedAsked asked = std::nullopt);

} // namespace AiRegistry
