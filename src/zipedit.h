#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>
#include <utility>

#include "failure.h"

// Changing a few files of a zip and nothing else (#74). Every other file's
// record is copied as it lies, its compressed bytes, times, extra fields and
// data descriptor byte for byte, and the central directory keeps what it said
// of each but where the record now begins. What EPUB asks of a zip's first
// file, "mimetype", stored, without an extra field and saying
// "application/epub+zip", holds for what is written whether or not it held for
// what was read (with `epub`).
namespace ZipEdit {

// A file of a zip as its central directory says it.
struct Entry {
    QString name;
    quint16 method = 0; // 0 stored, 8 deflated
    quint16 flags = 0;  // the general purpose bits
    quint32 crc = 0;
    qint64 offset = 0;      // where its local record begins
    qint64 length = 0;      // up to where the next begins: its data descriptor is in it
    quint16 localExtra = 0; // the length of its local header's extra field
};

// The files of a zip in the order their records lie. A failure for what this
// can't change in place: a zip that spans disks, a zip64, a damaged one.
[[nodiscard]] std::expected<QList<Entry>, Failure> entries(QByteArrayView zip);

struct Change {
    QHash<QString, QByteArray> replaced;         // by name: the file's new contents
    QList<std::pair<QString, QByteArray>> added; // new files, after the others
    bool epub = false;                           // the mimetype comes first, as EPUB says
};
// The bytes of `zip` with those changes made: a whole new zip.
[[nodiscard]] std::expected<QByteArray, Failure> edited(QByteArrayView zip, const Change &change);

// The CRC-32 a zip keeps of a file's contents.
[[nodiscard]] quint32 crc32(QByteArrayView data) noexcept;

} // namespace ZipEdit
