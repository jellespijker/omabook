#include "hubstorebridge.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <algorithm>
#include <utility>

#include "httpserver.h"
#include "hublinks.h"
#include "hubstore.h"
#include "log.h"
#include "snapshots.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

[[nodiscard]] HttpResponse respond(int status, const QByteArray &body = {}, const QByteArray &type = "application/octet-stream") {
    return {.status = status, .contentType = type, .body = body, .file = {}, .headers = {}};
}

// What restic asks of, by the REST protocol's path: a type's listing ("data/"), a file ("data/<id>", "config"), or the repository ("").
struct Asked {
    QString type;
    QString file;
};

[[nodiscard]] Asked askedOf(const QString &path) {
    const QString clean = path.mid(1);
    if (clean.endsWith(u'/') && !clean.chopped(1).contains(u'/'))
        return {.type = clean.chopped(1), .file = {}};
    return {.type = {}, .file = clean};
}

// The bytes `range` (bytes=<from>-[<to>]) asks of a file of `size`; all of it without one.
[[nodiscard]] std::optional<std::pair<qint64, qint64>> rangeOf(const QByteArray &range, qint64 size) {
    if (range.isEmpty())
        return std::pair{qint64{0}, size};
    static const QRegularExpression bytes(u"^bytes=(\\d+)-(\\d*)$"_s);
    const auto match = bytes.match(QString::fromLatin1(range).trimmed());
    if (!match.hasMatch())
        return std::nullopt;
    const qint64 from = match.captured(1).toLongLong();
    const qint64 to = match.captured(2).isEmpty() ? size - 1 : std::min(match.captured(2).toLongLong(), size - 1);
    if (from > to + 1 || from > size)
        return std::nullopt;
    return std::pair{from, to - from + 1};
}

} // namespace

HubStoreBridge::HubStoreBridge(QString hub, QString repository, Open open)
    : m_hub(std::move(hub)), m_repository(std::move(repository)), m_open(std::move(open)), m_user("omabook"),
      m_password(Snapshots::newRecoveryKey().toLatin1()), m_authorization("Basic " + (m_user + ':' + m_password).toBase64()),
      m_server(std::make_unique<HttpServer>([this](const HttpRequest &request) { return answer(request); })) {
    m_server->takeBodiesUpTo(largest);
    m_server->moveToThread(&m_thread);
}

HubStoreBridge::~HubStoreBridge() {
    if (m_thread.isRunning()) {
        // The server and the session are the thread's: they go there.
        QMetaObject::invokeMethod(
            m_server.get(),
            [this] {
                m_server->close();
                m_ask.reset();
            },
            Qt::BlockingQueuedConnection);
        m_thread.quit();
        m_thread.wait();
    }
}

std::expected<void, Failure> HubStoreBridge::start() {
    m_thread.start();
    std::expected<quint16, Failure> listening = std::unexpected(Failure(Failure::Code::ServerUnstarted, {u"127.0.0.1"_s, {}}));
    QMetaObject::invokeMethod(
        m_server.get(), [this, &listening] { listening = m_server->listen(QHostAddress::LocalHost, 0); }, Qt::BlockingQueuedConnection);
    if (!listening)
        return std::unexpected(listening.error());
    m_port = *listening;
    return {};
}

ResticRepository HubStoreBridge::repository() const {
    return {.location = u"rest:http://127.0.0.1:%1/"_s.arg(m_port),
            .passwordFile = {},
            .password = {},
            .program = u"restic"_s,
            .environment = {{u"RESTIC_REST_USERNAME"_s, m_user}, {u"RESTIC_REST_PASSWORD"_s, m_password}},
            .through = {}};
}

std::expected<QJsonObject, Failure> HubStoreBridge::store(QJsonObject request) {
    if (m_failed)
        return std::unexpected(*m_failed);
    if (!m_ask) {
        auto opened = m_open();
        if (!opened) {
            m_failed = opened.error();
            return std::unexpected(opened.error());
        }
        m_ask = std::move(*opened);
    }
    request.insert("request"_L1, u"store"_s);
    request.insert("repository"_L1, m_repository);
    QJsonObject answered = QJsonDocument::fromJson((*m_ask)(compact(request))).object();
    if (answered.isEmpty()) {
        m_failed = Failure(Failure::Code::HubSessionEnded, {m_hub});
        return std::unexpected(*m_failed);
    }
    if (!answered.value("ok"_L1).toBool())
        return std::unexpected(Failure::said(answered.value("error"_L1).toString()));
    return answered;
}

