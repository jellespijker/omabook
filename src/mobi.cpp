#include <QFile>
#include <QStringDecoder>
#include <QtEndian>
#include <concepts>
#include <optional>

#include "formatreaders.h"
#include "htmlclean.h"

namespace {

// A big-endian number at a place in some bytes; 0 where the bytes end before it does.
template <std::unsigned_integral Number>
[[nodiscard]] Number bigEndian(const QByteArray &data, qsizetype at) noexcept {
    const auto width = qsizetype(sizeof(Number));
    return at >= 0 && at + width <= data.size() ? qFromBigEndian<Number>(data.constData() + at) : Number{0};
}

[[nodiscard]] quint32 u32(const QByteArray &data, qsizetype at) noexcept { return bigEndian<quint32>(data, at); }
[[nodiscard]] quint16 u16(const QByteArray &data, qsizetype at) noexcept { return bigEndian<quint16>(data, at); }

} // namespace

// A Kindle file (MOBI, AZW, AZW3) is a Palm database whose first record holds
// a MOBI header and, after it, EXTH records with the book's metadata.
BookMeta readMobiMeta(const QString &path) {
    BookMeta meta;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return meta;
    // The record table, and after it the first record.
    const QByteArray head = file.read(78);
    const int records = u16(head, 76);
    if (head.mid(60, 8) != "BOOKMOBI" || records < 1)
        return meta;
    const QByteArray table = file.read(qint64(records) * 8);
    const auto record = [&file, &table, records](qint64 index) -> QByteArray {
        if (index < 0 || index >= records)
            return {};
        const qint64 start = u32(table, index * 8);
        const qint64 end = index + 1 < records ? u32(table, (index + 1) * 8) : file.size();
        if (end <= start || end - start > 64 * 1024 * 1024 || !file.seek(start))
            return {};
        return file.read(end - start);
    };

    const QByteArray first = record(0);
    if (first.mid(16, 4) != "MOBI")
        return meta;
    const quint32 headerLength = u32(first, 20);
    const bool utf8 = u32(first, 28) == 65001;
    const auto text = [utf8](const QByteArray &bytes) {
        if (utf8)
            return QString::fromUtf8(bytes);
        QStringDecoder western("windows-1252");
        return western.isValid() ? QString(western(bytes)) : QString::fromLatin1(bytes);
    };
    meta.title = text(first.mid(u32(first, 84), u32(first, 88))).simplified();
    const quint32 firstImage = u32(first, 108);

    qsizetype at = 16 + headerLength;
    if (!(u32(first, 128) & 0x40) || first.mid(at, 4) != "EXTH")
        return meta;
    const quint32 count = u32(first, at + 8);
    at += 12;
    std::optional<quint32> coverOffset;
    for (quint32 i = 0; i < count && at + 8 <= first.size(); ++i) {
        const quint32 type = u32(first, at), length = u32(first, at + 4);
        if (length < 8)
            break;
        const QByteArray value = first.mid(at + 8, length - 8);
        switch (type) {
        case 100: meta.authors << text(value).simplified(); break;
        case 101: meta.publisher = text(value).simplified(); break;
        case 103: meta.description = htmlToText(text(value)); break;
        case 104: meta.isbn = text(value).simplified(); break;
        case 105: meta.tags << BookMeta::splitTags(text(value)); break;
        case 106: meta.published = text(value).left(10); break;
        // A cover that isn't there is written as all ones.
        case 201: coverOffset = u32(value, 0) == 0xffffffff ? std::nullopt : std::optional(u32(value, 0)); break;
        case 503: meta.title = text(value).simplified(); break;
        case 524: meta.language = text(value).simplified(); break;
        }
        at += length;
    }
    if (coverOffset)
        meta.cover = QImage::fromData(record(qint64(firstImage) + *coverOffset));
    return meta;
}
