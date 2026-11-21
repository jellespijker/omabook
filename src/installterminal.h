#pragma once

#include <QObject>
#include <QTimer>
#include <chrono>

#include "voiceinstall.h"

// The floating terminal an install runs in (#156), from when it is started
// until it is closed. Omarchy hands the terminal to the desktop, and its
// command returns before the terminal is even up; so the terminal is taken
// to be closed once this window, which the terminal took the keyboard from,
// has it back. Where it never took it (no terminal came up), a while after
// it was started; where its command couldn't be started, at once. It is
// started on its own, not as this window's child: closing the window leaves
// an install to finish.
class InstallTerminal : public QObject {
    Q_OBJECT

public:
    explicit InstallTerminal(QObject *parent = nullptr);

    // How long a terminal may take to come up and take the keyboard.
    static constexpr std::chrono::seconds coming{20};

    // Whether one is open, or being opened.
    [[nodiscard]] bool open() const noexcept { return m_open; }
    // Opens one with this command; nothing while one is open.
    void run(const VoiceInstall::Command &command);

signals:
    void openChanged();
    // It was closed: what it installed is there now, or isn't.
    void closed();

private:
    void stateChanged(Qt::ApplicationState state);
    void close();

    QTimer m_coming;
    bool m_open = false;
    bool m_away = false;
};
