#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <functional>

#include "transcriber.h"

class Library;
class Settings;

// Writing out the recordings of the library, from the window (#151): one at
// a time, in the background, shown at the top of the window for as long as
// it goes on (TranscribingMark.qml) with a way to stop it; stopped, it goes
// on from where it was when it is started again. A recording written out is
// read as a book (RecordingBook) and found by the shelf's search by what is
// said in it (BookWords). Only through the speech-to-text capability, as the
// person chose it: a hosted way only where they allowed it on the AI page
// (starting it here is their asking for it), and nothing with the AI Off.
// The plugins write theirs out through the same Transcriber, so one
// recording is written out at a time whoever asked.
class TranscriptDesk : public QObject {
    Q_OBJECT
    // A recording of the library is being written out, and which.
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(qint64 book READ book NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    // Of its pieces, how many are written out, of how many (0 of 0 while it is looked at).
    Q_PROPERTY(int done READ done NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    // Why the last one stopped short, or waits for a server, in the window's words; empty when it didn't.
    Q_PROPERTY(QString why READ why NOTIFY changed)
    // Goes up whenever what is kept of a recording changed, for what shows it.
    Q_PROPERTY(int revision READ revision NOTIFY changed)

public:
    using LibraryOf = std::function<Library *()>;
    TranscriptDesk(Settings *settings, LibraryOf library, QObject *parent = nullptr);

    [[nodiscard]] Transcriber *transcriber() noexcept { return &m_transcriber; }
    [[nodiscard]] bool busy() const noexcept { return m_job != 0; }
    [[nodiscard]] qint64 book() const noexcept { return m_book; }
    [[nodiscard]] QString title() const { return m_title; }
    [[nodiscard]] int done() const noexcept { return m_done; }
    [[nodiscard]] int total() const noexcept { return m_total; }
    [[nodiscard]] QString why() const { return m_why; }
    [[nodiscard]] int revision() const noexcept { return m_revision; }

    // Of a book: {recording, state, done, total, timed, unavailable}. `recording`
    // false for a book that isn't one. `state` is "none", "partly" (stopped
    // part way), "whole" or "writing"; `unavailable` why no way may write it
    // out now, in plain words, else empty.
    Q_INVOKABLE QVariantMap state(qint64 bookId) const;
    // Writes it out, or goes on with it; stops it when it is the one being written out.
    Q_INVOKABLE void toggle(qint64 bookId);
    Q_INVOKABLE void stop();

signals:
    void changed();

private:
    void ended(qint64 bookId, const std::expected<Recognition::Result, QString> &outcome);

    Settings *m_settings;
    LibraryOf m_library;
    Transcriber m_transcriber;
    int m_job = 0;
    qint64 m_book = 0;
    QString m_title, m_why;
    int m_done = 0, m_total = 0, m_revision = 0;
};
