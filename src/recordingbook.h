#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtTypes>

#include "bookmeta.h"
#include "formatprovider.h"
#include "formatreaders.h"

// A recording in the library (#151), read as the book of what is said in
// it: its transcript (TranscriptStore), in chapters of ten minutes named by
// their moment, a paragraph for each stretch of speech. So the reader, the
// search in a book, the shelf's search (BookWords), the command line, the
// MCP server and the AI read a recording as they read any book, once it is
// written out; before that, opening it says how to have it written out.
namespace RecordingBook {

// The extensions of the recordings a library takes in, lower case.
[[nodiscard]] QStringList extensions();
[[nodiscard]] bool isRecording(const QString &extension);

// How long a chapter is, in milliseconds.
inline constexpr qint64 chapterLength = qint64{10} * 60 * 1000;

// The book a transcript (a Recognition::Result's JSON) makes, with these details.
[[nodiscard]] BookDescription describe(const QJsonObject &transcript, const BookMeta &meta);
// A moment as a clock: "4:05", "1:02:03".
[[nodiscard]] QString clock(qint64 milliseconds);

// The recording at `path`, written out, from the transcripts in `folder`;
// NotTranscribed when it isn't yet.
[[nodiscard]] OpenedDocument open(const QString &path, const QString &folder);

} // namespace RecordingBook
