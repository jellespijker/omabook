#pragma once

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QtTypes>
#include <expected>
#include <optional>

#include "failure.h"

// The transcripts of recordings, kept on this computer (#151): what the
// speech-to-text capability wrote out of a recording of the library, or of
// a plugin's, kept so that it is written out once, read as a book (a
// recording in the library, RecordingBook) and searched.
//
// A long recording is written out a piece at a time (Transcriber), and each
// piece is kept the moment it is written out, so stopping loses at most the
// piece under way and starting again goes on from there. Kept by the key
// of the recording's bytes, the SHA-256 the library knows a book's file by
// (Library::hashFile), so a recording in the library is found by its file's
// hash, and the same recording anywhere is written out once.
//
// In the folder, one folder a recording, by its key:
//   plan.json         {piece, count}: how long each piece is, in milliseconds, and how many there are
//   piece-NNNNN.json  a piece written out: a Recognition::Result's JSON, its moments from the start of the recording
//   transcript.json   the whole, once every piece is written out
// and files/<hash of the path>.json, {path, size, modified, key}, so that a
// recording that didn't change is not read whole again to know its key.
//
// Never in the change log: a transcript can be hours of words, and each
// computer writes out what it has (the meaning index is kept so, ADR 0007).
namespace TranscriptStore {

// Where transcripts are kept: the data folder's omabook/transcripts.
[[nodiscard]] QString folder();

// The key of the recording at `file`: remembered while its size and time
// stay as they are, else read from its bytes.
[[nodiscard]] std::expected<QString, Failure> keyOf(const QString &folder, const QString &file);
// Remembers the key of a file whose hash is known already (the library's).
void remember(const QString &folder, const QString &file, const QString &key);

// The pieces a recording is written out in, and those done, by their number from 0.
struct Pieces {
    qint64 piece = 0; // milliseconds; 0 when none were planned
    int count = 0;
    QMap<int, QJsonObject> done;
};
[[nodiscard]] Pieces pieces(const QString &folder, const QString &key);
// Plans the pieces; pieces kept by another plan (another length) go.
[[nodiscard]] std::expected<void, Failure> plan(const QString &folder, const QString &key, qint64 piece, int count);
[[nodiscard]] std::expected<void, Failure> keepPiece(const QString &folder, const QString &key, int index, const QJsonObject &piece);
[[nodiscard]] std::expected<void, Failure> keepWhole(const QString &folder, const QString &key, const QJsonObject &transcript);
// The whole transcript, once there is one.
[[nodiscard]] std::optional<QJsonObject> whole(const QString &folder, const QString &key);
// Everything kept of a recording goes, to be written out anew.
void forget(const QString &folder, const QString &key);

} // namespace TranscriptStore
