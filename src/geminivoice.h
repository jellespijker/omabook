#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QTemporaryDir>

#include "gemini.h"
#include "localvoices.h"
#include "voice.h"

// Gemini reading aloud with one of its voices, through its own API (#145):
// each sentence is sent to a speech model (generateContent with audio as the
// answer), and the sound it answers is played. Always someone else's
// computer, so only where the person allowed hosted for text to speech, and
// never with the AI off (Voices::choose says which may be asked). Gemini's
// voices take no pace, so the rate is the voice's own.
class GeminiVoice : public Voice {
    Q_OBJECT

public:
    GeminiVoice(Gemini::Api api, QString model, QObject *parent = nullptr);
    ~GeminiVoice() override;
    GeminiVoice(const GeminiVoice &) = delete;
    GeminiVoice &operator=(const GeminiVoice &) = delete;
    GeminiVoice(GeminiVoice &&) = delete;
    GeminiVoice &operator=(GeminiVoice &&) = delete;

    void say(const QString &sentence, const Speech &how) override;
    void stop() override;
    [[nodiscard]] QString name() const override;

private:
    void heard(QNetworkReply *reply);

    Gemini::Api m_api;
    QString m_model;
    QNetworkAccessManager m_network;
    QPointer<QNetworkReply> m_reply;
    QTemporaryDir m_folder;
    int m_files = 0;
    SoundOut m_sound;
};
