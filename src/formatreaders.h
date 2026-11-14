#pragma once

#include <QString>
#include <memory>

#include "document.h"

// One opener per format Omabook reads itself; formats.cpp maps extensions to
// them. Each returns null and sets `error` (English) when the file can't be read.
std::unique_ptr<Document> openEpub(const QString &path, QString *error);
std::unique_ptr<Document> openFb2(const QString &path, QString *error);
std::unique_ptr<Document> openText(const QString &path, QString *error);
std::unique_ptr<Document> openPdf(const QString &path, QString *error);
std::unique_ptr<Document> openComic(const QString &path, QString *error);

// Kindle files are read after conversion, but say who they are without it.
BookMeta readMobiMeta(const QString &path);

// The attributes of an <img> for a picture inside a book: its address for the
// reader's image provider and a size no wider than `width` (0 for any).
QString bookImageAttributes(const QString &name, const QByteArray &data, int width);
// The name a picture address from bookImageAttributes refers to.
QString bookImageName(const QString &encoded);
