#include "bookadmission.h"

#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <array>

#include "booksignature.h"
#include "document.h"
#include "formats.h"

using namespace Qt::StringLiterals;

namespace {

// What opening a book says when the file is not the book its name makes it.
constexpr std::array damaged{
    Failure::Code::EpubNotAnArchive,    Failure::Code::EpubWithoutPackage,
    Failure::Code::EpubWithoutChapters, Failure::Code::PdfUnreadable,
    Failure::Code::PdfWithoutPages,     Failure::Code::ComicWithoutPictures,
    Failure::Code::FictionBookEmpty,    Failure::Code::FictionBookWithoutText,
    Failure::Code::FileWithoutText,     Failure::Code::NoChaptersFound,
    Failure::Code::FileNotText,
};

// What it says when the book is there and locked.
constexpr std::array locked{Failure::Code::EpubProtected, Failure::Code::PdfLocked};

// A zip ends in a record of what is in it. One without was cut short, though
// what is left of it may still be listed and opened.
[[nodiscard]] bool cutShortZip(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.peek(4) != QByteArrayView("PK\x03\x04"))
        return false;
    // The record is 22 bytes, before a comment of 65535 at most.
    constexpr qint64 tail = 22 + 0xFFFF;
    return file.seek(std::max<qint64>(0, file.size() - tail)) && !file.readAll().contains(QByteArrayView("PK\x05\x06"));
}

[[nodiscard]] std::unexpected<Failure> refused(const QString &name, const Failure &why) {
    return std::unexpected(Failure(Failure::Code::BookRefused, {name, why.text()}));
}

[[nodiscard]] Failure unreadable(const QString &name, const Failure &why) {
    return Failure(Failure::Code::BookUnreadable, {name, why.text()});
}

// The verdict on a file Omabook reads itself, as opening it says.
[[nodiscard]] BookAdmission::Verdict opening(const QString &file, const QString &name) {
    const auto opened = Formats::opened(file);
    if (opened) {
        const std::optional<Failure> why = (*opened)->unreadable();
        return why ? std::optional(unreadable(name, *why)) : std::nullopt;
    }
    if (std::ranges::contains(damaged, opened.error().code()))
        return refused(name, opened.error());
    if (std::ranges::contains(locked, opened.error().code()))
        return unreadable(name, opened.error());
    // Anything else, such as a recording not written out yet, is no fault of the file.
    return std::nullopt;
}

// The verdict on a file that is read after converting it, as it looks (#264).
[[nodiscard]] BookAdmission::Verdict looking(const QString &file, const QString &name, const QString &extension) {
    const auto seen = BookSignature::look(file, extension);
    if (!seen)
        return refused(name, seen.error());
    return *seen ? std::optional(unreadable(name, **seen)) : std::nullopt;
}

} // namespace

BookAdmission::Verdict BookAdmission::examine(const Library &library, const QString &file, const QString &hash) {
    const QFileInfo info(file);
    if (!info.isFile() || !info.isReadable() || !Formats::isBook(file))
        return std::nullopt;
    // Before the library is asked: every empty file is the same file.
    if (info.size() == 0)
        return std::unexpected(Failure(Failure::Code::EmptyBookFile, {info.fileName()}));
    const QString extension = Formats::extension(file);
    if (!Formats::ownFormat(extension) || library.bookWithHash(hash) != 0)
        return std::nullopt;
    if (cutShortZip(file))
        return refused(info.fileName(), Failure(Failure::Code::FileCutShort));
    return Formats::readable(extension) ? opening(file, info.fileName()) : looking(file, info.fileName(), extension);
}

Library::Added BookAdmission::add(Library &library, const QString &file, const BookMeta &meta) {
    if (const auto verdict = examine(library, file, Library::hashFile(file)); !verdict)
        return {.error = verdict.error().text()};
    return library.add(file, meta);
}

BookAdmission::Verdict BookAdmission::addFormat(Library &library, qint64 id, const QString &file, Library::SameFormat same) {
    const Verdict verdict = examine(library, file, Library::hashFile(file));
    if (!verdict)
        return verdict;
    if (const auto added = library.addFormat(id, file, same); !added)
        return std::unexpected(added.error());
    return verdict;
}
