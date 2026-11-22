#include "recordingbook.h"

#include <QFileInfo>
#include <QJsonArray>
#include <array>
#include <ranges>

#include "booksource.h"
#include "formats.h"
#include "transcriptstore.h"

using namespace Qt::StringLiterals;

namespace {

// What Qt Multimedia and ffmpeg play, and recorders and audiobooks are kept as.
constexpr auto recordings = std::to_array({"mp3"_L1, "m4a"_L1, "m4b"_L1, "aac"_L1, "ogg"_L1, "oga"_L1, "opus"_L1, "flac"_L1, "wav"_L1});

} // namespace

QStringList RecordingBook::extensions() {
    return recordings | std::views::transform([](QLatin1StringView extension) { return QString(extension); })
           | std::ranges::to<QStringList>();
}

bool RecordingBook::isRecording(const QString &extension) { return std::ranges::contains(recordings, extension); }

QString RecordingBook::clock(qint64 milliseconds) {
    const qint64 seconds = milliseconds / 1000;
    const auto two = [](qint64 number) { return u"%1"_s.arg(number, 2, 10, QChar(u'0')); };
    if (seconds >= 3600)
        return u"%1:%2:%3"_s.arg(seconds / 3600).arg(two(seconds / 60 % 60), two(seconds % 60));
    return u"%1:%2"_s.arg(seconds / 60).arg(two(seconds % 60));
}

BookDescription RecordingBook::describe(const QJsonObject &transcript, const BookMeta &meta) {
    BookDescription description;
    description.meta = meta;
    if (description.meta.language.isEmpty())
        description.meta.language = transcript.value("language"_L1).toString();
    // A chapter for each ten minutes that has words, named by when it begins.
    qint64 chapter = -1;
    QString html;
    const auto close = [&] {
        if (chapter >= 0 && !html.isEmpty())
            description.chapters.append({.title = clock(chapter * chapterLength), .html = html, .file = {}, .level = 0});
        html.clear();
    };
    for (const auto &value : transcript.value("segments"_L1).toArray()) {
        const QJsonObject segment = value.toObject();
        const QString text = segment.value("text"_L1).toString().trimmed();
        if (text.isEmpty())
            continue;
        if (const qint64 at = segment.value("start"_L1).toInteger() / chapterLength; at != chapter) {
            close();
            chapter = at;
        }
        html += u"<p>"_s + text.toHtmlEscaped() + u"</p>"_s;
    }
    close();
    // Written out, and nothing was said: the book says so in the recording's own words, none.
    if (description.chapters.isEmpty())
        description.chapters.append({.title = clock(0), .html = u"<p></p>"_s, .file = {}, .level = 0});
    return description;
}

OpenedDocument RecordingBook::open(const QString &path, const QString &folder) {
    const QString name = QFileInfo(path).fileName();
    const auto key = TranscriptStore::keyOf(folder, path);
    if (!key)
        return std::unexpected(key.error());
    const auto transcript = TranscriptStore::whole(folder, *key);
    if (!transcript)
        return std::unexpected(Failure(Failure::Code::NotTranscribed, {name}));
    return Formats::fromDescription(describe(*transcript, Formats::metaFromFileName(path)), BookSource(path));
}

OpenedDocument openRecording(const QString &path) { return RecordingBook::open(path, TranscriptStore::folder()); }
