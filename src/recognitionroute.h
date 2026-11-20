#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <expected>
#include <memory>
#include <vector>

#include "recognition.h"

class Settings;

// Which ways a recognition goes, from the settings (#84), with the privacy
// rule: handwriting and speech go only to the person's own servers (this
// computer, the tailnet, an address they marked as theirs), unless they allowed
// hosted ones in the settings *and* the one asking (a plugin, for one
// notebook) asked for it. A model whose name says it runs elsewhere
// (`...:cloud`) is someone else's computer however near its server is.
//
// config.json's "recognition" holds what is chosen:
//   "ink_model"            the vision model handwriting is read by; else the
//                          AI server's main model, else the first it lists
//   "transcription_url"    a server of the OpenAI-compatible API that
//                          transcribes; else the AI server itself
//   "transcription_model"  the model it transcribes with ("whisper-1" unless said)
//   "whisper_program", "whisper_model"   whisper.cpp on this computer, the way
//                          speech goes when no server transcribes it
//   "own"                  addresses of servers the person calls their own
//   "hosted"               whether a hosted server may be asked at all
namespace RecognitionRoute {

using Ways = std::vector<std::unique_ptr<Recognition::Recognizer>>;

// Why a server at this address, asked with this model, may not be sent the
// input; empty when it may.
[[nodiscard]] QString whyNot(const Settings &settings, const QString &baseUrl, const QString &model, bool hostedAsked);

// The ways to try, in order, none of them started: for ink the vision model,
// for speech the transcription server and then whisper.cpp. Fails, in plain
// words, when there is none that may be used (the privacy rule says why).
[[nodiscard]] std::expected<Ways, QString> ways(const Settings &settings, Recognition::Kind kind, bool hostedAsked);

} // namespace RecognitionRoute
