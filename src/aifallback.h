#pragma once

#include <QString>
#include <QVariantMap>
#include <cstdint>
#include <optional>

#include "aicapability.h"
#include "aiendpoint.h"
#include "aiprivacy.h"

class Settings;

// A second server per capability, used when the first can't be reached (#174,
// ADR 0008): the laptop away from home, the server off. Only a first that
// can't be reached at all (ServerReach) is passed over, never one that answers
// with an error, and only for as long as it stays away: once it answers again
// it is used again, by itself.
//
// config.json's "capabilities", each capability's "fallback":
//   "to"      "this-computer", "server" or "none"; without it, the capability's own default
//   "url"     the server's address, for "server" (its key in the keyring, by that address)
//   "model"   the model asked for there; without it, the first's
//   "hosted"  whether it may be someone else's computer where the first wasn't:
//             a fallback that is hosted needs this allowance of its own
//
// What each capability does by default (`rule`):
//   - embeddings, handwriting, speech to text and text to speech fall back to
//     this computer: its Ollama (`thisComputersOllama`) for embeddings and
//     handwriting, voxtype and whisper.cpp for speech to text, Piper or
//     speech-dispatcher for text to speech;
//   - quick work (the knowledge graph) waits for its server, and catches up once
//     it is back, as a request that failed leaves nothing marked as built;
//   - answers fall back only where the person chose a fallback for them.
//
// Embeddings stay consistent: vectors of different models can't be mixed, so
// their fallback is always asked for the first's model, and a book indexed
// with it is searched with it on either server.
namespace AiFallback {

enum class To : std::uint8_t { None, ThisComputer, Server };
enum class Rule : std::uint8_t { FallsBack, Waits, IfChosen };

inline constexpr auto thisComputersOllama = "http://localhost:11434/v1";

[[nodiscard]] Rule rule(Capability capability);
// "this-computer", "server", "none"; and back, none for a word it isn't.
[[nodiscard]] QString toName(To to);
[[nodiscard]] std::optional<To> toNamed(const QString &name);

// The fallback as the settings choose it, else the capability's default.
struct Chosen {
    To to = To::None;
    QString url;
    QString model;
    bool hosted = false;
};
[[nodiscard]] Chosen chosen(const Settings &settings, Capability capability);

// The fallback to use in place of the first, at this address, as it can't be
// reached now. None where the first answers; where the capability has no
// fallback, or waits; with the AI Off; where the first is this computer and
// so would the fallback be; and where the fallback is someone else's computer
// without its own allowance, or the one asking kept to their own.
struct Instead {
    To to = To::ThisComputer;
    AiServer server; // the fallback's server; empty for this computer's own ways of speech
    QString model;   // the model asked for there
    bool hosted = false;
    QString first; // what the first, away, is called: its host
};
[[nodiscard]] std::optional<Instead> instead(const Settings &settings, Capability capability, const QString &firstUrl,
                                             const QString &firstModel, AiPrivacy::HostedAsked asked = std::nullopt);

// The row of the AI page's part on it: {to, url, model, hosted, rule ("falls-back",
// "waits", "if-chosen"), chosenTo (whether the person chose, or it is the default),
// hasKey, inTheClear}.
[[nodiscard]] QVariantMap describe(const Settings &settings, Capability capability);
// Puts these of the fallback's fields into the settings (to, url, model,
// hosted); an empty url or model is taken out. Quick work has none.
void choose(Settings &settings, Capability capability, const QVariantMap &fields);
// A copy's settings that try the fallback as though the first were away: the
// row's Test of its fallback, which goes through exactly what would be used.
void tryInstead(Settings &settings, Capability capability);

} // namespace AiFallback
