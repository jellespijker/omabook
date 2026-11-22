#pragma once

#include <QLocalServer>
#include <QString>
#include <chrono>
#include <cstddef>
#include <expected>

#include "failure.h"

class HubKeeper;
class HubRequests;
class QLocalSocket;

// The hub daemon's socket (ADR 0013, #179): a local one, this user's alone,
// where `omabook hub session` and the hub's own jobs ask, one JSON line a
// request and one an answer. Every request is answered on the daemon's
// thread, one after another, so two sessions that write at once take turns.
class HubDaemon {
public:
    // The longest line a request may be; a socket that sends more is answered no and closed.
    static constexpr std::size_t longestLine = std::size_t{1024} * 1024;

    HubDaemon(HubKeeper &keeper, const HubRequests &requests) : m_keeper(keeper), m_requests(requests) {}

    // Listens at `socket`: refused when a daemon answers there already, and
    // the file a killed one left there taken over.
    [[nodiscard]] std::expected<void, Failure> listen(const QString &socket);

    // Whether a daemon answers at `socket`, within `wait`.
    [[nodiscard]] static bool answersAt(const QString &socket, std::chrono::milliseconds wait = std::chrono::milliseconds(500));

private:
    void take();
    void serve(QLocalSocket &socket);

    HubKeeper &m_keeper;
    const HubRequests &m_requests;
    QLocalServer m_server;
};
