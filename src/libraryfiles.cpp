#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QLockFile>
#include <QScopeGuard>
#include <QSqlQuery>
#include <QTimeZone>
#include <QVariant>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <system_error>

#include "epubdetails.h"
#include "formats.h"
#include "hlc.h"
#include "library.h"
#include "spaces.h"

// A book's files and its cover as the library keeps them, and its details
// written into its EPUB (#74). The file one added is never touched: the
// library's copy is what changes, and the copy as it came in is kept beside
// it, in the book's folder, until it is put back.

using namespace Qt::StringLiterals;

namespace {

constexpr auto epub = "epub"_L1;

// Where the file as it came in is kept: in a hidden folder beside it, so it
// goes wherever the book's folder goes.
[[nodiscard]] QString keptOriginal(const QString &file) { return QFileInfo(file).absolutePath() + u"/.original/original.epub"_s; }

// The new file beside the one it replaces, under a name no other write uses.
[[nodiscard]] QString partialFor(const QString &file) {
    static std::atomic<int> writes = 0;
    return u"%1.details-%2-%3.part"_s.arg(file).arg(QCoreApplication::applicationPid()).arg(++writes);
}

// What a write cut short left behind: its new file, never put in place. One
// still being written by another program is younger than this.
void clearPartials(const QString &file) {
    constexpr auto abandoned = std::chrono::minutes(10);
    const QFileInfo info(file);
    const QDateTime before = QDateTime::currentDateTime().addSecs(-std::chrono::seconds(abandoned).count());
    for (const QFileInfo &left : QDir(info.absolutePath()).entryInfoList({info.fileName() + u".details-*.part"_s}, QDir::Files))
        if (left.lastModified() < before)
            QFile::remove(left.filePath());
}

[[nodiscard]] std::unexpected<Failure> unwritable(const QString &path, const QString &why) {
    return std::unexpected(Failure(Failure::Code::BookFileUnwritable, {path, why}));
}

// `target` replaced by `source` in one step: whoever opens it finds the one
// or the other, whole, whenever the computer stops.
[[nodiscard]] std::expected<void, Failure> replaceWith(const QString &source, const QString &target) {
    std::error_code error;
    std::filesystem::rename(QFile::encodeName(source).constData(), QFile::encodeName(target).constData(), error);
    if (error)
        return unwritable(target, QString::fromStdString(error.message()));
    return {};
}

[[nodiscard]] std::expected<void, Failure> writeWhole(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
        return unwritable(path, file.errorString());
    return {};
}

// A copy of the file as it is, kept before details are first written into it.
[[nodiscard]] std::expected<void, Failure> keepOriginal(const FormatFile &file) {
    const QString kept = keptOriginal(file.path);
    const QString partial = kept + u".part"_s;
    QFile::remove(partial);
    if (!QDir().mkpath(QFileInfo(kept).absolutePath()) || !QFile::copy(file.path, partial) || Library::hashFile(partial) != file.hash) {
        QFile::remove(partial);
        return unwritable(kept, {});
    }
    return replaceWith(partial, kept);
}

// What is written into the book's EPUB: its details, and the library's
// cover unless it is the file's own as the library keeps it.
[[nodiscard]] OpfDetails::Details detailsFor(const Book &book, const QString &coverFile, const QString &epubFile) {
    QFile cover(coverFile);
    const QByteArray ours = !coverFile.isEmpty() && cover.open(QIODevice::ReadOnly) ? cover.readAll() : QByteArray();
    const QImage theirs = ours.isEmpty() ? QImage() : Formats::readMeta(epubFile).cover;
    return {.meta = book.meta,
            .identifiers = book.identifiers,
            .modified = QDateTime::fromSecsSinceEpoch(book.detailsAt > 0 ? book.detailsAt : book.added, QTimeZone::UTC),
            .cover = !theirs.isNull() && Library::coverJpeg(theirs) == ours ? QByteArray() : ours};
}

// The EPUB at `file` with those details, written whole at `partial` and read back.
[[nodiscard]] std::expected<void, Failure> writtenBeside(const QString &file, const QString &partial, const OpfDetails::Details &details) {
    const auto bytes = EpubDetails::written(file, details);
    if (!bytes)
        return std::unexpected(bytes.error());
    if (const auto put = writeWhole(partial, *bytes); !put)
        return put;
    return EpubDetails::check(partial, details);
}

// The book's file to change by this program alone, for as long as the lock
// lives: another write of it, from the window or a command, waits, so the
// file kept as it came in is never one already written into.
[[nodiscard]] std::expected<std::unique_ptr<QLockFile>, Failure> heldAlone(const QString &file) {
    if (file.isEmpty())
        return nullptr; // nothing to hold; epubFile says why
    auto lock = std::make_unique<QLockFile>(QFileInfo(file).absolutePath() + u"/.details.lock"_s);
    if (!lock->tryLock(std::chrono::minutes(1)))
        return std::unexpected(Failure(Failure::Code::LibraryBusy));
    return lock;
}

} // namespace

