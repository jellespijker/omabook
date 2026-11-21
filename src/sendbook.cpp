#include "sendbook.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

#include "bookops.h"
#include "formats.h"
#include "library.h"
#include "opdsfeed.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool has(const QDir &folder, const QString &path) { return QFileInfo::exists(folder.filePath(path)); }

// The book's file in the format chosen, converted first when it must be.
[[nodiscard]] std::expected<std::pair<Book, QString>, Failure> fileToSend(Library &library, qint64 id, const QStringList &wanted,
                                                                          const SendBook::Convert &convert) {
    const auto book = library.book(id);
    if (!book)
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(id)}));
    const auto choice = SendBook::choose(book->formats, wanted);
    if (!choice)
        return std::unexpected(Failure(Failure::Code::NothingToSend, {book->meta.title}));
    if (!choice->convert)
        return std::pair{*book, library.formatPath(id, choice->format)};
    const auto converted = convert(library, id, choice->format);
    if (!converted)
        return std::unexpected(converted.error());
    // Read again, with the format it now has.
    const auto again = library.book(id);
    if (!again)
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(id)}));
    return std::pair{*again, *converted};
}

} // namespace

SendBook::Device SendBook::deviceAt(const QString &folder) {
    const QDir device(folder);
    if (has(device, u".kobo"_s))
        return Device::Kobo;
    if ((has(device, u"documents"_s) || has(device, u"Documents"_s)) && (has(device, u"system"_s) || has(device, u"System"_s)))
        return Device::Kindle;
    if (has(device, u"system/config"_s) || has(device, u"system/profiles"_s))
        return Device::PocketBook;
    return Device::Other;
}

QString SendBook::deviceName(Device device) {
    switch (device) {
    case Device::Kindle:
        return u"Kindle"_s;
    case Device::Kobo:
        return u"Kobo"_s;
    case Device::PocketBook:
        return u"PocketBook"_s;
    case Device::Other:
        break;
    }
    return {};
}

QStringList SendBook::formatsFor(Device device) {
    switch (device) {
    // Over USB a Kindle reads its own formats, never EPUB.
    case Device::Kindle:
        return {"azw3", "mobi", "pdf", "txt"};
    case Device::Kobo:
        return {"epub", "pdf", "cbz", "cbr", "mobi", "txt"};
    case Device::PocketBook:
        return {"epub", "fb2", "pdf", "mobi", "djvu", "cbz", "cbr", "txt"};
    case Device::Other:
        break;
    }
    return {"epub", "pdf", "azw3", "mobi", "fb2", "cbz", "cbr", "djvu", "txt"};
}

QStringList SendBook::mailFormats() { return {"epub", "pdf", "docx", "rtf", "txt"}; }

QString SendBook::booksFolder(const QString &folder, Device device) {
    if (device != Device::Kindle)
        return folder;
    const QDir root(folder);
    return root.filePath(has(root, u"Documents"_s) && !has(root, u"documents"_s) ? u"Documents"_s : u"documents"_s);
}

std::optional<SendBook::Choice> SendBook::choose(const QStringList &bookFormats, const QStringList &wanted) {
    if (bookFormats.isEmpty() || wanted.isEmpty())
        return std::nullopt;
    const auto found = std::ranges::find_if(wanted, [&bookFormats](const QString &format) { return bookFormats.contains(format); });
    if (found != wanted.end())
        return Choice{.format = *found, .convert = false};
    return Choice{.format = wanted.first(), .convert = true};
}

QString SendBook::fileName(const Book &book, const QString &format) {
    QString name = book.meta.authors.isEmpty() ? book.meta.title : book.meta.title + u" - "_s + book.meta.authors.first();
    static const QString forbidden = u"/\\:*?\"<>|"_s;
    for (QChar &c : name) {
        if (forbidden.contains(c) || c.unicode() < 32)
            c = '_';
    }
    const QString trimmed = name.left(120).trimmed();
    return (trimmed.isEmpty() ? u"Book"_s : trimmed) + '.' + format;
}

std::expected<QString, Failure> SendBook::calibre(Library &library, qint64 book, const QString &format) {
    QString error;
    QString file = BookOps::convert(library, book, format, {}, &error);
    if (file.isEmpty())
        return std::unexpected(Failure::said(error));
    return file;
}

std::expected<QString, Failure> SendBook::toFolder(Library &library, qint64 id, const QString &folder, const Convert &convert) {
    if (folder.isEmpty() || !QFileInfo(folder).isDir())
        return std::unexpected(Failure(Failure::Code::NoDeviceFolder, {folder}));
    const Device device = deviceAt(folder);
    const auto file = fileToSend(library, id, formatsFor(device), convert);
    if (!file)
        return std::unexpected(file.error());
    const QString into = booksFolder(folder, device);
    if (!QDir().mkpath(into))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {into}));
    QString target = QDir(into).filePath(fileName(file->first, Formats::extension(file->second)));
    // A copy sent before is replaced, as the newer one may have other details.
    QFile source(file->second);
    if ((QFileInfo::exists(target) && !QFile::remove(target)) || !source.copy(target))
        return std::unexpected(Failure(Failure::Code::NotCopied, {target, source.errorString()}));
    return target;
}

std::expected<void, Failure> SendBook::byMail(Library &library, qint64 id, const Post &post, const Convert &convert) {
    if (post.to.isEmpty() || post.server.host.isEmpty() || post.from.isEmpty())
        return std::unexpected(Failure(Failure::Code::MailNotSetUp));
    const auto file = fileToSend(library, id, mailFormats(), convert);
    if (!file)
        return std::unexpected(file.error());
    const QString format = Formats::extension(file->second);
    return SmtpMail::send(post.server, {.from = post.from,
                                        .to = post.to,
                                        .subject = file->first.meta.title,
                                        .text = u"Sent from Omabook."_s,
                                        .attachment = file->second,
                                        .attachmentName = fileName(file->first, format),
                                        .attachmentType = OpdsFeed::mimeType(format)});
}
