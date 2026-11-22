#include "voicepreview.h"

#include <utility>

#include "airegistry.h"
#include "settings.h"
#include "voices.h"

VoicePreview::VoicePreview(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings) {}

VoicePreview::~VoicePreview() {
    if (m_voice)
        m_voice->stop();
}

void VoicePreview::play(const QString &text, const QString &language) {
    stop();
    auto voice = m_maker ? m_maker() : AiRegistry::voice(*m_settings);
    if (!voice) {
        ended(voice.error().text());
        return;
    }
    m_voice = *std::move(voice);
    connect(m_voice.get(), &Voice::spoken, this, [this] { ended({}); });
    connect(m_voice.get(), &Voice::failed, this, [this](const Failure &why) { ended(why.text()); });
    m_why.clear();
    emit changed();
    m_voice->say(text, Voices::speech(*m_settings, language));
}

void VoicePreview::stop() {
    if (!m_voice)
        return;
    m_voice->stop();
    ended({});
}

void VoicePreview::ended(const QString &why) {
    if (m_voice) {
        // Its signal may still be on the way out of it.
        m_voice->disconnect(this);
        m_voice.release()->deleteLater();
    }
    m_why = why;
    emit changed();
}
