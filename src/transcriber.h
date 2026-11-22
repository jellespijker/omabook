#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <chrono>
#include <expected>
#include <functional>
#include <list>
#include <memory>

#include "recognition.h"
#include "recognitionqueue.h"
#include "transcriptstore.h"

// Writing out recordings (#151): a recording of the library, or a plugin's,
// written out through the speech-to-text capability, in the background,
// one recording at a time and a piece of it at a time (five minutes, which
// every way takes at once, Gemini's inline audio too). Each piece goes the
// ways RecognitionRoute gives (voxtype first, a hosted one only where it is
// allowed and asked for, none with the AI Off), through a RecognitionQueue
// of its own, which waits for a server that is away; and each is kept the
// moment it is written out (TranscriptStore), so a recording of hours can
// be stopped and goes on, when started again, from the piece it was at.
// Kept whole, it is answered from what was kept without asking anyone.
//
// A word's moment is kept where the way gives one (Result::timed), counted
// from the start of the recording, not of its piece. A word said across the
// moment two pieces meet may come out in either, or in both halves.
class Transcriber : public QObject {
    Q_OBJECT

public:
    struct Request {
        QString file;
        // The language it is in ("en"), as a hint; empty for the model to tell.
        QString language;
        // A hosted way may be asked, where the settings allow one for speech to text.
        bool hosted = false;
        // Written out anew, though it is kept.
        bool again = false;
        // The recording's key (TranscriptStore), when it is known already: the library's hash of its file.
        QString key;
    };
    struct Progress {
        int done = 0;
        int total = 0;
    };
    // Called once for each request: the whole transcript, or why there is
    // none in plain words (TranscriptionStopped when it was stopped).
    using Done = std::function<void(const std::expected<Recognition::Result, QString> &)>;
    using Progressed = std::function<void(Progress)>;

    explicit Transcriber(RecognitionQueue::Route route, QString folder = TranscriptStore::folder(), QObject *parent = nullptr);
    ~Transcriber() override;
    Q_DISABLE_COPY_MOVE(Transcriber)

    static constexpr std::chrono::milliseconds defaultPiece = std::chrono::minutes(5);
    void setPieceLength(std::chrono::milliseconds piece) { m_piece = piece; }
    void setRetry(std::chrono::milliseconds first, std::chrono::milliseconds most) { m_queue.setRetry(first, most); }
    [[nodiscard]] QString folder() const { return m_folder; }

    // Queues a recording; its number, for stop().
    int transcribe(Request request, Done done, Progressed progressed = {}, RecognitionQueue::Waiting waiting = {});
    // Stops one, under way or waiting its turn, keeping what was written out; false when there is no such one.
    bool stop(int job);
    [[nodiscard]] qsizetype pending() const { return static_cast<qsizetype>(m_jobs.size()); }

    // What is kept of the recording with this key: the pieces written out of
    // how many (none planned yet: 0 of 0), and whether it is whole.
    struct Kept {
        int done = 0;
        int total = 0;
        bool whole = false;
    };
    [[nodiscard]] Kept kept(const QString &key) const;

    // The pieces' transcripts, in order, as one.
    [[nodiscard]] static Recognition::Result joined(const QList<Recognition::Result> &pieces);

signals:
    // A piece of the recording at `file` was written out.
    void progressed(const QString &file, int done, int total);

private:
    struct Job;
    struct Prepared {
        QString key;
        std::chrono::milliseconds length{0};
        QString why;
    };

    void next();
    void prepared(Job &job, const Prepared &made);
    void step(Job &job);
    void cut(Job &job, int index, const std::expected<void, Failure> &made);
    void heard(Job &job, int index, const std::expected<Recognition::Result, QString> &outcome);
    void finish(const std::expected<Recognition::Result, QString> &outcome);
    [[nodiscard]] bool current(int id) const;

    RecognitionQueue::Route m_route;
    QString m_folder;
    RecognitionQueue m_queue;
    std::list<std::unique_ptr<Job>> m_jobs; // the first is the one under way
    std::chrono::milliseconds m_piece = defaultPiece;
    int m_nextJob = 1;
    bool m_running = false;
};
