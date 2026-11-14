#pragma once

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

// Read-only access to the files inside a zip, rar, 7z or tar, which is what
// EPUB and the comic formats are. Every call opens the archive afresh, so one
// Archive is safe to use from several threads and never holds a file open.
class Archive {
public:
    explicit Archive(const QString &path) : m_path(path) {}

    // A book's largest honest member is a scanned page or an embedded font; this
    // stops an archive that claims gigabytes from eating the machine's memory.
    static constexpr qint64 maximumEntrySize = 512LL * 1024 * 1024;

    // The files inside, in the archive's own order; empty when it can't be read.
    QStringList entries() const;
    // One file's bytes, or a null array when it is missing or too large.
    QByteArray read(const QString &name) const;

    // Writes these files, by name, as a new zip at `path`; false when it
    // couldn't be written (what was written of it is removed).
    static bool write(const QString &path, const QList<QPair<QString, QByteArray>> &files);

private:
    QString m_path;
};
