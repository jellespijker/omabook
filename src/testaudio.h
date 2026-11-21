#pragma once

#include <QString>
#include <memory>
#include <optional>

#include "audiodevices.h"

// Sound for trying a plugin out where there is no sound card or nobody to
// speak (#135): `omabook plugin run --test-microphone`. The microphone hears
// what it is given, as fast as a real one would, over and over; the speakers
// take what is played as fast as a real pair would, and nobody hears it. A
// recording then goes as it does in the window, into the plugin's data/.
namespace TestAudio {

// What the microphone hears: "tone" (an A at 440 Hz, half as loud as it can
// be), "silence", or a WAV file of mono 16-bit samples at 48 kHz, as a
// recording keeps them (AudioPorts::format). None for anything else: a
// file that isn't there, isn't WAV, holds no sound or holds another kind.
[[nodiscard]] std::optional<AudioDevices::Inputs> microphone(const QString &hears);
[[nodiscard]] std::unique_ptr<AudioOutput> speakers();

} // namespace TestAudio
