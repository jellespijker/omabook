#include "syncwait.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QNetworkInformation>
#include <QObject>

using namespace Qt::StringLiterals;

namespace {

// How long UPower is waited for, so a sync never hangs on a desktop that doesn't answer.
constexpr int answerWithin = 1000;

[[nodiscard]] bool onBattery() {
    const QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected())
        return false;
    QDBusMessage asked = QDBusMessage::createMethodCall(u"org.freedesktop.UPower"_s, u"/org/freedesktop/UPower"_s,
                                                        u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    asked << u"org.freedesktop.UPower"_s << u"OnBattery"_s;
    const QDBusMessage answer = bus.call(asked, QDBus::Block, answerWithin);
    if (answer.type() != QDBusMessage::ReplyMessage || answer.arguments().isEmpty())
        return false;
    return answer.arguments().constFirst().value<QDBusVariant>().variant().toBool();
}

// Qt's network information, loaded once, or null where there is none to load.
[[nodiscard]] QNetworkInformation *network() {
    if (QNetworkInformation::instance() == nullptr)
        QNetworkInformation::loadDefaultBackend();
    return QNetworkInformation::instance();
}

} // namespace

SyncWait::Reason SyncWait::now() {
    if (onBattery())
        return Reason::Battery;
    if (const QNetworkInformation *information = network();
        information != nullptr && information->supports(QNetworkInformation::Feature::Metered) && information->isMetered())
        return Reason::Metered;
    return Reason::None;
}

QString SyncWait::name(Reason reason) {
    switch (reason) {
    case Reason::Battery:
        return u"battery"_s;
    case Reason::Metered:
        return u"metered"_s;
    case Reason::None:
        break;
    }
    return {};
}

void SyncWait::whenOnline(const QObject *context, const std::function<void()> &then) {
    const QNetworkInformation *information = network();
    if (information == nullptr || !information->supports(QNetworkInformation::Feature::Reachability))
        return;
    QObject::connect(information, &QNetworkInformation::reachabilityChanged, context, [then](QNetworkInformation::Reachability now) {
        if (now == QNetworkInformation::Reachability::Online)
            then();
    });
}
