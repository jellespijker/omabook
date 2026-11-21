#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <expected>
#include <optional>

#include "failure.h"

// An OPDS 1.x catalog as Omabook reads one (#71): Standard Ebooks, Project
// Gutenberg, a Calibre content server, or another Omabook's. A feed is a
// list of entries, each either a book with links to download it in its
// formats, or a link to another feed.
namespace OpdsCatalog {

struct Link {
    QString rel;
    QUrl href; // made whole against the feed's own address
    QString type;
    QString title;
};

struct Entry {
    QString id;
    QString title;
    QStringList authors;
    QString summary;
    QList<Link> links;

    // The links that download the book: rel http://opds-spec.org/acquisition, or one of its kinds.
    [[nodiscard]] QList<Link> acquisitions() const;
    // The feed this entry leads to, when it is a link to another rather than a book.
    [[nodiscard]] std::optional<Link> feed() const;
};

struct Feed {
    QString id;
    QString title;
    QList<Link> links;
    QList<Entry> entries;

    [[nodiscard]] std::optional<Link> link(const QString &rel) const;
};

// The feed in `xml`, read from `address`. A document that is no Atom feed fails.
[[nodiscard]] std::expected<Feed, Failure> parse(const QByteArray &xml, const QUrl &address);

} // namespace OpdsCatalog
