#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// The files a book is made of, wherever they are: a folder, an archive (zip,
// rar, 7z, tar), or one file on its own. A format that reads a book through
// this works on all three without knowing which it has, which is what lets a
// folder of chapters and the same folder zipped be one format.
//
// Names are the files' paths inside the book, with forward slashes. Safe to
// use from several threads: nothing is kept open between calls.
class BookSource {
public:
    explicit BookSource(const QString &path);

    QString path() const { return m_path; }
    // The book's name as its folder or file is called, without an extension.
    QString name() const;
    bool isFolder() const noexcept { return m_kind == Kind::Folder; }

    // Every file, in natural order ("2" before "10") with hidden files and
    // folders left out; empty when the book can't be read.
    QStringList entries() const;
    bool contains(const QString &name) const { return entries().contains(name); }
    // One file's bytes; null when it is missing, too large, or the name
    // points outside the book.
    QByteArray read(const QString &name) const;
    // The same, as text in whatever encoding it is in.
    QString text(const QString &name) const;

    // `relative` as written in the file `from`, as a name in the book:
    // "../img/a.png" from "chapters/one.md" is "img/a.png". Empty when it
    // leaves the book or is an address elsewhere.
    static QString resolve(const QString &from, const QString &relative);

    // Writes every file as a zip at `zipPath`, which is how a folder becomes
    // one file a library can hold. False when something couldn't be written.
    bool pack(const QString &zipPath) const;

    // Puts names in natural order: "2" before "10", whatever the case.
    static void sortNaturally(QStringList &names);

private:
    enum class Kind { Folder, Archived, Single };
    static Kind kindOf(const QString &path);

    QString m_path;
    Kind m_kind;
};
