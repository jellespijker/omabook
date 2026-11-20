#include "noterecognition.h"

#include <algorithm>
#include <ranges>
#include <utility>

#include "annotationink.h"
#include "library.h"
#include "log.h"

using namespace std::chrono_literals;

namespace {

constexpr auto marginNote = "margin-note";

// The ids of a note's strokes, to tell whether its ink changed while it was read.
[[nodiscard]] QStringList idsOf(const std::vector<Ink::Stroke> &strokes) {
    QStringList ids;
    for (const Ink::Stroke &stroke : strokes)
        ids << stroke.id;
    return ids;
}

} // namespace

NoteRecognition::NoteRecognition(RecognitionQueue::Route route, RecognitionQueue::LibraryOf library, QObject *parent)
    : QObject(parent), m_library(library), m_queue(std::move(route), std::move(library), this) {
    m_pause.setSingleShot(true);
    m_pause.setInterval(3s);
    connect(&m_pause, &QTimer::timeout, this, [this] {
        const QHash<qint64, qint64> waiting = std::exchange(m_waiting, {});
        for (auto note = waiting.cbegin(); note != waiting.cend(); ++note)
            submit(note.value(), note.key(), false);
    });
}

void NoteRecognition::setPause(std::chrono::milliseconds pause) { m_pause.setInterval(pause); }

void NoteRecognition::soon(qint64 bookId, qint64 note) {
    m_waiting.insert(note, bookId);
    m_pause.start();
}

void NoteRecognition::now(qint64 bookId, qint64 note) {
    m_waiting.remove(note);
    submit(bookId, note, true);
}

void NoteRecognition::setState(qint64 note, const QString &state) {
    if (m_states.value(note) == state)
        return;
    if (state.isEmpty())
        m_states.remove(note);
    else
        m_states.insert(note, state);
    emit stateChanged(note);
}

void NoteRecognition::submit(qint64 bookId, qint64 note, bool asked) {
    const Library *library = m_library ? m_library() : nullptr;
    if (library == nullptr)
        return;
    std::vector<Ink::Stroke> strokes = AnnotationInk::strokes(*library, note);
    if (strokes.empty())
        return;
    // What is being read of the note's ink before is of ink it no longer has.
    if (const auto earlier = m_jobs.constFind(note); earlier != m_jobs.cend())
        m_queue.cancel(*earlier);
    const QStringList ids = idsOf(strokes);
    setState(note, tr("Reading the handwriting…"));
    const int job = m_queue.submit(
        {.input = {.kind = Recognition::Kind::Ink, .strokes = std::move(strokes), .file = {}, .language = {}},
         .owner = QString::fromLatin1(owner),
         .hosted = asked,
         .again = false},
        [this, bookId, note, ids](const std::expected<Recognition::Result, QString> &outcome) { done(bookId, note, ids, outcome); },
        [this, note](const QString &why) { setState(note, tr("Waiting to read the handwriting: %1").arg(why)); });
    m_jobs.insert(note, job);
}

void NoteRecognition::done(qint64 bookId, qint64 note, const QStringList &strokes,
                           const std::expected<Recognition::Result, QString> &outcome) {
    m_jobs.remove(note);
    if (!outcome) {
        // One cancelled for newer ink is said by the one after it.
        if (!m_jobs.contains(note) && !m_waiting.contains(note))
            setState(note, outcome.error());
        return;
    }
    Library *library = m_library ? m_library() : nullptr;
    if (library == nullptr)
        return;
    const QList<Annotation> notes = library->annotations(bookId, QString::fromLatin1(marginNote));
    const auto found = std::ranges::find(notes, note, &Annotation::id);
    // Removed meanwhile, or written on since: the newer ink is read next.
    if (found == notes.cend() || idsOf(AnnotationInk::strokes(*library, note)) != strokes) {
        setState(note, {});
        return;
    }
    const QString words = outcome->text.trimmed();
    if (words.isEmpty()) {
        setState(note, tr("No words were read in the handwriting."));
        return;
    }
    setState(note, {});
    if (found->note == words)
        return;
    Annotation changed = *found;
    changed.note = words;
    if (!library->storeAnnotation(bookId, changed)) {
        qCWarning(lcLibrary) << "The words read of a margin note couldn't be kept.";
        return;
    }
    emit read(note);
}
