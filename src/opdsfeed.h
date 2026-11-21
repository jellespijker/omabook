#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

#include "library.h"

// A library as an OPDS 1.2 catalog (#71), the Atom feeds e-reader and phone
// apps (KOReader, Moon+ Reader, Librera, Thorium, …) browse and download
// books from. Only the feeds: which books go in, and who may ask, is the
// server's (DeviceServer). Every link starts with `base`, the path the
// catalog is served at, so the feeds work behind any prefix.
namespace OpdsFeed {

inline constexpr auto navigationType = "application/atom+xml;profile=opds-catalog;kind=navigation";
inline constexpr auto acquisitionType = "application/atom+xml;profile=opds-catalog;kind=acquisition";

// An entry of a navigation feed: a feed of its own to go to.
struct Section {
    QString id;
    QString title;
    QString summary;
    QString href;            // below `base`: "/books", "/collections/3"
    bool acquisition = true; // a feed of books, else one of more sections
};

// A page of books, with the pages before and after it where there are some.
struct Page {
    QString id;
    QString title;
    QString self; // below `base`, as the others
    QList<Book> books;
    QString previous, next;
    QDateTime updated;
};

[[nodiscard]] QByteArray navigation(const QString &base, const QString &id, const QString &title, const QList<Section> &sections,
                                    const QDateTime &updated);
// Each book with its details, its cover and a link to download each of its formats.
[[nodiscard]] QByteArray acquisition(const QString &base, const Page &page);
// The OpenSearch description that tells an app how to search the catalog.
[[nodiscard]] QByteArray openSearch(const QString &base);

// The media type an app knows a format's file by.
[[nodiscard]] QString mimeType(const QString &format);

} // namespace OpdsFeed
