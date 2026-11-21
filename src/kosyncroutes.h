#pragma once

#include <QString>
#include <QtTypes>
#include <functional>
#include <optional>

#include "httpserver.h"
#include "koreaderbooks.h"

class Library;

// KOReader's progress sync, served (#71): what koreader-sync-server answers,
// so KOReader on an e-reader or a phone, pointed at Omabook as its custom
// sync server, keeps its reading place in step with the library's. One
// person, by the name and key set in Omabook: logging in with them works,
// and so does registering them, which is how some KOReader versions log in
// the first time; anyone else is turned away.
class KosyncRoutes {
public:
    // `user` and `key` (the MD5 of the password) are the one person's;
    // `taken` is told of each book whose place a device moved.
    KosyncRoutes(Library &library, KoreaderBooks &books, QString user, QString key, std::function<void(qint64)> taken);

    // The answer to a request on one of its paths (/users/…, /syncs/…,
    // /healthcheck); none for any other.
    [[nodiscard]] std::optional<HttpResponse> answer(const HttpRequest &request);

private:
    [[nodiscard]] bool authorized(const HttpRequest &request) const;
    [[nodiscard]] HttpResponse put(const HttpRequest &request);
    [[nodiscard]] HttpResponse get(const QString &document);
    [[nodiscard]] HttpResponse create(const HttpRequest &request) const;

    Library *m_library;     // never null: what it was made with
    KoreaderBooks *m_books; // never null: what it was made with
    QString m_user;
    QString m_key;
    QString m_deviceId;
    std::function<void(qint64)> m_taken;
};
