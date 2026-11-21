#pragma once

#include <QList>
#include <QString>
#include <optional>

#include "httpserver.h"
#include "library.h"

// The library's OPDS catalog, served (#71): read-only, for whoever has the
// token, of the whole library or of one collection and those inside it.
// Only books with a file are in it, and never a book a plugin keeps private
// (PrivateBooks) that the person didn't let go. The token comes as the
// password of HTTP basic authentication (any name), as a bearer token, or in
// the path: /t/<token>/opds, for an app that can't send either.
class OpdsRoutes {
public:
    // `collection` is the path of the collection served; empty for all of the library.
    OpdsRoutes(const Library &library, QString token, QString collection);

    // The answer to a request below /opds or /t/<token>/opds; none for any other.
    [[nodiscard]] std::optional<HttpResponse> answer(const HttpRequest &request) const;

private:
    [[nodiscard]] QList<Book> served() const;
    [[nodiscard]] std::optional<Collection> servedCollection() const;
    [[nodiscard]] QList<Collection> sections() const;
    [[nodiscard]] HttpResponse route(const HttpRequest &request, const QString &base, const QString &rest) const;
    [[nodiscard]] HttpResponse root(const QString &base) const;
    [[nodiscard]] HttpResponse search(const HttpRequest &request, const QString &base, const QString &rest) const;
    [[nodiscard]] HttpResponse collectionPage(const HttpRequest &request, const QString &base, const QString &rest) const;
    [[nodiscard]] HttpResponse page(const QString &base, const QString &self, const QString &id, const QString &title, QList<Book> books,
                                    const HttpRequest &request) const;
    [[nodiscard]] HttpResponse bookFile(const QString &rest) const;

    const Library *m_library; // never null: what it was made with
    QString m_token;
    QString m_collection;
};
