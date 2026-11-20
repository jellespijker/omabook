#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QtTypes>

#include <chrono>

#include "recognitionqueue.h"

class Library;

// Handwritten margin notes read (#90): the words of a note's ink, read by the
// recognition service (#84) in the background once the pen has rested on it
// a while, and written as the note's words, so search finds it, the list of
// lost notes says it, and an Omabook from before shows it as a typed note.
// The note itself stays its ink; its words are shown when the reader asks.
//
// Only the computer the ink was written on reads it, and only on the person's
// own servers (RecognitionRoute): a note is sent to a hosted one only when the
// reader asks for it to be read *and* the settings allow hosted servers. What
// was read is kept in the change log by the ink's key (RecognitionStore), so
// ink written back to what it was is not read again.
class NoteRecognition : public QObject {
    Q_OBJECT

public:
    // Whose data a result is kept with, as a plugin's is with its id.
    static constexpr auto owner = "omabook";

    NoteRecognition(RecognitionQueue::Route route, RecognitionQueue::LibraryOf library, QObject *parent = nullptr);

    // The note of that book is read once no more ink has come to it for the pause.
    void soon(qint64 bookId, qint64 note);
    // Read now, as the reader asked: a hosted server too, where the settings allow one.
    void now(qint64 bookId, qint64 note);
    void setPause(std::chrono::milliseconds pause);
    // How it goes with a note: that it is being read, or why it wasn't; empty when neither.
    [[nodiscard]] QString state(qint64 note) const { return m_states.value(note); }

signals:
    // Its state changed; and its words were written, when they were.
    void stateChanged(qint64 note);
    void read(qint64 note);

private:
    void submit(qint64 bookId, qint64 note, bool asked);
    void done(qint64 bookId, qint64 note, const QStringList &strokes, const std::expected<Recognition::Result, QString> &outcome);
    void setState(qint64 note, const QString &state);

    RecognitionQueue::LibraryOf m_library;
    RecognitionQueue m_queue;
    QTimer m_pause;
    QHash<qint64, qint64> m_waiting; // note: its book, until the pause is over
    QHash<qint64, int> m_jobs;       // note: its job in the queue
    QHash<qint64, QString> m_states;
};
