#pragma once

#include <QString>

// A notification on this computer's desktop (#184), through the desktop's
// notification server (org.freedesktop.Notifications, mako on Omarchy): what
// a command run without a window says that the person has to know of, such
// as a warning of the snapshots. Nothing is shown, and false said, without a
// session bus or a server on it.
namespace DesktopNotice {

[[nodiscard]] bool show(const QString &title, const QString &body);

} // namespace DesktopNotice
