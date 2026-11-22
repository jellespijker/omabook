#include "spokenanswer.h"

#include <QTextDocument>
#include <utility>

#include "airegistry.h"
#include "audiodevices.h"
#include "narration.h"
#include "settings.h"
#include "voices.h"

SpokenAnswer::SpokenAnswer(Settings *settings, AudioDevices *devices, QObject *parent)
    : QObject(parent), m_settings(settings), m_devices(devices) {
    // The window's stop (the bar's, Shift+R) stops an answer as it stops a plugin reading.
    if (m_devices)
        connect(m_devices, &AudioDevices::stopSpeakingAsked, this, &SpokenAnswer::stop);
}

SpokenAnswer::~SpokenAnswer() {
    if (m_narration && m_devices)
        m_devices->endedSpeaking(this);
}

QString SpokenAnswer::plain(const QString &markdown) {
    QTextDocument document;
    document.setMarkdown(markdown);
    return document.toPlainText().trimmed();
}

void SpokenAnswer::say(const QString &answer, const QString &who, const QString &language) {
    stop();
    QString words = plain(answer);
    if (words.isEmpty())
        return;
    auto voice = m_maker ? m_maker() : AiRegistry::voice(*m_settings);
    if (!voice) {
        emit failed(voice.error().text());
        return;
    }
    m_narration = std::make_unique<Narration>(1, [words = std::move(words)](int) { return words; }, *std::move(voice));
    if (!m_narration->from(0, 0)) {
        m_narration.reset();
        return;
    }
    connect(m_narration.get(), &Narration::finished, this, &SpokenAnswer::ended);
    connect(m_narration.get(), &Narration::failed, this, [this](const Failure &why) {
        ended();
        emit failed(why.text());
    });
    m_narration->setSpeech(Voices::speech(*m_settings, language));
    if (m_devices)
        m_devices->beganSpeaking(this, who);
    emit changed();
    m_narration->play();
}

void SpokenAnswer::stop() {
    if (m_narration)
        m_narration->pause();
    ended();
}

void SpokenAnswer::ended() {
    if (!m_narration)
        return;
    // Its signal may still be on the way out of it: let it go once that is done.
    m_narration->disconnect(this);
    m_narration.release()->deleteLater();
    if (m_devices)
        m_devices->endedSpeaking(this);
    emit changed();
}
