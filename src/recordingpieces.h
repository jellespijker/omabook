#pragma once

#include <QString>
#include <chrono>
#include <expected>

#include "failure.h"

// A recording cut into pieces to be written out one at a time (#151): how
// long it is, and a stretch of it as a WAV of 16 kHz mono, which every way
// of writing out speech takes (voxtype, whisper.cpp, a server, Gemini's
// inline audio). A WAV of 16-bit samples and an Opus recording in Ogg (a
// plugin's, #79) are read here; anything else (MP3, M4B, FLAC...) through
// ffmpeg, which says how long it is too (ffprobe). Each call reads only
// what it needs, so an audiobook of hours is never decoded whole. Blocking:
// run it beside the window.
namespace RecordingPieces {

// The rate the pieces are written at.
inline constexpr int sampleRate = 16000;

[[nodiscard]] std::expected<std::chrono::milliseconds, Failure> length(const QString &file);
// The stretch of `file` from `from` on, `length` long (less at its end), as a WAV at `wav`.
[[nodiscard]] std::expected<void, Failure> cut(const QString &file, std::chrono::milliseconds from, std::chrono::milliseconds length,
                                               const QString &wav);

} // namespace RecordingPieces
