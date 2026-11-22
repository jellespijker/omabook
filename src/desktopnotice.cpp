#include "desktopnotice.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QStringList>
#include <QVariantMap>

using namespace Qt::StringLiterals;

namespace {

// How long the server is waited for, so a command never hangs on a desktop that doesn't answer.
constexpr int answerWithin = 2000;

} // namespace

bool DesktopNotice::show(const QString &title, const QString &body) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return false;
    QDBusMessage notify = QDBusMessage::createMethodCall(u"org.freedesktop.Notifications"_s, u"/org/freedesktop/Notifications"_s,
                                                         u"org.freedesktop.Notifications"_s, u"Notify"_s);
    // The application, none replaced, its icon, the summary, the body, no actions, no hints, the server's own time out.
    notify << u"Omabook"_s << 0U << u"omabook"_s << title << body << QStringList() << QVariantMap() << -1;
    return bus.call(notify, QDBus::Block, answerWithin).type() == QDBusMessage::ReplyMessage;
}
