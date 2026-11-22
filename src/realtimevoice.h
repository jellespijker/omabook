#pragma once

#include <QJsonObject>
#include <QTemporaryDir>
#include <QUrl>
#include <QWebSocket>
#include <memory>

#include "aiendpoint.h"
#include "localvoices.h"
#include "voice.h"

// OpenAI's voices through its Realtime API (#169), for the models that speak
// only there: the successors of /audio/speech's, which OpenAI shuts down on
// 2027-01-06. The shapes are those of OpenAI's Realtime guide and reference,
// checked on 2026-11-22:
//   - a WebSocket to wss://api.openai.com/v1/realtime?model=<model>, with the
//     key in "Authorization: Bearer"; the server opens with session.created;
//   - session.update {session: {type: "realtime", instructions,
//     output_modalities: ["audio"], audio: {output: {format: {type:
//     "audio/pcm", rate: 24000}, voice, speed}}}}: the voice can't change once
//     it has spoken, and the speed is 0.25 to 1.5, so a sentence in another
//     voice or pace opens a session of its own;
//   - a sentence is response.create {response: {conversation: "none", input:
//     [a user message of input_text]}}, out of band, so the session keeps no
//     conversation and each sentence is read on its own;
//   - its sound comes as response.output_audio.delta (base64 of mono 16-bit
//     PCM at 24 kHz), and it ends in response.done {response: {status,
//     status_details}}; what went wrong is an error {error: {type, code,
//     message}}.
// The model is a conversational one, told to read the text as it is written.
// Like ServerVoice for OpenAI: only where hosted is allowed for text to
// speech, never with the AI off (Voices::choose says which may be asked).
class RealtimeVoice : public Voice {
    Q_OBJECT

public:
    // OpenAI's server (its name and key), where its Realtime API is, the
    // model, and what plays the sound (the desktop's speakers unless said).
    RealtimeVoice(AiServer server, QUrl address, QString model, std::unique_ptr<SoundOut> sound = {}, QObject *parent = nullptr);
    ~RealtimeVoice() override;
    RealtimeVoice(const RealtimeVoice &) = delete;
    RealtimeVoice &operator=(const RealtimeVoice &) = delete;
    RealtimeVoice(RealtimeVoice &&) = delete;
    RealtimeVoice &operator=(RealtimeVoice &&) = delete;

    // The successor OpenAI's deprecations page names for every /audio/speech
    // model (checked on 2026-11-22).
    static constexpr auto openAiModel = "gpt-realtime-2.1-mini";
    static constexpr int rate = 24000;
    // The voices its reference lists, and the one taken for any other.
    [[nodiscard]] static QStringList voices();
    static constexpr auto defaultVoice = "marin";
    // Whether a model of OpenAI's speaks only through the Realtime API.
    [[nodiscard]] static bool realtimeOnly(const QString &model);
    // Where its Realtime API is, for the API at `base` (".../v1"): the same host over a WebSocket.
    [[nodiscard]] static QUrl addressOf(const QString &base);
    // What the session is told, and what is asked for one sentence.
    [[nodiscard]] static QJsonObject session(const Speech &how);
    [[nodiscard]] static QJsonObject response(const QString &sentence);

    void say(const QString &sentence, const Speech &how) override;
    void stop() override;
    [[nodiscard]] QString name() const override { return m_server.name; }

private:
    void open();
    void ask();
    void received(const QString &message);
    void done(const QJsonObject &response);
    void refused(const QJsonObject &error);
    void fail(const Failure &why);
    void play();

    AiServer m_server;
    QUrl m_address;
    QString m_model;
    QWebSocket m_socket;
    // What the open session reads with, and whether it said it is there.
    Speech m_session;
    bool m_ready = false;
    // The sentence being read until its sound plays or it fails, and the sound so far.
    QString m_sentence;
    bool m_asking = false;
    QByteArray m_pcm;
    QTemporaryDir m_folder;
    int m_files = 0;
    std::unique_ptr<SoundOut> m_sound;
};
