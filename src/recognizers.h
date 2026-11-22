#pragma once

#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>
#include <cstdint>

#include "aiendpoint.h"
#include "recognition.h"

class QNetworkReply;

// The ways of recognising that ask a server of the OpenAI-compatible API
// (#84, the servers of #32): handwriting through a vision model, which is
// shown the ink as a picture (Chat Completions with an image), and speech
// through the transcription service (`/audio/transcriptions`, as whisper
// servers, LocalAI, speaches and OpenAI have it), each word with its moment.
// Whether a server may be asked is decided before one of these is made
// (RecognitionRoute); these only ask.
namespace Recognizers {

// The model a transcription is asked of when none is named: the name OpenAI
// gives Whisper, which most servers that speak its API take for their own.
inline constexpr auto defaultTranscriptionModel = "whisper-1";

class Vision : public Recognition::Recognizer {
    Q_OBJECT

public:
    // Whose server it is: the AI server answers come from, or one named for
    // handwriting alone (`recognition.ink_url`, #170).
    enum class Whose : std::uint8_t { AiServer, Handwriting };

    // An empty model is the server's first that runs on it, asked when resolved.
    Vision(AiServer server, QString model, Whose whose = Whose::AiServer, QObject *parent = nullptr);
    ~Vision() override;
    Vision(const Vision &) = delete;
    Vision &operator=(const Vision &) = delete;
    Vision(Vision &&) = delete;
    Vision &operator=(Vision &&) = delete;

    [[nodiscard]] QString where() const override;
    [[nodiscard]] QString model() const override { return m_model; }
    void resolve() override;
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void read(QNetworkReply *reply);

    AiServer m_server;
    QString m_model;
    Whose m_whose;
    QString m_language;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};

class Transcription : public Recognition::Recognizer {
    Q_OBJECT

public:
    Transcription(AiServer server, QString model, QObject *parent = nullptr);
    ~Transcription() override;
    Transcription(const Transcription &) = delete;
    Transcription &operator=(const Transcription &) = delete;
    Transcription(Transcription &&) = delete;
    Transcription &operator=(Transcription &&) = delete;

    [[nodiscard]] QString where() const override;
    [[nodiscard]] QString model() const override { return m_model; }
    // Asked for them; a server that answers only the text says so in its result.
    [[nodiscard]] bool timesWords() const override { return true; }
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void read(QNetworkReply *reply);

    AiServer m_server;
    QString m_model;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};

// What a model that reads pictures is told of handwriting, with the
// language it is most likely in (empty for the model to tell).
[[nodiscard]] QString inkPrompt(const QString &language);
// The type a recording is sent as, by its file's name ("audio/ogg").
[[nodiscard]] QString mimeTypeOf(const QString &file);
// Why a request to a server failed, in plain words, and what is done about
// it: waited for and tried again, the next way tried, or said as it is.
[[nodiscard]] Recognition::Failure failure(const QNetworkReply &reply, const QByteArray &body, const QString &where, const QString &model,
                                           bool transcribing);

// A transcription's answer, `verbose_json` as OpenAI writes it (seconds,
// each word apart from the segments) or a plain `{text}`, as a result:
// moments in milliseconds, each word in the segment it was said in.
[[nodiscard]] Recognition::Result transcript(const QJsonObject &answer);

} // namespace Recognizers
