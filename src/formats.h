#pragma once

#include <QString>
#include <QStringList>
#include <memory>

#include "document.h"

// The one place that knows which formats exist. A format Omabook reads itself
// has an opener; one it only recognizes (Kindle files, Word documents, ...)
// is read after converting it to EPUB, see converter.h.
namespace Formats {

// Lower-case extension of a file, with "fb2.zip" as one extension.
QString extension(const QString &path);
// Every extension Omabook takes into a library.
QStringList extensions();
bool isBook(const QString &path);
// Whether Omabook reads this format itself, without converting it.
bool readable(const QString &extension);
// The display name, such as "EPUB" or "Kindle (AZW3)".
QString name(const QString &extension);
// Formats of one book in the order the reader prefers them.
QStringList byPreference(QStringList extensions);

// Opens a book for reading; on failure the result is null and `error` says
// why in English, for the caller to show.
std::unique_ptr<Document> open(const QString &path, QString *error = nullptr);

// What the file says about itself, with the file name standing in for a
// missing title ("Author - Title.epub" gives both).
BookMeta readMeta(const QString &path);
// "Author - Title" or "Title", from a file name.
BookMeta metaFromFileName(const QString &path);

} // namespace Formats
