#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <expected>
#include <memory>
#include <optional>
#include <vector>

#include "aicapability.h"
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
//   "ink_url"              a server of the OpenAI-compatible API handwriting
//                          is read by (#170); else the AI server itself
//   "ink_model"            the vision model handwriting is read by; else, on
//                          the AI server, its main model, else the first it lists
//   "transcription_url"    a server of the OpenAI-compatible API that
//                          transcribes; else the AI server itself
//   "transcription_model"  the model it transcribes with ("whisper-1" unless said)
//   "voxtype_program", "voxtype_model"   voxtype on this computer ("voxtype"
//                          on the path, with the model it was set up with)
//   "whisper_program", "whisper_model"   whisper.cpp on this computer
//   "own"                  addresses of servers the person calls their own
//   "hosted"               whether a hosted server may be asked at all
//
// Speech goes to voxtype, then whisper.cpp, then the server (#146, ADR 0008),
// each where it is there; where the person chose a server for it or
// whisper.cpp (before there was voxtype), to the server, then whisper.cpp,
// then voxtype, as it always went.
//
// With the AI set to Off (#133) there is no way at all: nothing is asked of
// any server, nor of whisper.cpp or voxtype.
namespace RecognitionRoute {

using Ways = std::vector<std::unique_ptr<Recognition::Recognizer>>;

// Why nothing may be recognised, as a plugin is told it
// (`omabook.recognize.status`): the registry's codes (aicapability.h), of
// which recognition says AiOff, NoServer (and, for speech, no whisper.cpp),
// HostedNotAllowed, HostedNotChosen and NoModel (learnt by asking).
using Lack = CapabilityLack;

struct Refusal {
    Lack lack = Lack::NoServer;
    QString message;
};

// Why a server at this address, asked with this model, may not be sent the
// input; empty when it may.
[[nodiscard]] QString whyNot(const Settings &settings, const QString &baseUrl, const QString &model, bool hostedAsked);

// Why there is no way that may be used for this kind; none when there is one.
// Nothing is asked of the network: whether the server has a model for it is
// known only once it was asked (RecognitionQueue::lacking).
[[nodiscard]] std::optional<Refusal> refusal(const Settings &settings, Recognition::Kind kind, bool hostedAsked);

// What the ways are, where each goes and with what model, as far as known
// before any is asked: two that are alike go the same way.
[[nodiscard]] QString signature(const Ways &ways);

// The command voxtype is run as: the settings', else "voxtype" on the path.
[[nodiscard]] QString voxtypeProgram(const Settings &settings);

// The ways to try, in order, none of them started: for ink the vision model,
// for speech those above. Fails, in plain
// words, when there is none that may be used (the privacy rule says why).
[[nodiscard]] std::expected<Ways, QString> ways(const Settings &settings, Recognition::Kind kind, bool hostedAsked);
// Where the first server can't be reached and the ways go to its fallback
// instead (#174, AiFallback), what the first is called; empty when they don't.
[[nodiscard]] QString insteadOf(const Settings &settings, Recognition::Kind kind, bool hostedAsked);

} // namespace RecognitionRoute
