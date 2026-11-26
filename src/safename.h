#pragma once

#include <QString>
#include <QStringView>

namespace SafeName {

// A name that is safe as one path component on any filesystem a library
// might be synced to.
[[nodiscard]] inline QString component(QString text, const QString &fallback) {
    static constexpr QStringView unsafe = u"/\\:*?\"<>|";
    for (QChar &c : text)
        if (c.unicode() < 32 || unsafe.contains(c))
            c = ' ';
    text = text.simplified();
    while (text.startsWith('.'))
        text.remove(0, 1);
    text = text.left(80).trimmed();
    while (text.endsWith('.'))
        text.chop(1);
    return text.isEmpty() ? fallback : text;
}

} // namespace SafeName
