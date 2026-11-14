#pragma once

#include <QString>
#include <concepts>
#include <expected>
#include <memory>
#include <utility>

#include "document.h"

// A book opened for reading, or why (in English) the file can't be read.
using OpenedDocument = std::expected<std::unique_ptr<Document>, QString>;

// One opener per format Omabook reads itself; formats.cpp maps extensions to them.
OpenedDocument openEpub(const QString &path);
OpenedDocument openFb2(const QString &path);
OpenedDocument openText(const QString &path);
OpenedDocument openPdf(const QString &path);
OpenedDocument openComic(const QString &path);

// What every opener does with its own kind of book: the book once it has
// loaded, or what its loading said was wrong.
template <std::derived_from<Document> Book, typename... Arguments>
    requires requires(Book &book, Arguments &&...arguments) {
        { book.load(std::forward<Arguments>(arguments)...) } -> std::same_as<std::expected<void, QString>>;
    }
OpenedDocument loaded(std::unique_ptr<Book> book, Arguments &&...arguments) {
    return book->load(std::forward<Arguments>(arguments)...).transform([&book]() -> std::unique_ptr<Document> {
        return std::move(book);
    });
}

// Kindle files are read after conversion, but say who they are without it.
BookMeta readMobiMeta(const QString &path);

// The attributes of an <img> for a picture inside a book: its address for the
// reader's image provider and a size no wider than `width` (0 for any).
QString bookImageAttributes(const QString &name, const QByteArray &data, int width);
// The name a picture address from bookImageAttributes refers to.
QString bookImageName(const QString &encoded);
