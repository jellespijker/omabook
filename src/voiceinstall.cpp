#include "voiceinstall.h"

#include <QLocale>
#include <QStandardPaths>
#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace {

constexpr auto installApp = "omarchy-install-app";
constexpr auto floatingTerminal = "omarchy-launch-floating-terminal-with-presentation";
constexpr auto aurAdd = "omarchy-pkg-aur-add";

// The AUR's voices for Piper, by language and region. A language's first is
// the one taken where a book names no region the AUR has.
constexpr std::array piperVoices{
    "ar-jo", "bg-bg", "ca-es", "cs-cz", "cy-gb", "da-dk", "de-de", "el-gr", "en-us", "en-gb", "es-es", "es-mx", "es-ar", "fa-ir", "fi-fi",
    "fr-fr", "hi-in", "hu-hu", "id-id", "is-is", "it-it", "ka-ge", "kk-kz", "lb-lu", "lv-lv", "ml-in", "ne-np", "nl-nl", "nl-be", "no-no",
    "pl-pl", "pt-br", "pt-pt", "ro-ro", "ru-ru", "sk-sk", "sl-si", "sr-rs", "sv-se", "sw-cd", "te-in", "tr-tr", "uk-ua", "vi-vn", "zh-cn",
};

[[nodiscard]] bool onPath(const char *program) { return !QStandardPaths::findExecutable(QString::fromLatin1(program)).isEmpty(); }

[[nodiscard]] bool hasVoice(const QString &code) {
    return std::ranges::any_of(piperVoices, [&](const char *voice) { return code == QLatin1StringView(voice); });
}

} // namespace

bool VoiceInstall::offered(Kind kind) {
    if (kind == Kind::Voice)
        return onPath(installApp);
    return onPath(installApp) && onPath(floatingTerminal) && onPath(aurAdd);
}

QString VoiceInstall::piperVoice(QStringView language) {
    // QLocale reads "nl", "nl-BE", "nl_BE" and "nld" alike, and fills in a
    // language's usual region ("nl" is the Netherlands').
    const QLocale locale(language.trimmed().isEmpty() ? u"en"_s : language.trimmed().toString());
    QString spoken = locale.language() == QLocale::C ? u"en"_s : QLocale::languageToCode(locale.language(), QLocale::ISO639Part1).toLower();
    // Piper's Norwegian is Bokmål, under the language's older name.
    if (spoken == "nb"_L1 || spoken == "nn"_L1)
        spoken = u"no"_s;
    const QString prefix = spoken + u'-';
    if (const QString wanted = prefix + QLocale::territoryToCode(locale.territory()).toLower(); hasVoice(wanted))
        return u"piper-voices-"_s + wanted;
    const auto *const ofLanguage =
        std::ranges::find_if(piperVoices, [&](const char *voice) { return QLatin1StringView(voice).startsWith(prefix); });
    return u"piper-voices-"_s + (ofLanguage != piperVoices.end() ? QString::fromLatin1(*ofLanguage) : u"en-us"_s);
}

QStringList VoiceInstall::packages(Kind kind, QStringView language) {
    if (kind == Kind::Voice)
        return {u"speech-dispatcher"_s, u"espeak-ng"_s};
    return {u"piper-tts-bin"_s, piperVoice(language)};
}

VoiceInstall::Command VoiceInstall::command(Kind kind, QStringView language) {
    const QStringList installed = packages(kind, language);
    if (kind == Kind::Voice)
        return {.program = QString::fromLatin1(installApp), .arguments = {u"speech-dispatcher"_s, installed.join(u' ')}};
    // What omarchy-install-app does for Arch's packages, with the AUR's
    // command; the packages' names are this file's, so need no quoting.
    return {.program = QString::fromLatin1(floatingTerminal),
            .arguments = {u"echo 'Installing Piper...'; "_s + QString::fromLatin1(aurAdd) + u' ' + installed.join(u' ')}};
}