QString Library::hashFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
}

QByteArray Library::coverJpeg(const QImage &cover) {
    // Large enough for a sharp tile on a 4K screen, small enough to load a
    // shelf of them at once.
    const QImage scaled = cover.height() > 900 ? cover.scaledToHeight(900, Qt::SmoothTransformation) : cover;
    QByteArray bytes;
    QBuffer buffer(&bytes);
    return buffer.open(QIODevice::WriteOnly) && scaled.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPG", 88) ? bytes
                                                                                                                      : QByteArray();
}

bool Library::setCover(qint64 id, const QImage &cover, std::optional<qint64> at) {
    const QString folder = bookFolder(id);
    if (folder.isEmpty() || cover.isNull() || !QDir().mkpath(folder))
        return false;
    const QByteArray jpeg = coverJpeg(cover);
    const QString file = folder + "/cover.jpg";
    const QString before = hashFile(file);
    if (jpeg.isEmpty() || !writeWhole(file, jpeg))
        return false;
    // The same picture again is no change to the details.
    if (at || hashFile(file) != before)
        return exec("UPDATE books SET has_cover = 1, details_at = ? WHERE id = ?",
                    {at ? Hlc::seconds(*at) : Spaces::wallSeconds(*this), id});
    return exec("UPDATE books SET has_cover = 1 WHERE id = ?", {id});
}

QString Library::coverPath(qint64 id) const {
    const QString path = bookFolder(id) + "/cover.jpg";
    return QFileInfo::exists(path) ? path : QString();
}

QString Library::formatPath(qint64 id, const QString &format) const {
    QHash<QString, QString> files;
    for (QSqlQuery found = query("SELECT format, file FROM formats WHERE book_id = ?", {id}); found.next();)
        files.insert(found.value(0).toString(), found.value(1).toString());
    const QString chosen = format.isEmpty() ? Formats::byPreference(files.keys()).value(0) : format.toLower();
    return files.contains(chosen) ? bookFolder(id) + "/" + files.value(chosen) : QString();
}

QList<FormatFile> Library::formatFiles(qint64 id) const {
    const QString folder = bookFolder(id);
    QList<FormatFile> files;
    for (QSqlQuery found = query("SELECT format, file, hash, size, original FROM formats WHERE book_id = ?", {id}); found.next();)
        files.append({.format = found.value(0).toString(),
                      .path = folder + "/" + found.value(1).toString(),
                      .hash = found.value(2).toString(),
                      .size = found.value(3).toLongLong(),
                      .original = found.value(4).toString()});
    return files;
}

qint64 Library::bookWithHash(const QString &hash) const {
    QSqlQuery found = query("SELECT book_id FROM formats WHERE hash = ? OR original = ? ORDER BY hash = ? DESC", {hash, hash, hash});
    return !hash.isEmpty() && found.next() ? found.value(0).toLongLong() : 0;
}

