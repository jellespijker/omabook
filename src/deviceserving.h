#pragma once

#include <QHostAddress>
#include <QJsonObject>
#include <QString>
#include <chrono>
#include <cstdint>
#include <optional>

#include "failure.h"

// Which Omabook serves e-readers and phones on a port (#177): the window, its
// background service, or `omabook serve` in a terminal. Each answers who it
// is at one path, to this computer only, so that another one that finds the
// port taken can tell "the window is serving already" from another program,
// and the window can show what its background service is doing.
namespace DeviceServing {

enum class By : std::uint8_t { Window, Service, Command };

// What a server says of itself.
struct Status {
    By by = By::Window;
    bool opds = false;
    bool koreader = false;
    // The devices that asked it for something in the last day.
    int devices = 0;

    [[nodiscard]] QJsonObject toJson() const;
    [[nodiscard]] static std::optional<Status> fromJson(const QJsonObject &object);
    bool operator==(const Status &) const = default;
};

// Where it is asked, and the name Omabook answers it with.
inline constexpr auto path = "/omabook/serving";

[[nodiscard]] QString byName(By by);

// Whether a request came from this computer, as only it may ask who serves.
[[nodiscard]] bool fromHere(const QHostAddress &peer);

// What the Omabook listening on `address` and `port` says of itself; none
// when nothing answers in time, or something that isn't Omabook does. Asked
// on this computer's own address when `address` is every address. It waits,
// so never on the window's thread.
[[nodiscard]] std::optional<Status> ask(const QHostAddress &address, quint16 port,
                                        std::chrono::milliseconds wait = std::chrono::milliseconds(1500));

// Why Omabook can't listen on a port that is taken, naming what took it: an
// Omabook serving already (at `shown`, the address a device is told), or
// another program, by its name when it can be told.
[[nodiscard]] Failure taken(const QHostAddress &address, quint16 port, const QString &shown);

} // namespace DeviceServing
