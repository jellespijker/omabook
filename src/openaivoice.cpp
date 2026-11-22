#include "openaivoice.h"

#include <utility>

#include "realtimevoice.h"
#include "servervoice.h"

OpenAiVoice::OpenAiVoice(AiServer server, QUrl realtime, const QString &model, QObject *parent)
    : Voice(parent), m_server(std::move(server)), m_address(std::move(realtime)) {
    if (RealtimeVoice::realtimeOnly(model))
        use(new RealtimeVoice(m_server, m_address, model, {}, this), true);
    else
        use(new ServerVoice(m_server, model, QString::fromLatin1(ServerVoice::openAiModel), this), false);
}

void OpenAiVoice::use(Voice *voice, bool realtime) {
    if (m_voice != nullptr) {
        m_voice->disconnect(this);
        m_voice->stop();
        // It may be the one whose signal is being answered, so it goes once that is over.
        m_voice->deleteLater();
    }
    m_voice = voice;
    m_realtime = realtime;
    connect(m_voice, &Voice::spoken, this, &Voice::spoken);
    connect(m_voice, &Voice::modelRetired, this, &Voice::modelRetired);
    connect(m_voice, &Voice::failed, this, &OpenAiVoice::failedWith);
}

void OpenAiVoice::failedWith(const Failure &why) {
    if (m_realtime || why.code() != Failure::Code::VoiceModelRetired) {
        emit failed(why);
        return;
    }
    // /audio/speech has no model left to read with: its successor reads the same sentence.
    const QStringList &said = why.arguments();
    emit modelRetired(said.size() > 1 ? said.at(1) : QString());
    use(new RealtimeVoice(m_server, m_address, QString::fromLatin1(RealtimeVoice::openAiModel), {}, this), true);
    m_voice->say(m_sentence, m_how);
}

void OpenAiVoice::say(const QString &sentence, const Speech &how) {
    m_sentence = sentence;
    m_how = how;
    m_voice->say(sentence, how);
}

void OpenAiVoice::stop() { m_voice->stop(); }
