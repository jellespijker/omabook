#include "booksignature.h"

#include <QByteArrayView>
#include <QFile>
#include <algorithm>
#include <array>
#include <cctype>

using namespace Qt::StringLiterals;

namespace {

constexpr std::array kindle{"mobi"_L1, "azw"_L1, "azw3"_L1, "prc"_L1};

// A Palm database says its kind at byte 60: a Kindle's book, or an older one.
// A Kindle file may also be Amazon's Topaz or KFX under the same names, and a
// .prc any Palm database, which says its kind in eight letters.
[[nodiscard]] bool kindleBook(const QByteArray &head, bool anyPalm) {
    const QByteArray kind = head.mid(60, 8);
    if (kind == "BOOKMOBI" || kind == "TEXtREAd")
        return true;
    // KFX with its lock begins with a byte that is no letter.
    constexpr QByteArrayView lockedKfx("\xea"
                                       "DRMION");
    if (head.startsWith("TPZ") || head.startsWith("CONT") || head.startsWith(lockedKfx))
        return true;
    return anyPalm && kind.size() == 8
           && std::ranges::all_of(kind, [](char letter) { return std::isprint(static_cast<unsigned char>(letter)) != 0; });
}

// Whether the group the document begins with is closed again: a file cut short
// ends inside it. A brace after a backslash is a letter, and the bytes a \bin
// word announces are not looked at.
[[nodiscard]] bool closes(QByteArrayView rtf) {
    int depth = 0;
    for (qsizetype at = 0; at < rtf.size(); ++at) {
        if (rtf[at] == '{') {
            ++depth;
        } else if (rtf[at] == '}') {
            if (--depth == 0)
                return true;
        } else if (rtf[at] == '\\') {
            if (!rtf.sliced(at + 1).startsWith("bin")) {
                ++at;
                continue;
            }
            qsizetype end = at + 4;
            while (end < rtf.size() && std::isdigit(static_cast<unsigned char>(rtf[end])) != 0)
                ++end;
            const qsizetype bytes = rtf.sliced(at + 4, std::max<qsizetype>(0, end - at - 4)).toLongLong();
            at = end > at + 4 ? end + std::clamp<qsizetype>(bytes, 0, rtf.size()) : at + 3;
        }
    }
    return false;
}

[[nodiscard]] BookSignature::Verdict richText(QFile &file) {
    QByteArray rtf = file.readAll();
    if (rtf.startsWith("\xef\xbb\xbf"))
        rtf.remove(0, 3);
    rtf = rtf.trimmed();
    if (!rtf.startsWith("{\\rtf"))
        return std::unexpected(Failure(Failure::Code::FileNotItsFormat));
    if (!closes(rtf))
        return std::unexpected(Failure(Failure::Code::FileCutShort));
    return std::nullopt;
}

} // namespace

BookSignature::Verdict BookSignature::look(const QString &path, const QString &extension) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    if (extension == "rtf"_L1)
        return richText(file);
    const QByteArray head = file.read(68);
    bool fits = true;
    if (std::ranges::contains(kindle, extension)) {
        fits = kindleBook(head, extension == "prc"_L1);
    } else if (extension == "docx"_L1) {
        // A document with a password is an older kind of file, not a zip.
        if (head.startsWith("\xd0\xcf\x11\xe0"))
            return Failure(Failure::Code::DocumentLocked);
        fits = head.startsWith("PK\x03\x04");
    } else if (extension == "odt"_L1) {
        fits = head.startsWith("PK\x03\x04");
    } else if (extension == "djvu"_L1) {
        fits = head.startsWith("AT&T") || head.startsWith("FORM") || head.startsWith("SDJV");
    }
    if (!fits)
        return std::unexpected(Failure(Failure::Code::FileNotItsFormat));
    return std::nullopt;
}
