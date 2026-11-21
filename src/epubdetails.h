#pragma once

#include <QByteArray>
#include <QString>
#include <expected>

#include "failure.h"
#include "opfdetails.h"

// A book's details written into an EPUB (#74): the metadata of its package
// document (OpfDetails) and, when there is one to write, its cover, in a new
// zip in which every other file is as it was (ZipEdit). The file read is
// never changed: what is written is handed back, for whoever keeps the file
// to put in its place once it has been checked.
namespace EpubDetails {

// The EPUB at `path` with these details written into it: the bytes of the
// new file. A cover replaces the picture the package names as its cover
// when that is a JPEG or a PNG, and is added beside the package otherwise.
[[nodiscard]] std::expected<QByteArray, Failure> written(const QString &path, const OpfDetails::Details &details);

// Whether the EPUB at `path`, as written, opens as a book, begins with the
// mimetype EPUB asks for, and says these details' title, authors and series.
[[nodiscard]] std::expected<void, Failure> check(const QString &path, const OpfDetails::Details &details);

} // namespace EpubDetails
