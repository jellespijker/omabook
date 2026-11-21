#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <expected>
#include <optional>

#include "bookmeta.h"
#include "failure.h"

// A book's details written into the metadata of an EPUB's package document,
// the OPF (#74), of EPUB 2 and of EPUB 3, with Calibre's conventions where
// EPUB has none (its "calibre:series" and "calibre:series_index" meta, in
// both; an EPUB 3 also says the series as a collection). Only the metadata
// this writes changes: the rest of the document stays as it was, character
// for character, and so do the metadata it doesn't write (a contributor, a
// rights statement, Calibre's own columns) and the identifier the package is
// known by, which readers keep their places by.
namespace OpfDetails {

struct Details {
    BookMeta meta;           // its cover picture is not read: `cover` is
    QJsonObject identifiers; // what the book is called elsewhere, by scheme
    QDateTime modified;      // when the details last changed, for an EPUB 3's dcterms:modified
    // A JPEG to make the book's cover (EpubDetails), or nothing to leave its cover as it is.
    QByteArray cover;
};

// The picture the package names as its cover: its manifest item's href, as
// written there (relative to the package document), and its media type.
struct CoverItem {
    QString href;
    QString mediaType;
};
[[nodiscard]] std::optional<CoverItem> coverItem(const QByteArray &opf);

// The id of the manifest item a cover added beside the package gets.
inline constexpr auto addedCoverId = "omabook-cover";

// The package document with these details in its metadata. Given
// `newCover`, an href beside the package, that picture (a JPEG) is added to
// the manifest and named the cover. A failure for a package this can't
// change: one that isn't well-formed, isn't UTF-8, or has no metadata.
[[nodiscard]] std::expected<QByteArray, Failure> rewritten(const QByteArray &opf, const Details &details, const QString &newCover = {});

} // namespace OpfDetails
