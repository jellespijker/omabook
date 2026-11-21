#pragma once

#include <QString>
#include <QStringList>
#include <QStringView>
#include <cstdint>

// Installing what reads aloud (#156), the way Omarchy's own apps install what
// they need: in a floating terminal of its own, which asks for the password
// there. A voice is speech-dispatcher with espeak-ng, from Arch's own
// repositories, through omarchy-install-app; a natural voice is Piper
// (piper-tts-bin, whose command is `piper`) and a voice of the book's
// language (piper-voices-<language>-<region>, put in /usr/share/piper-voices),
// which only the AUR has, through omarchy-pkg-aur-add. Both are where Voices
// looks for them. Offered only on Omarchy, where those commands are.
namespace VoiceInstall {

enum class Kind : std::uint8_t { Voice, Piper };

// A command line: the program, and its arguments as they are passed.
struct Command {
    QString program;
    QStringList arguments;
    friend bool operator==(const Command &, const Command &) = default;
};

// Whether this computer installs that way: Omarchy's commands for it are on the path.
[[nodiscard]] bool offered(Kind kind = Kind::Voice);
// Piper's voice for a book in `language` ("de", "pt-BR", "nld"): the AUR's
// package of it, of the language's usual region where the book names none,
// and English where there is none of the language.
[[nodiscard]] QString piperVoice(QStringView language);
// The packages that kind installs, for a book in `language`.
[[nodiscard]] QStringList packages(Kind kind, QStringView language);
// The command that installs them.
[[nodiscard]] Command command(Kind kind, QStringView language);

} // namespace VoiceInstall
