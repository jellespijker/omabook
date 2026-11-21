#include "untrustedtext.h"

QString UntrustedText::plain(const QString &words) {
    QString kept;
    kept.reserve(words.size());
    for (const QChar c : words) {
        const char16_t unit = c.unicode();
        const bool turns =
            (unit >= 0x202A && unit <= 0x202E) || (unit >= 0x2066 && unit <= 0x2069) || unit == 0x200E || unit == 0x200F || unit == 0x061C;
        const bool control = c.category() == QChar::Other_Control && unit != u'\n' && unit != u'\t';
        if (!turns && !control)
            kept += c;
    }
    return kept;
}

std::optional<QString> UntrustedText::name(const QString &chosen) {
    // A new line or a tab is a space in a name: it is shown on one line.
    QString one = plain(chosen).simplified();
    if (one.isEmpty() || one.size() > maxNameLength)
        return std::nullopt;
    return one;
}
