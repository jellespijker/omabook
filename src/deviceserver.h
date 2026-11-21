#pragma once

#include <QList>
#include <QNetworkInterface>
#include <QObject>
#include <QString>
#include <expected>
#include <memory>

#include "deviceoptions.h"
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
// it on one (DeviceDesk), `omabook serve` on its main thread.
class DeviceServer : public QObject {
    Q_OBJECT

public:
    // What it is started with: the options, and the secrets they need from the keyring.
    struct Setup {
        DeviceOptions options;
        QString opdsToken;
        QString koreaderKey; // the MD5 of the password KOReader logs in with
    };

    explicit DeviceServer(QString libraryRoot, QObject *parent = nullptr);
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

signals:
    // A device moved the reading place of a book.
    void readTo(qint64 book);

private:
    [[nodiscard]] HttpResponse handle(const HttpRequest &request);

    QString m_root;
    std::unique_ptr<Library> m_library;
    std::unique_ptr<KoreaderBooks> m_books;
    std::unique_ptr<OpdsRoutes> m_opds;
    std::unique_ptr<KosyncRoutes> m_kosync;
    HttpServer m_http;
};
