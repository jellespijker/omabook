#pragma once

#include <QObject>
#include <QString>

#include "installterminal.h"

class Settings;

// What writes out speech on this computer, for the settings (#146): the way
// a recording goes first (voxtype, whisper.cpp or a server; through the
// registry), whether it gives each word its moment, or why there is none;
// and, on Omarchy without voxtype, installing it in a terminal, as a voice is
// installed (#156). Once that terminal is closed it looks again.
class SpeechInstall : public QObject {
    Q_OBJECT
    // Where a recording goes first ("voxtype on this computer"); empty when none may be used.
    Q_PROPERTY(QString heardBy READ heardBy NOTIFY changed)
    // Whether that way gives each word its moment.
    Q_PROPERTY(bool timed READ timed NOTIFY changed)
    // Why no way may be used, in plain words.
    Q_PROPERTY(QString why READ why NOTIFY changed)
    // voxtype isn't there, and Omarchy can install it; and whether its terminal is open.
    Q_PROPERTY(bool installable READ installable NOTIFY changed)
    Q_PROPERTY(bool installing READ installing NOTIFY changed)

public:
    explicit SpeechInstall(Settings *settings, QObject *parent = nullptr);

    [[nodiscard]] QString heardBy() const;
    [[nodiscard]] bool timed() const;
    [[nodiscard]] QString why() const;
    [[nodiscard]] bool installable() const;
    [[nodiscard]] bool installing() const noexcept { return m_terminal.open(); }

    // Installs voxtype in a terminal; nothing while one is open, or where it can't.
    Q_INVOKABLE void install();

signals:
    void changed();

private:
    Settings *m_settings;
    InstallTerminal m_terminal;
};
