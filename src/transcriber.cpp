#include "transcriber.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <QtConcurrentRun>
#include <algorithm>
#include <tuple>
#include <utility>

#include "failure.h"
#include "recordingpieces.h"

using namespace Qt::StringLiterals;
using std::chrono::milliseconds;

struct Transcriber::Job {
    int id = 0;
    Request request;
    Done done;
    Progressed progressed;
    RecognitionQueue::Waiting waiting;
    QString key;
    int count = 0;
    QMap<int, Recognition::Result> pieces; // written out, by their number
    int asked = 0;                         // the queue's number of the piece under way, 0 for none
    std::unique_ptr<QTemporaryDir> scratch;
};

namespace {

// A piece's moments, from its own start to the recording's.
[[nodiscard]] Recognition::Result movedBy(Recognition::Result piece, qint64 offset) {
    for (Recognition::Segment &segment : piece.segments) {
        segment.start += offset;
        segment.end += offset;
        for (Recognition::Word &word : segment.words) {
            word.start += offset;
            word.end += offset;
        }
    }
    return piece;
}

[[nodiscard]] QString stopped() { return Failure(Failure::Code::TranscriptionStopped).text(); }

} // namespace

Transcriber::Transcriber(RecognitionQueue::Route route, QString folder, QObject *parent)
    : QObject(parent), m_route(route), m_folder(std::move(folder)), m_queue(std::move(route), [] { return nullptr; }) {}

Transcriber::~Transcriber() = default;

Transcriber::Kept Transcriber::kept(const QString &key) const {
    if (key.isEmpty())
        return {};
    const TranscriptStore::Pieces pieces = TranscriptStore::pieces(m_folder, key);
    const bool whole = TranscriptStore::whole(m_folder, key).has_value();
    return {.done = whole ? pieces.count : static_cast<int>(pieces.done.size()), .total = pieces.count, .whole = whole};
}

Recognition::Result Transcriber::joined(const QList<Recognition::Result> &pieces) {
    Recognition::Result whole;
    whole.kind = Recognition::Kind::Audio;
    QStringList texts;
    bool timed = true;
    bool said = false;
    for (const Recognition::Result &piece : pieces) {
        whole.segments.append(piece.segments);
        if (const QString text = piece.text.trimmed(); !text.isEmpty()) {
            texts << text;
            said = true;
            // A piece in which nothing was said has no word to time.
            timed = timed && piece.timed;
        }
        if (whole.language.isEmpty())
            whole.language = piece.language;
        if (whole.model.isEmpty())
            whole.model = piece.model;
        if (whole.server.isEmpty())
            whole.server = piece.server;
    }
    whole.text = texts.join(u' ');
    whole.timed = said && timed;
    whole.at = QDateTime::currentMSecsSinceEpoch();
    return whole;
}

int Transcriber::transcribe(Request request, Done done, Progressed progressed, RecognitionQueue::Waiting waiting) {
    auto job = std::make_unique<Job>();
    job->id = m_nextJob++;
    job->request = std::move(request);
    job->done = std::move(done);
    job->progressed = std::move(progressed);
    job->waiting = std::move(waiting);
    const int id = job->id;
    m_jobs.push_back(std::move(job));
    if (!m_running)
        QMetaObject::invokeMethod(this, &Transcriber::next, Qt::QueuedConnection);
    return id;
}

bool Transcriber::stop(int job) {
    const auto found = std::ranges::find(m_jobs, job, [](const std::unique_ptr<Job> &each) { return each->id; });
    if (found == m_jobs.end())
        return false;
    if (found == m_jobs.begin() && m_running) {
        // The queue tells of the piece it stops too: the job has gone by then, and nobody listens.
        const int asked = std::exchange((*found)->asked, 0);
        finish(std::unexpected(stopped()));
        if (asked != 0)
            m_queue.cancel(asked);
        return true;
    }
    const std::unique_ptr<Job> waiting = std::move(*found);
    m_jobs.erase(found);
    waiting->done(std::unexpected(stopped()));
    return true;
}

bool Transcriber::current(int id) const { return m_running && !m_jobs.empty() && m_jobs.front()->id == id; }

