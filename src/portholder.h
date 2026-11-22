#pragma once

#include <QString>

// Which program listens on a TCP port of this computer (#177), so that "the
// address is in use" can say what uses it: read from Linux's /proc, the
// listening socket in net/tcp and net/tcp6, and the process holding it among
// those this user may look into. Empty when it can't be told (another user's
// program, or no /proc).
namespace PortHolder {

// The program's name, as the system knows it ("python3"); `proc` is /proc, or a test's stand-in.
[[nodiscard]] QString program(quint16 port, const QString &proc = QStringLiteral("/proc"));

} // namespace PortHolder
