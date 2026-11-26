#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QNetworkInterface>
#include <QObject>
#include <QString>
#include <expected>
#include <functional>
#include <memory>

#include "deviceoptions.h"
#include "deviceserving.h"
#include "failure.h"
#include "httpserver.h"

class Library;
class KoreaderBooks;
class KosyncRoutes;
class OpdsRoutes;

// The server e-readers and phones talk to (#71): the OPDS catalog and
// KOReader's progress sync on one port, each when it is switched on, on the
// address the person allowed and nowhere else (DeviceReach). It opens the
// library itself, so it can live on a thread of its own: the window starts
// it on one (DeviceDesk), `omabook serve` on its main thread. Or it is lent
// one, a request at a time: the hub's daemon lends it a library in its
// keeper's turn (HubDevices, #193), as the one writer there. It says who it
// is to this computer (DeviceServing), so another Omabook finding its port
// taken can tell.
class DeviceServer : public QObject {
    Q_OBJECT

public:
    // What it is started with: the options, and the secrets they need from the keyring.
    struct Setup {
        DeviceOptions options;
        QString opdsToken;
        QString koreaderKey; // the MD5 of the password KOReader logs in with
        DeviceServing::By by = DeviceServing::By::Window;
    };

    // How a library is lent to it: `work` done with it, and its answer;
    // refused when it can't be had. The same library every time.
    using Work = std::function<HttpResponse(Library &library)>;
    using Lend = std::function<std::expected<HttpResponse, Failure>(const Work &work)>;

    // Serving the library at `libraryRoot`, opened when it starts.
    explicit DeviceServer(QString libraryRoot, QObject *parent = nullptr);
    // Serving a library lent to it for each request.
    explicit DeviceServer(Lend lend, QObject *parent = nullptr);
    ~DeviceServer() override;
    DeviceServer(const DeviceServer &) = delete;
    DeviceServer &operator=(const DeviceServer &) = delete;
    DeviceServer(DeviceServer &&) = delete;
    DeviceServer &operator=(DeviceServer &&) = delete;

    // Listens as the options say; the address to tell a device, as
    // "http://100.101.102.103:6622". Nothing is served when the reach is off,
    // or neither the catalog nor the sync is on.
    [[nodiscard]] std::expected<QString, Failure> start(const Setup &setup,
                                                        const QList<QNetworkInterface> &interfaces = QNetworkInterface::allInterfaces());
    void stop();
    [[nodiscard]] bool running() const { return m_http.listening(); }
    // What it says of itself while it runs.
    [[nodiscard]] DeviceServing::Status status() const;

signals:
    // A device moved the reading place of a book.
    void readTo(qint64 book);

private:
    [[nodiscard]] HttpResponse handle(const HttpRequest &request);
    [[nodiscard]] HttpResponse route(const HttpRequest &request);
    void make(Library &library, const Setup &setup);

    QString m_root;
    Lend m_lend;
    std::unique_ptr<Library> m_library; // its own, when none is lent
    std::unique_ptr<KoreaderBooks> m_books;
    std::unique_ptr<OpdsRoutes> m_opds;
    std::unique_ptr<KosyncRoutes> m_kosync;
    DeviceServing::By m_by = DeviceServing::By::Window;
    QHash<QString, QDateTime> m_seen; // when each device last asked for something
    HttpServer m_http;
};