void Transcriber::next() {
    if (m_running || m_jobs.empty())
        return;
    m_running = true;
    const Job &job = *m_jobs.front();
    // Nothing is read of a recording no way may hear: with the AI Off, or only hosted ways and none allowed.
    if (const auto ways = m_route(Recognition::Kind::Audio, job.request.hosted); !ways) {
        finish(std::unexpected(ways.error()));
        return;
    }
    const int id = job.id;
    // Its end comes back through the continuation, never waited for; it is dropped when this goes.
    std::ignore = QtConcurrent::run([file = job.request.file, key = job.request.key, folder = m_folder] {
                      Prepared made;
                      if (!key.isEmpty()) {
                          made.key = key;
                          TranscriptStore::remember(folder, file, key);
                      } else if (const auto read = TranscriptStore::keyOf(folder, file)) {
                          made.key = *read;
                      } else {
                          made.why = read.error().text();
                          return made;
                      }
                      if (const auto length = RecordingPieces::length(file))
                          made.length = *length;
                      else
                          made.why = length.error().text();
                      return made;
                  }).then(this, [this, id](const Prepared &made) {
        if (current(id))
            prepared(*m_jobs.front(), made);
    });
}

void Transcriber::prepared(Job &job, const Prepared &made) {
    if (!made.why.isEmpty()) {
        finish(std::unexpected(made.why));
        return;
    }
    job.key = made.key;
    if (job.request.again)
        TranscriptStore::forget(m_folder, job.key);
    else if (const auto whole = TranscriptStore::whole(m_folder, job.key))
        if (auto kept = Recognition::Result::fromJson(*whole)) {
            kept->kept = true;
            finish(*kept);
            return;
        }
    const qint64 piece = m_piece.count();
    job.count = static_cast<int>(std::max<qint64>(1, (made.length.count() + piece - 1) / piece));
    if (const auto planned = TranscriptStore::plan(m_folder, job.key, piece, job.count); !planned) {
        finish(std::unexpected(planned.error().text()));
        return;
    }
    const TranscriptStore::Pieces kept = TranscriptStore::pieces(m_folder, job.key);
    for (const auto &[index, json] : kept.done.asKeyValueRange())
        if (const auto result = Recognition::Result::fromJson(json))
            job.pieces.insert(index, *result);
    job.scratch = std::make_unique<QTemporaryDir>();
    step(job);
}

void Transcriber::step(Job &job) {
    const int id = job.id;
    const int total = job.count;
    const int done = static_cast<int>(job.pieces.size());
    // Whoever is told may stop it, and the job is gone then.
    const Progressed tell = job.progressed;
    const QString file = job.request.file;
    if (tell)
        tell({.done = done, .total = total});
    emit progressed(file, done, total);
    if (!current(id))
        return;
    int index = 0;
    while (job.pieces.contains(index))
        ++index;
    if (index >= job.count) {
        const Recognition::Result whole = joined(job.pieces.values());
        if (const auto kept = TranscriptStore::keepWhole(m_folder, job.key, whole.toJson()); !kept) {
            finish(std::unexpected(kept.error().text()));
            return;
        }
        finish(whole);
        return;
    }
    const milliseconds from{m_piece.count() * index};
    // As above: told through the continuation.
    std::ignore = QtConcurrent::run(RecordingPieces::cut, job.request.file, from, m_piece, job.scratch->filePath(u"piece.wav"_s))
                      .then(this, [this, id, index](const std::expected<void, Failure> &made) {
                          if (current(id))
                              cut(*m_jobs.front(), index, made);
                      });
}

void Transcriber::cut(Job &job, int index, const std::expected<void, Failure> &made) {
    if (!made) {
        finish(std::unexpected(made.error().text()));
        return;
    }
    const int id = job.id;
    RecognitionQueue::Request request{.input = {.kind = Recognition::Kind::Audio,
                                                .strokes = {},
                                                .file = job.scratch->filePath(u"piece.wav"_s),
                                                .language = job.request.language},
                                      .owner = {},
                                      .hosted = job.request.hosted,
                                      .again = false};
    job.asked = m_queue.submit(
        std::move(request),
        [this, id, index](const std::expected<Recognition::Result, QString> &outcome) {
            if (current(id))
                heard(*m_jobs.front(), index, outcome);
        },
        job.waiting);
}

void Transcriber::heard(Job &job, int index, const std::expected<Recognition::Result, QString> &outcome) {
    job.asked = 0;
    if (!outcome) {
        finish(std::unexpected(outcome.error()));
        return;
    }
    const Recognition::Result piece = movedBy(*outcome, m_piece.count() * index);
    if (const auto kept = TranscriptStore::keepPiece(m_folder, job.key, index, piece.toJson()); !kept) {
        finish(std::unexpected(kept.error().text()));
        return;
    }
    job.pieces.insert(index, piece);
    step(job);
}

void Transcriber::finish(const std::expected<Recognition::Result, QString> &outcome) {
    if (m_jobs.empty())
        return;
    const std::unique_ptr<Job> job = std::move(m_jobs.front());
    m_jobs.pop_front();
    m_running = false;
    job->done(outcome);
    next();
}
