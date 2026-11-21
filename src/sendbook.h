#pragma once

#include <QString>
#include <QStringList>
#include <QtTypes>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>

#include "failure.h"
#include "smtpmail.h"

class Library;
struct Book;

// Sending a book to an e-reader (#71): copied onto one mounted over USB, in
// a format it reads, or mailed to a Kindle's Send-to-Kindle address. A book
// with no such format is converted with Calibre first, and the new format is
// kept with the book, so KOReader on the device and the library know the
// same file (KoreaderBooks). Every function waits, so none is for the
// window's thread.
namespace SendBook {

enum class Device : std::uint8_t { Kindle, Kobo, PocketBook, Other };

// What kind of e-reader is mounted at a folder, by what it keeps there.
[[nodiscard]] Device deviceAt(const QString &folder);
[[nodiscard]] QString deviceName(Device device);
// The formats a device reads, the best first.
[[nodiscard]] QStringList formatsFor(Device device);
// Those Send-to-Kindle takes by mail.
[[nodiscard]] QStringList mailFormats();
// Where on the device a book goes: a Kindle's documents folder, else the top.
[[nodiscard]] QString booksFolder(const QString &folder, Device device);

// Which of a book's formats to send, of those wanted: the first it has;
// else the first wanted, converted to (`convert`). None for a book with no file.
struct Choice {
    QString format;
    bool convert = false;
};
[[nodiscard]] std::optional<Choice> choose(const QStringList &bookFormats, const QStringList &wanted);

// The name a book's file goes by on a device: "Title - Author.epub", without
// what a file system won't take.
[[nodiscard]] QString fileName(const Book &book, const QString &format);

// Converts a book to a format and keeps it with the book: its file.
using Convert = std::function<std::expected<QString, Failure>(Library &, qint64 book, const QString &format)>;
// Calibre's converter, as `omabook convert` runs it.
[[nodiscard]] std::expected<QString, Failure> calibre(Library &library, qint64 book, const QString &format);

// Copies the book onto the e-reader mounted at `folder`; the file it wrote.
[[nodiscard]] std::expected<QString, Failure> toFolder(Library &library, qint64 id, const QString &folder,
                                                       const Convert &convert = calibre);

// Mails the book to `to` from `from`, through `server`.
struct Post {
    SmtpMail::Server server;
    QString from;
    QString to;
};
[[nodiscard]] std::expected<void, Failure> byMail(Library &library, qint64 id, const Post &post, const Convert &convert = calibre);

} // namespace SendBook
