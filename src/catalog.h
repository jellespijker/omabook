#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "library.h"

// A catalog is the library, or part of it, written out as one file for
// something else to read: a spreadsheet, a reference manager. These are
// Omabook's counterparts of Calibre's catalog generator plugins.
namespace Catalog {

// "csv" and "bibtex".
QStringList formats();
QString describe(const QString &format);
// The catalog of these books, or a null array for a format there isn't.
QByteArray make(const QList<Book> &books, const QString &format);

} // namespace Catalog