HttpResponse HubStoreBridge::answer(const HttpRequest &request) {
    if (request.header("authorization") != m_authorization)
        return respond(401);
    const Asked asked = askedOf(request.path);
    if (request.method == "POST" && request.path == u"/"_s) {
        const auto made = store({{"op", "create"}});
        return respond(made ? 200 : 500);
    }
    if (!asked.type.isEmpty() && request.method == "GET") {
        const auto listed = store({{"op", "list"}, {"type", asked.type}});
        if (!listed)
            return respond(500);
        const QJsonArray files = listed->value("files"_L1).toArray();
        if (request.header("accept").contains("application/vnd.x.restic.rest.v2"))
            return respond(200, QJsonDocument(files).toJson(QJsonDocument::Compact), "application/vnd.x.restic.rest.v2");
        QJsonArray names;
        for (const auto &one : files)
            names << one["name"_L1];
        return respond(200, QJsonDocument(names).toJson(QJsonDocument::Compact), "application/json");
    }
    return file(request, asked.file);
}

HttpResponse HubStoreBridge::file(const HttpRequest &request, const QString &path) {
    if (request.method == "POST")
        return write(path, request.body);
    if (request.method == "DELETE") {
        const auto removed = store({{"op", "remove"}, {"path", path}});
        if (!removed)
            return respond(500);
        return respond(removed->value("removed"_L1).toBool() ? 200 : 404);
    }
    if (request.method != "HEAD" && request.method != "GET")
        return respond(405);
    return read(request, path);
}

HttpResponse HubStoreBridge::read(const HttpRequest &request, const QString &path) {
    const auto stat = store({{"op", "stat"}, {"path", path}});
    if (!stat)
        return respond(500);
    if (!stat->value("exists"_L1).toBool())
        return respond(404);
    const qint64 size = stat->value("size"_L1).toInteger();
    if (request.method == "HEAD") {
        HttpResponse head = respond(200);
        head.headers.insert("Content-Length", QByteArray::number(size));
        return head;
    }
    const auto range = rangeOf(request.header("range"), size);
    if (!range)
        return respond(416);
    QByteArray data;
    for (qint64 at = range->first; at < range->first + range->second;) {
        const qint64 length = std::min<qint64>(HubStore::piece, range->first + range->second - at);
        const auto piece = store({{"op", "get"}, {"path", path}, {"at", at}, {"length", length}});
        if (!piece)
            return respond(500);
        const QByteArray got = QByteArray::fromBase64(piece->value("data"_L1).toString().toLatin1());
        if (got.isEmpty())
            return respond(500);
        data += got;
        at += got.size();
    }
    return respond(request.header("range").isEmpty() ? 200 : 206, data);
}

HttpResponse HubStoreBridge::write(const QString &path, const QByteArray &body) {
    // An empty file is one piece, its last, too.
    for (qsizetype at = 0, pieces = 0; at < body.size() || pieces == 0; ++pieces) {
        const QByteArray piece = body.mid(at, HubStore::piece);
        const bool last = at + piece.size() >= body.size();
        const auto put =
            store({{"op", "put"}, {"path", path}, {"at", at}, {"data", QString::fromLatin1(piece.toBase64())}, {"last", last}});
        if (!put) {
            qCWarning(lcLibrary).noquote() << "A snapshot's file couldn't be kept on" << m_hub << "-" << put.error().english();
            return respond(500);
        }
        at += piece.size();
    }
    return respond(200);
}

SnapshotDestinations::HubReach HubStoreBridge::reach(std::function<std::expected<Ask, Failure>(const QString &host)> open) {
    return [open = std::move(open)](const QString &host, const QString &name) -> std::expected<ResticRepository, Failure> {
        auto bridge = std::make_shared<HubStoreBridge>(host, name, [open, host] { return open(host); });
        if (const auto started = bridge->start(); !started)
            return std::unexpected(started.error());
        ResticRepository repository = bridge->repository();
        repository.through = std::move(bridge);
        return repository;
    };
}

SnapshotDestinations::HubReach HubStoreBridge::linked() {
    return reach([](const QString &host) -> std::expected<Ask, Failure> {
        const HubLinks links;
        auto opened = links.open(host);
        if (!opened)
            return std::unexpected(opened.error());
        const std::shared_ptr<HubLinks::Session> session = std::move(*opened);
        return Ask([session](const QByteArray &line) { return session->ask(line); });
    });
}
