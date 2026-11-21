#pragma once

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
//                    "piper", "speech-dispatcher", "server", or "gemini"
//                    (Gemini's voices, with the key in the keyring; #145)
//   "rate", "voice"  how fast, and which of the engine's voices
//   "piper_program"  Piper's command, when it isn't piper-tts or piper on the path
//   "url", "model"   the server of the OpenAI-compatible API that reads
//                    aloud (else the AI server), and its model ("tts-1")
//   "hosted"         whether a hosted server may be sent the book's text
// Nothing goes to a server unless the person chose one for it: automatic
// never does. As with recognition, a server is one's own (this computer,
// the tailnet, an address listed under "recognition"'s "own") unless hosted
// ones are allowed, and none is asked while the AI is off.
namespace Voices {

// The engine that reads with these settings: "piper", "speech-dispatcher",
// "server" or "gemini"; or why there is none that may (a server by AiPrivacy's rule, for
// text to speech, with `asked` what the one asking said of hosted ones).
[[nodiscard]] std::expected<QString, Failure> engine(const Settings &settings, AiPrivacy::HostedAsked asked = std::nullopt);
// That engine's voice, made; or why there is none. Asked through
// AiRegistry::voice, as every use of a model is.
[[nodiscard]] std::expected<std::unique_ptr<Voice>, Failure> choose(const Settings &settings, AiPrivacy::HostedAsked asked = std::nullopt);
// The voices it has to choose from, by name; none when there is no engine.
[[nodiscard]] QStringList offered(const Settings &settings);
// How fast and in which voice, as chosen, for a book in `language`.
[[nodiscard]] Speech speech(const Settings &settings, const QString &language);

} // namespace Voices
