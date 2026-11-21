#pragma once

#include <QString>
#include <QtTypes>
#include <optional>

// Words that come from another person (#85, #124): what they wrote in a
// margin note, the name they go by in a space. They are shown, never obeyed:
// plain text, not markup, and without the characters that would change how
// the text around them is shown.
namespace UntrustedText {

// The longest name a person goes by in a space, in characters.
inline constexpr qsizetype maxNameLength = 64;

// Words as others wrote them, made plain: no control characters (but new
// lines and tabs), none that turn the text's direction around.
[[nodiscard]] QString plain(const QString &words);

// A name as someone chose it, made plain and put on one line, its spaces
// collapsed; none when nothing is left of it, or more than maxNameLength.
[[nodiscard]] std::optional<QString> name(const QString &chosen);

} // namespace UntrustedText
