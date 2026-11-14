#include "booksource.h"

#include <QCollator>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <algorithm>

#include "bookarchive.h"
#include "htmlclean.h"

namespace {

// What one file of a book may honestly weigh; see bookarchive.cpp.
const qint64 maximumFileSize = 512LL * 1024 * 1024;

bool hidden(const QString &name) {
    const QStringList parts = name.split('/');
    return std::any_of(parts.begin(), parts.end(), [](const QString &part) { return part.startsWith('.') || part == "__MACOSX"; });
}

} // namespace

BookSource::BookSource(const QString &path) : m_path(path) {
    const QFileInfo info(path);
    m_kind = info.isDir() ? Folder : Single;
    if (info.isDir())
        return;
    // An archive is told by how it begins, not by what it is called, so a
    // format's own extension for a zipped book needs no list kept here.
    QFile file(path);
    const QByteArray start = file.open(QIODevice::ReadOnly) ? file.read(8) : QByteArray();
    static const QStringList tarred{"tar", "tgz", "gz", "cbt"};
    if (start.startsWith("PK") || start.startsWith("Rar!") || start.startsWith("7z\xBC\xAF")
        || tarred.contains(info.suffix().toLower()))
        m_kind = Archived;
}

QString BookSource::name() const {
    const QFileInfo info(m_path);
    return m_kind == Folder ? info.fileName() : info.completeBaseName();
}

QStringList BookSource::entries() const {
    QStringList names;
    if (m_kind == Single) {
        names << QFileInfo(m_path).fileName();
    } else if (m_kind == Archived) {
        names = Archive(m_path).entries();
    } else {
        const QDir root(m_path);
        QDirIterator walk(m_path, QDir::Files, QDirIterator::Subdirectories);
        while (walk.hasNext())
            names << root.relativeFilePath(walk.next());
    }
    names.erase(std::remove_if(names.begin(), names.end(), hidden), names.end());
    QCollator natural;
    natural.setNumericMode(true);
    natural.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return natural.compare(a, b) < 0; });
    return names;
}

QByteArray BookSource::read(const QString &name) const {
    // A name is a path inside the book and nothing else.
    if (name.isEmpty() || name.startsWith('/') || QDir::cleanPath(name).startsWith(".."))
        return {};
    if (m_kind == Archived)
        return Archive(m_path).read(name);
    QFile file(m_kind == Single ? m_path : m_path + "/" + name);
    if (m_kind == Single && name != QFileInfo(m_path).fileName())
        return {};
    if (file.size() > maximumFileSize || !file.open(QIODevice::ReadOnly))
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
