#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <chrono>
#include <functional>

#include "hubautosync.h"
#include "sync.h"
#include "syncwait.h"

// Automatic sync with the window closed (#191): what the background service
// `omabook-hubsync.service` runs (`omabook hub sync --service`), once the person
// allowed it, from login on. It syncs as the window does (HubAutoSync), on the
// timer and when the network is back, here on its own thread, one hub at a
// time; a refusal it is the first to see, it tells the desktop of.
class HubAutoRun : public QObject {
    Q_OBJECT

public:
    struct Given {
        QString folder;                                       // HubAutoSync's
        std::function<QStringList()> hosts;                   // the hubs this computer is linked to
        std::function<SyncSummary(const QString &host)> sync; // the library in use synced with one
        std::function<QDateTime()> now = [] { return QDateTime::currentDateTimeUtc(); };
        std::function<SyncWait::Reason()> wait = SyncWait::now;
        std::function<void(const QString &title, const QString &body)> notify;
        std::chrono::milliseconds every = std::chrono::seconds(30); // how often it looks
    };

    explicit HubAutoRun(Given given, QObject *parent = nullptr);

    // Looks every so often, and when the network is back, from now on.
    void start();
    // Syncs with the hub that is due, if one is and nothing says to wait.
    void tick();

private:
    Given m_given;
    HubAutoSync m_auto;
    QTimer m_timer;
};
