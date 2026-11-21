#include "installterminal.h"

#include <QGuiApplication>
#include <QProcess>

InstallTerminal::InstallTerminal(QObject *parent) : QObject(parent) {
    m_coming.setSingleShot(true);
    m_coming.setInterval(coming);
    connect(&m_coming, &QTimer::timeout, this, [this] {
        if (m_open && !m_away)
            close();
    });
    if (const auto *application = qobject_cast<const QGuiApplication *>(QCoreApplication::instance()))
        connect(application, &QGuiApplication::applicationStateChanged, this, &InstallTerminal::stateChanged);
}

void InstallTerminal::run(const VoiceInstall::Command &command) {
    if (m_open)
        return;
    m_open = true;
    m_away = false;
    emit openChanged();
    if (!QProcess::startDetached(command.program, command.arguments)) {
        close();
        return;
    }
    m_coming.start();
}

void InstallTerminal::stateChanged(Qt::ApplicationState state) {
    if (!m_open)
        return;
    if (state != Qt::ApplicationActive)
        m_away = true;
    else if (m_away)
        close();
}

void InstallTerminal::close() {
    m_coming.stop();
    m_open = false;
    m_away = false;
    emit openChanged();
    emit closed();
}
