#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <expected>
#include <optional>
#include <vector>

#include "inkstroke.h"

// Recognition (#84): recognised text from handwriting and transcripts from
// speech, for Omabook's own ink and for plugins (`omabook.recognize`). This is
// the port: what is recognised, what comes back, and the one interface every
// way of recognising (a vision model on an AI server, a transcription server,
// whisper.cpp or voxtype on this computer; see recognizers.h, whisperlocal.h,
// voxtypelocal.h) answers to.
// Which way is taken, and whether a server may be asked at all, is
// RecognitionRoute's; waiting, keeping and cancelling is RecognitionQueue's.
namespace Recognition {

enum class Kind { Ink, Audio };
// "ink" or "audio", as a plugin and the log name it.
[[nodiscard]] QString kindName(Kind kind);
[[nodiscard]] std::optional<Kind> kindNamed(const QString &name);

// A word with its moment, in milliseconds from the start of the recording.
struct Word {
    QString text;
    qint64 start = 0;
    qint64 end = 0;
};

// A stretch of what was said (or the whole of what was written), with its
// words when the way it was recognised gives them.
struct Segment {
    QString text;
    qint64 start = 0;
    qint64 end = 0;
    QList<Word> words;
};

struct Result {
    Kind kind = Kind::Ink;
    QString text;
    QList<Segment> segments;
    QString language;  // as the model tells it, when it does
    QString model;     // the model that recognised it
    QString server;    // where: "Ollama at localhost:11434", "whisper.cpp on this computer"
    qint64 at = 0;     // when, in milliseconds since the epoch
    bool kept = false; // it was found among the results kept, not recognised now
    // Each word has its moment. Not every way gives them (voxtype doesn't,
    // nor a server that answers only the text, #146): then each segment has
    // no words, and this says so plainly.
    bool timed = false;

    // {kind, text, language, model, server, at, kept, timed,
    //  segments: [{text, start, end, words: [{text, start, end}]}]}
    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<Result> fromJson(const QJsonObject &json);
};

// What is to be recognised: strokes, or a recording by its file.
struct Input {
    Kind kind = Kind::Ink;
    std::vector<Ink::Stroke> strokes;
    QString file;
    // The language it is in ("en", "nl"), as a hint; empty for the model to tell.
    QString language;
};

// The key a result is kept under: the hash of what was recognised (the
// strokes' points, not their ids; the recording's bytes), its language hint
// and the model, so the same ink read by the same model is read once, on
// whichever computer, and a change of the ink or the model is read anew.
// Fails, in plain words, when there is nothing to read.
[[nodiscard]] std::expected<QString, QString> key(const Input &input, const QString &model);

// Why a way of recognising didn't answer, and so what is done next.
struct Failure {
    enum class Why {
        Refused,     // it answered no (a model it hasn't, a key it won't take): said as it is
        Away,        // it can't be reached now: waited for, and tried again
        Unsupported, // it has no such service: the next way is tried
    };
    Why why = Why::Refused;
    QString message;
    // It lacks what was asked for: the model, one that reads pictures, or
    // transcription at all (#133), so it would fail the same way again.
    bool lacking = false;
};

// One way of recognising. Each recognises one input at a time and says how
// it went with one of its signals, never both, and neither after cancel().
class Recognizer : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Where the input goes, for the result and for a failure to name it.
    [[nodiscard]] virtual QString where() const = 0;
    // The model it reads with: known once resolved.
    [[nodiscard]] virtual QString model() const = 0;
    // Whether what it hears comes with each word's moment, as far as known
    // before it is asked (Result::timed says it of each result).
    [[nodiscard]] virtual bool timesWords() const { return false; }
    // Finds the model when none was named (by asking the server), then says
    // resolved(); one that was named is resolved at once, a moment later.
    virtual void resolve();
    virtual void recognize(const Input &input) = 0;
    virtual void cancel() = 0;

signals:
    void resolved();
    void recognized(const Recognition::Result &result);
    void failed(const Recognition::Failure &failure);
};

// Where a server is, for the privacy rule: on this computer, on the tailnet,
// at an address the person marked as their own, or someone else's.
enum class Reach { ThisComputer, Tailnet, Own, Hosted };
[[nodiscard]] Reach reach(const QString &baseUrl, const QStringList &own = {});

} // namespace Recognition
