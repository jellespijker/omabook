#include "transcriptdesk.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "bookwords.h"
#include "failure.h"
#include "library.h"
#include "log.h"
#include "recognitionroute.h"
#include "recordingbook.h"
#include "settings.h"
#include "transcriptstore.h"

using namespace Qt::StringLiterals;

namespace {

// The book's recording, when it is one: the first of its files that is.
[[nodiscard]] std::optional<FormatFile> recordingOf(const Library &library, qint64 bookId) {
    const QList<FormatFile> files = library.formatFiles(bookId);
    const auto found = std::ranges::find_if(files, [](const FormatFile &file) { return RecordingBook::isRecording(file.format); });
    return found != files.end() ? std::optional(*found) : std::nullopt;
}

} // namespace

TranscriptDesk::TranscriptDesk(Settings *settings, LibraryOf library, QObject *parent)
    : QObject(parent), m_settings(settings), m_library(std::move(library)),
      m_transcriber(RecognitionQueue::routeFor(*settings), TranscriptStore::folder()) {}

QVariantMap TranscriptDesk::state(qint64 bookId) const {
    const Library *library = m_library ? m_library() : nullptr;
    const auto file = library != nullptr && bookId > 0 ? recordingOf(*library, bookId) : std::nullopt;
    if (!file)
        return {{u"recording"_s, false}};
    const Transcriber::Kept kept = m_transcriber.kept(file->hash);
    QString state = u"none"_s;
    if (busy() && m_book == bookId)
        state = u"writing"_s;
    else if (kept.whole)
        state = u"whole"_s;
    else if (kept.done > 0)
        state = u"partly"_s;
    const auto whole = kept.whole ? TranscriptStore::whole(m_transcriber.folder(), file->hash) : std::nullopt;
    const auto refused = RecognitionRoute::refusal(*m_settings, Recognition::Kind::Audio, true);
    return {{u"recording"_s, true},
            {u"state"_s, state},
            {u"done"_s, kept.done},
            {u"total"_s, kept.total},
            {u"timed"_s, whole && whole->value("timed"_L1).toBool()},
            {u"unavailable"_s, refused ? refused->message : QString()}};
}

void TranscriptDesk::toggle(qint64 bookId) {
    const bool same = busy() && m_book == bookId;
    stop();
    const Library *library = m_library ? m_library() : nullptr;
    const auto file = library != nullptr && !same ? recordingOf(*library, bookId) : std::nullopt;
    const auto book = file ? library->book(bookId) : std::nullopt;
    if (!book)
        return;
    m_book = bookId;
    m_title = book->meta.title;
    m_why.clear();
    m_done = 0;
    m_total = 0;
    // Started here, by the person: a hosted way is asked for, and taken only where they allowed one for speech to text.
    m_job = m_transcriber.transcribe(
        {.file = file->path, .language = book->meta.language, .hosted = true, .again = false, .key = file->hash},
        [this, bookId](const std::expected<Recognition::Result, QString> &outcome) { ended(bookId, outcome); },
        [this](Transcriber::Progress at) {
            m_done = at.done;
            m_total = at.total;
            m_why.clear();
            ++m_revision;
            emit changed();
        },
        [this](const QString &why) {
            m_why = why;
            emit changed();
        });
    ++m_revision;
    emit changed();
}

void TranscriptDesk::stop() {
    if (m_job != 0)
        m_transcriber.stop(m_job);
}

void TranscriptDesk::ended(qint64 bookId, const std::expected<Recognition::Result, QString> &outcome) {
    m_job = 0;
    m_why = outcome || outcome.error() == Failure(Failure::Code::TranscriptionStopped).text() ? QString() : outcome.error();
    if (const Library *library = m_library ? m_library() : nullptr; outcome && library != nullptr)
        // The shelf's search finds it by what is said in it from now on.
        if (const auto indexed = BookWords::index(*library, bookId, RecordingBook::extensions()); !indexed)
            qCWarning(lcAi).noquote() << "The words of recording" << bookId << "weren't kept for the search:" << indexed.error().english();
    ++m_revision;
    emit changed();
}
