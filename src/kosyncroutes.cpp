#include "kosyncroutes.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>

#include "library.h"

using namespace Qt::StringLiterals;

namespace {

// The answers koreader-sync-server gives, by its codes.
[[nodiscard]] HttpResponse said(int status, int code, const char *message) {
    return HttpResponse::json(status, QJsonDocument(QJsonObject{{"code", code}, {"message", message}}).toJson(QJsonDocument::Compact));
}

[[nodiscard]] HttpResponse answered(const QJsonObject &object, int status = 200) {
    return HttpResponse::json(status, QJsonDocument(object).toJson(QJsonDocument::Compact));
}

[[nodiscard]] HttpResponse unauthorized() { return said(401, 2001, "Unauthorized"); }

[[nodiscard]] HttpResponse invalid() { return said(403, 2003, "Invalid request"); }

// How far into its book a place is: a number from 0 to 1. Without one, or with
// another, the place would be taken as the book's start or end (#287).
[[nodiscard]] bool saysHowFar(const QJsonObject &record) {
    const QJsonValue percentage = record.value("percentage");
    return percentage.isDouble() && percentage.toDouble() >= 0 && percentage.toDouble() <= 1;
}

} // namespace

KosyncRoutes::KosyncRoutes(Library &library, KoreaderBooks &books, QString user, QString key, std::function<void(qint64)> taken)
    : m_library(&library), m_books(&books), m_user(std::move(user)), m_key(std::move(key)), m_deviceId(KoreaderBooks::deviceId(library)),
      m_taken(std::move(taken)) {}

bool KosyncRoutes::authorized(const HttpRequest &request) const {
    return !m_user.isEmpty() && !m_key.isEmpty() && QString::fromUtf8(request.header("x-auth-user")) == m_user
           && QString::fromUtf8(request.header("x-auth-key")).compare(m_key, Qt::CaseInsensitive) == 0;
}

std::optional<HttpResponse> KosyncRoutes::answer(const HttpRequest &request) {
    const QString &path = request.path;
    if (path == u"/healthcheck")
        return answered({{"state", "OK"}});
    if (path == u"/users/create")
        return request.method == "POST" ? create(request) : said(405, 2000, "Unknown server error.");
    if (path == u"/users/auth")
        return authorized(request) ? answered({{"authorized", "OK"}}) : unauthorized();
    if (path == u"/syncs/progress") {
        if (!authorized(request))
            return unauthorized();
        return request.method == "PUT" ? put(request) : said(405, 2000, "Unknown server error.");
    }
    if (path.startsWith(u"/syncs/progress/")) {
        if (!authorized(request))
            return unauthorized();
        return get(path.mid(QStringView(u"/syncs/progress/").size()));
    }
    return std::nullopt;
}

HttpResponse KosyncRoutes::create(const HttpRequest &request) const {
    // Registering is logging in as the one person Omabook knows.
    const QJsonObject asked = QJsonDocument::fromJson(request.body).object();
    if (!m_user.isEmpty() && asked.value("username").toString() == m_user
        && asked.value("password").toString().compare(m_key, Qt::CaseInsensitive) == 0)
        return answered({{"username", m_user}}, 201);
    return said(402, 2005, "User registration is disabled.");
}

HttpResponse KosyncRoutes::put(const HttpRequest &request) {
    const QJsonObject asked = QJsonDocument::fromJson(request.body).object();
    auto record = KoreaderRecord::fromJson(asked);
    if (!record || !saysHowFar(asked))
        return invalid();
    record->document = record->document.toLower();
    record->timestamp = QDateTime::currentSecsSinceEpoch();
    KoreaderRecords::hear(*m_library, *record);
    if (const auto file = m_books->find(record->document); file && m_books->take(*file, *record) && m_taken)
        m_taken(file->book);
    return answered({{"document", record->document}, {"timestamp", record->timestamp}});
}

HttpResponse KosyncRoutes::get(const QString &document) {
    const QString asked = document.toLower();
    const auto heard = KoreaderRecords::heard(*m_library, asked);
    const auto file = m_books->find(asked);
    const auto ours = file ? m_books->recordOf(*file, asked, m_deviceId) : std::nullopt;
    // What a device said stands until the book is read further here.
    if (ours && (!heard || ours->timestamp > heard->timestamp))
        return answered(ours->toJson());
    return answered(heard ? heard->toJson() : QJsonObject{});
}