void Library::knowOriginal(qint64 id, const QString &format, const QString &original) {
    if (!original.isEmpty())
        exec("UPDATE formats SET original = ? WHERE book_id = ? AND format = ? AND original = '' AND hash <> ?",
             {original, id, format, original});
}

QString Library::originalFile(qint64 id) const {
    const QList<FormatFile> files = formatFiles(id);
    const auto held = std::ranges::find(files, QString(epub), &FormatFile::format);
    const QString kept = held == files.end() || held->original.isEmpty() ? QString() : keptOriginal(held->path);
    return QFileInfo::exists(kept) ? kept : QString();
}

std::expected<FormatFile, Failure> Library::epubFile(qint64 id) {
    if (!book(id))
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(id)}));
    const QList<FormatFile> files = formatFiles(id);
    const auto held = std::ranges::find(files, QString(epub), &FormatFile::format);
    if (held == files.end())
        return std::unexpected(Failure(Failure::Code::DetailsNeedEpub));
    FormatFile file = *held;
    // A write cut short after its file was put in place, before the library
    // was told, leaves the two apart: the library is told now.
    if (const QString actual = hashFile(file.path); !actual.isEmpty() && actual != file.hash) {
        const QString kept = keptOriginal(file.path);
        if (file.original.isEmpty() && QFileInfo::exists(kept))
            file.original = hashFile(kept);
        file.hash = actual;
        file.size = QFileInfo(file.path).size();
        exec("UPDATE formats SET hash = ?, size = ?, original = ? WHERE book_id = ? AND format = ?",
             {file.hash, file.size, file.original, id, epub});
    }
    return file;
}

std::expected<Library::DetailsWritten, Failure> Library::writeDetails(qint64 id) {
    const auto held = heldAlone(formatPath(id, epub));
    if (!held)
        return std::unexpected(held.error());
    const auto file = epubFile(id);
    if (!file)
        return std::unexpected(file.error());
    clearPartials(file->path);
    const QString partial = partialFor(file->path);
    const auto leftOver = qScopeGuard([&partial] { QFile::remove(partial); });
    if (const auto made = writtenBeside(file->path, partial, detailsFor(*this->book(id), coverPath(id), file->path)); !made)
        return std::unexpected(made.error());
    const QString hash = hashFile(partial);
    if (hash == file->hash)
        return DetailsWritten{.changed = false, .hash = hash};
    if (file->original.isEmpty())
        if (const auto kept = keepOriginal(*file); !kept)
            return std::unexpected(kept.error());
    QFile::setPermissions(partial, QFileInfo(file->path).permissions());
    if (const auto put = replaceWith(partial, file->path); !put)
        return std::unexpected(put.error());
    exec("UPDATE formats SET hash = ?, size = ?, original = ? WHERE book_id = ? AND format = ?",
         {hash, QFileInfo(file->path).size(), file->original.isEmpty() ? file->hash : file->original, id, epub});
    return DetailsWritten{.changed = true, .hash = hash};
}

std::expected<void, Failure> Library::restoreFile(qint64 id) {
    const auto held = heldAlone(formatPath(id, epub));
    if (!held)
        return std::unexpected(held.error());
    const auto file = epubFile(id);
    if (!file)
        return std::unexpected(file.error());
    const QString kept = keptOriginal(file->path);
    if (file->original.isEmpty() || hashFile(kept) != file->original)
        return std::unexpected(Failure(Failure::Code::NoOriginalKept));
    const qint64 size = QFileInfo(kept).size();
    if (const auto put = replaceWith(kept, file->path); !put)
        return std::unexpected(put.error());
    QDir().rmdir(QFileInfo(kept).absolutePath());
    exec("UPDATE formats SET hash = ?, size = ?, original = '' WHERE book_id = ? AND format = ?", {file->original, size, id, epub});
    return {};
}
