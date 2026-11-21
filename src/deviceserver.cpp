#include "deviceserver.h"

#include "koreaderbooks.h"
#include "kosyncroutes.h"
#include "library.h"
#include "log.h"
#include "opdsroutes.h"

using namespace Qt::StringLiterals;

DeviceServer::DeviceServer(QString libraryRoot, QObject *parent)
    : QObject(parent), m_root(std::move(libraryRoot)), m_http([this](const HttpRequest &request) { return handle(request); }, this) {}

DeviceServer::~DeviceServer() = default;

std::expected<QString, Failure> DeviceServer::start(const Setup &setup, const QList<QNetworkInterface> &interfaces) {
    stop();
    const DeviceOptions &options = setup.options;
    if (options.reach == DeviceOptions::Reach::Off || (!options.opds && !options.koreaderServe))
        return QString();
    const auto address = DeviceReach::address(options.reach, interfaces);
    if (!address)
        return std::unexpected(Failure(Failure::Code::NoTailnet));
    auto library = std::make_unique<Library>(m_root);
    if (QString why; !library->open(&why))
        return std::unexpected(Failure(Failure::Code::LibraryUnwritable, {why}));
    m_library = std::move(library);
    m_books = std::make_unique<KoreaderBooks>(*m_library);
    if (options.opds)
        m_opds = std::make_unique<OpdsRoutes>(*m_library, setup.opdsToken, options.collection);
    if (options.koreaderServe)
        m_kosync = std::make_unique<KosyncRoutes>(*m_library, *m_books, options.koreaderServeUser, setup.koreaderKey,
                                                  [this](qint64 book) { emit readTo(book); });
    const auto port = m_http.listen(*address, options.port);
    if (!port) {
        stop();
        return std::unexpected(port.error());
    }
    return DeviceReach::shown(options.reach, *address, *port);
}

void DeviceServer::stop() {
    m_http.close();
    m_kosync.reset();
    m_opds.reset();
    m_books.reset();
    m_library.reset();
}

HttpResponse DeviceServer::handle(const HttpRequest &request) {
    if (m_opds) {
        if (auto answer = m_opds->answer(request))
            return *std::move(answer);
    }
    if (m_kosync) {
        if (auto answer = m_kosync->answer(request))
            return *std::move(answer);
    }
    qCInfo(lcSync) << request.peer.toString() << "asked for" << request.path << "which isn't served";
    return {404, "text/plain; charset=utf-8", "Not found", {}, {}};
}
