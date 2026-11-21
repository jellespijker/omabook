#pragma once

#include <QString>
#include <QStringList>

// What the ways of transcribing on this computer share (whisper.cpp, voxtype;
// #84, #146): finding their command, and making a recording the 16 kHz mono
// WAV they read, with ffmpeg, when it is in another format.
namespace LocalSpeech {

// The command a recording that isn't WAV is converted with.
inline constexpr auto converter = "ffmpeg";

// Where a command is: itself when it is a path that runs, else found on the
// path; empty when it isn't there.
[[nodiscard]] QString found(const QString &program);
// Whether a recording is a WAV, by its name.
[[nodiscard]] bool isWav(const QString &file);
// The converter's arguments that make `recording` a 16 kHz mono WAV at `wav`.
[[nodiscard]] QStringList conversion(const QString &recording, const QString &wav);

} // namespace LocalSpeech
