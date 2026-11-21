#pragma once

#include <QNetworkAccessManager>
#include <QPointer>
#include <QString>

#include "gemini.h"
#include "recognition.h"

class QNetworkReply;

// Handwriting and speech recognised by Gemini through its own API (#145):
// the ink is shown to a model that reads pictures (generateContent with the
// picture inline), and a recording is heard by Gemini's transcription model,
// each word with its moment, or by any model that hears, asked for a
// transcript in words. Whether Gemini may be sent them is decided before one
// of these is made (RecognitionRoute): it is always someone else's computer.
class GeminiVision : public Recognition::Recognizer {
    Q_OBJECT

public:
    GeminiVision(Gemini::Api api, QString model, QObject *parent = nullptr);
    ~GeminiVision() override;
    GeminiVision(const GeminiVision &) = delete;
    GeminiVision &operator=(const GeminiVision &) = delete;
    GeminiVision(GeminiVision &&) = delete;
    GeminiVision &operator=(GeminiVision &&) = delete;

    [[nodiscard]] QString where() const override;
    [[nodiscard]] QString model() const override { return m_model; }
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void read(QNetworkReply *reply);

    Gemini::Api m_api;
    QString m_model;
    QString m_language;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};

class GeminiTranscription : public Recognition::Recognizer {
    Q_OBJECT

public:
    GeminiTranscription(Gemini::Api api, QString model, QObject *parent = nullptr);
    ~GeminiTranscription() override;
    GeminiTranscription(const GeminiTranscription &) = delete;
    GeminiTranscription &operator=(const GeminiTranscription &) = delete;
    GeminiTranscription(GeminiTranscription &&) = delete;
    GeminiTranscription &operator=(GeminiTranscription &&) = delete;

    [[nodiscard]] QString where() const override;
    [[nodiscard]] QString model() const override { return m_model; }
    // Its transcription model gives them; another model only the words.
    [[nodiscard]] bool timesWords() const override;
    void recognize(const Recognition::Input &input) override;
    void cancel() override;

private:
    void read(QNetworkReply *reply);

    Gemini::Api m_api;
    QString m_model;
    QString m_language;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
};
