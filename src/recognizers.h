#pragma once

#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>

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
    // An empty model is the server's first that runs on it, asked when resolved.
    Vision(AiServer server, QString model, QObject *parent = nullptr);
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
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void read(QNetworkReply *reply);

    AiServer m_server;
    QString m_model;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};

// A transcription's answer, `verbose_json` as OpenAI writes it (seconds,
// each word apart from the segments) or a plain `{text}`, as a result:
// moments in milliseconds, each word in the segment it was said in.
[[nodiscard]] Recognition::Result transcript(const QJsonObject &answer);

} // namespace Recognizers
