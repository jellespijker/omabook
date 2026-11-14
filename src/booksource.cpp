#include "booksource.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>
#include <array>

#include "bookarchive.h"
#include "htmlclean.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool hidden(const QString &name) {
    return std::ranges::any_of(name.split('/'), [](const QString &part) { return part.startsWith('.') || part == "__MACOSX"_L1; });
}

} // namespace

BookSource::BookSource(const QString &path) : m_path(path), m_kind(kindOf(path)) {}

BookSource::Kind BookSource::kindOf(const QString &path) {
    const QFileInfo info(path);
    if (info.isDir())
        return Kind::Folder;
    // An archive is told by how it begins, not by what it is called, so a
    // format's own extension for a zipped book needs no list kept here.
    QFile file(path);
    const QByteArray start = file.open(QIODevice::ReadOnly) ? file.read(8) : QByteArray();
    static constexpr std::array tarred{"tar"_L1, "tgz"_L1, "gz"_L1, "cbt"_L1};
    const bool archived = start.startsWith("PK") || start.startsWith("Rar!") || start.startsWith("7z\xBC\xAF")
                          || std::ranges::contains(tarred, info.suffix().toLower());
    return archived ? Kind::Archived : Kind::Single;
}

QString BookSource::name() const {
    const QFileInfo info(m_path);
    return m_kind == Kind::Folder ? info.fileName() : info.completeBaseName();
}

void BookSource::sortNaturally(QStringList &names) {
    QCollator natural;
    natural.setNumericMode(true);
    natural.setCaseSensitivity(Qt::CaseInsensitive);
    std::ranges::sort(names, natural);
}

QStringList BookSource::entries() const {
    QStringList names;
    switch (m_kind) {
    case Kind::Single:
        names << QFileInfo(m_path).fileName();
        break;
    case Kind::Archived:
        names = Archive(m_path).entries();
        break;
    case Kind::Folder: {
        const QDir root(m_path);
        QDirIterator walk(m_path, QDir::Files, QDirIterator::Subdirectories);
        while (walk.hasNext())
            names << root.relativeFilePath(walk.next());
        break;
    }
    }
    names.removeIf(hidden);
    sortNaturally(names);
    return names;
}

QByteArray BookSource::read(const QString &name) const {
    // A name is a path inside the book and nothing else.
    if (name.isEmpty() || name.startsWith('/') || QDir::cleanPath(name).startsWith(".."))
        return {};
    if (m_kind == Kind::Archived)
        return Archive(m_path).read(name);
    QFile file(m_kind == Kind::Single ? m_path : m_path + "/" + name);
    if (m_kind == Kind::Single && name != QFileInfo(m_path).fileName())
        return {};
    // What one file of a book may honestly weigh is what one member of an archive may.
    if (file.size() > Archive::maximumEntrySize || !file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray data = file.readAll();
    return data.isNull() ? QByteArray("") : data;
}

QString BookSource::text(const QString &name) const { return decodeText(read(name)); }

QString BookSource::resolve(const QString &from, const QString &relative) {
    QString target = relative.section('#', 0, 0).section('?', 0, 0);
    if (target.isEmpty() || target.contains("://") || target.startsWith("mailto:") || target.startsWith("data:"))
        return {};
    target = QUrl::fromPercentEncoding(target.toUtf8());
    const QString folder = from.contains('/') ? from.section('/', 0, -2) + "/" : QString();
    const QString cleaned = QDir::cleanPath(target.startsWith('/') ? target.mid(1) : folder + target);
    return cleaned.startsWith("..") ? QString() : cleaned;
}

bool BookSource::pack(const QString &zipPath) const {
    QList<QPair<QString, QByteArray>> files;
    for (const QString &name : entries()) {
        const QByteArray data = read(name);
        if (data.isNull())
            return false;
        files.append({name, data});
    }
    return !files.isEmpty() && Archive::write(zipPath, files);
}
