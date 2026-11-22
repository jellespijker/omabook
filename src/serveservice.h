#pragma once

#include <QString>
#include <QStringList>
#include <expected>
#include <utility>

#include "failure.h"

// Serving e-readers and phones without the window (#177): a systemd user
// service, the person's own and no root's, that runs `omabook serve
// --service` with the settings in config.json, from login on and again
// after a failure (as when the tailnet isn't up yet). The window then
// leaves serving to it. Each call waits for systemctl, so it is made off the
// window's thread.
class ServeService {
public:
    struct Where {
        QString folder;    // where systemd finds the person's units: ~/.config/systemd/user
        QString systemctl; // the program that runs them
        QString omabook;   // the program the service runs
    };
    static constexpr auto unit = "omabook-serve.service";

    // This person's, with systemctl on the path and this very program.
    [[nodiscard]] static Where usual();

    explicit ServeService(Where where) : m_where(std::move(where)) {}

    [[nodiscard]] QString unitPath() const;
    [[nodiscard]] QString unitText() const;
    [[nodiscard]] bool installed() const;
    // Written, and started now and at every login.
    [[nodiscard]] std::expected<void, Failure> install() const;
    // Stopped, and gone.
    [[nodiscard]] std::expected<void, Failure> remove() const;
    // Started again, as the settings it serves with have changed.
    [[nodiscard]] std::expected<void, Failure> restart() const;

private:
    [[nodiscard]] std::expected<void, Failure> run(const QStringList &arguments) const;

    Where m_where;
};
