#include "speechinstall.h"

#include "airegistry.h"
#include "recognitionroute.h"
#include "settings.h"
#include "voiceinstall.h"
#include "voxtypelocal.h"

namespace {

// The ways the person's own recordings go, keeping to their own computers.
[[nodiscard]] std::expected<RecognitionRoute::Ways, QString> heard(const Settings &settings) {
    return AiRegistry::recognizers(settings, Recognition::Kind::Audio, false);
}

} // namespace

SpeechInstall::SpeechInstall(Settings *settings, QObject *parent) : QObject(parent), m_settings(settings) {
    connect(m_settings, &Settings::changed, this, &SpeechInstall::changed);
    connect(&m_terminal, &InstallTerminal::openChanged, this, &SpeechInstall::changed);
}

QString SpeechInstall::heardBy() const {
    const auto ways = heard(*m_settings);
    return ways && !ways->empty() ? ways->front()->where() : QString();
}

bool SpeechInstall::timed() const {
    const auto ways = heard(*m_settings);
    return ways && !ways->empty() && ways->front()->timesWords();
}

QString SpeechInstall::why() const {
    const auto ways = heard(*m_settings);
    return ways ? QString() : ways.error();
}

bool SpeechInstall::installable() const {
    return !VoxtypeLocal::missing(RecognitionRoute::voxtypeProgram(*m_settings)).isEmpty()
           && VoiceInstall::offered(VoiceInstall::Kind::Voxtype);
}

void SpeechInstall::install() {
    if (!installable())
        return;
    m_terminal.run(VoiceInstall::command(VoiceInstall::Kind::Voxtype, {}));
}
