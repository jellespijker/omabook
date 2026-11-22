#pragma once

#include <QJsonValue>
#include <QString>
#include <QStringList>
#include <expected>
#include <memory>

#include "aiprivacy.h"
#include "failure.h"
#include "voice.h"

class Settings;

// Which voice reads a book aloud (#73), from config.json's "read_aloud":
//   "engine"         "auto" (Piper with a voice, else speech-dispatcher),
//                    "piper", "speech-dispatcher", "server", "openai"
//                    (OpenAI's voices, #147) or "gemini" (Gemini's, #145),
//                    each hosted one with its key in the keyring
//   "voices"         how fast and in which voice, per engine (#147), as
//                    {"piper": {"voice", "rate"}, ...}: one engine's voice
//                    means nothing to another
//   "rate", "voice"  the same for the engine in use, as an older Omabook reads
//                    them; and, without "voices", for every engine
//   "piper_program"  Piper's command, when it isn't piper-tts or piper on the path
//   "url", "model"   the server of the OpenAI-compatible API that reads
//                    aloud (else the AI server), and its model ("tts-1")
//   "openai_model"   OpenAI's model ("gpt-4o-mini-tts"), and "openai_url" for a
//   "openai_url"     proxy or a test; the key is the one OpenAI answers with
//   "hosted"         whether a hosted server may be sent the book's text
// Nothing goes to a server unless the person chose one for it: automatic
// never does. As with recognition, a server is one's own (this computer,
// the tailnet, an address listed under "recognition"'s "own") unless hosted
// ones are allowed, and none is asked while the AI is off.
namespace Voices {

// The engine that reads with these settings: "piper", "speech-dispatcher",
// "server", "openai" or "gemini"; or why there is none that may (a server by AiPrivacy's rule, for
// text to speech, with `asked` what the one asking said of hosted ones).
[[nodiscard]] std::expected<QString, Failure> engine(const Settings &settings, AiPrivacy::HostedAsked asked = std::nullopt);
// That engine's voice, made; or why there is none. Asked through
// AiRegistry::voice, as every use of a model is.
[[nodiscard]] std::expected<std::unique_ptr<Voice>, Failure> choose(const Settings &settings, AiPrivacy::HostedAsked asked = std::nullopt);
// The voices it has to choose from, by name; none when there is no engine.
[[nodiscard]] QStringList offered(const Settings &settings);
// How fast and in which voice, as chosen for the engine in use, for a book in `language`.
[[nodiscard]] Speech speech(const Settings &settings, const QString &language);
// Keeps the voice or the rate ("voice", "rate") chosen for the engine in use.
void keep(Settings &settings, const QString &field, const QJsonValue &value);
// What reads aloud as the settings have it, to tell when that changed: the
// engine and its set-up, not its voice or pace; empty when nothing may read.
[[nodiscard]] QString reader(const Settings &settings);
// Where OpenAI's voices are asked ("openai_url", else OpenAI's own), and
// whether there is a key for it.
[[nodiscard]] QString openAiUrl(const Settings &settings);
[[nodiscard]] bool openAiHasKey(const Settings &settings);
// What to call an engine: "Piper", "speech-dispatcher", "OpenAI", "Gemini"; a server by its id.
[[nodiscard]] QString nameOf(const QString &engine);
// Whether the engine in use reads at the pace it is asked (Gemini's doesn't).
[[nodiscard]] bool paced(const Settings &settings);

} // namespace Voices
