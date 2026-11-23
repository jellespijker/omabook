#pragma once

#include <QString>
#include <functional>

class QObject;

// Whether automatic sync with a hub waits (#191): on battery, as UPower says
// on the system bus (its OnBattery), or on a metered connection, as
// NetworkManager says through Qt's network information (isMetered). Where the
// desktop says neither (no UPower, no backend that knows of metered
// connections), it doesn't wait.
namespace SyncWait {

enum class Reason { None, Battery, Metered };

// Why to wait now, asked of the desktop; each answer within a second.
[[nodiscard]] Reason now();
// "battery", "metered", or empty: as the window's Hubs panel has it.
[[nodiscard]] QString name(Reason reason);
// Calls `then` on `context`'s thread each time the network is back online,
// as Qt's network information says it; never where it can't say.
void whenOnline(const QObject *context, const std::function<void()> &then);

} // namespace SyncWait
