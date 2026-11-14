#include "secretstore.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusVariant>
#include <QEventLoop>
#include <QMap>
#include <QObject>
#include <QTimer>

namespace {

constexpr auto service = "org.freedesktop.secrets";
constexpr auto servicePath = "/org/freedesktop/secrets";
constexpr auto defaultCollection = "/org/freedesktop/secrets/aliases/default";

// A secret as the Secret Service passes it: the session it is sent under,
// the parameters of its encryption (none: the bus is local), the bytes, and
// what kind of thing they are.
struct Sealed {
    QDBusObjectPath session;
    QByteArray parameters;
    QByteArray value;
    QString contentType;
};

QDBusArgument &operator<<(QDBusArgument &argument, const Sealed &sealed) {
    argument.beginStructure();
    argument << sealed.session << sealed.parameters << sealed.value << sealed.contentType;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, Sealed &sealed) {
    argument.beginStructure();
    argument >> sealed.session >> sealed.parameters >> sealed.value >> sealed.contentType;
    argument.endStructure();
    return argument;
}

using Attributes = QMap<QString, QString>;

Attributes attributesFor(const QString &name) { return {{"application", "omabook"}, {"secret", name}}; }

QDBusMessage call(const QString &path, const char *interface, const char *method, const QVariantList &arguments = {},
                  int timeout = 8000) {
    QDBusMessage message = QDBusMessage::createMethodCall(service, path, interface, method);
    message.setArguments(arguments);
    return QDBusConnection::sessionBus().call(message, QDBus::Block, timeout);
}

bool worked(const QDBusMessage &reply) { return reply.type() == QDBusMessage::ReplyMessage; }

// Catches a prompt's answer: the keyring asks for its password in a window
// of its own, and says when that window is done with.
class PromptWait : public QObject {
    Q_OBJECT

public:
    QEventLoop loop;
    bool dismissed = true;

public slots:
    void completed(bool wasDismissed, const QDBusVariant &) {
        dismissed = wasDismissed;
        loop.quit();
    }
};

// Shows a prompt the service asked for (to unlock the keyring, say) and
// waits for it; true when there was none or it was answered.
bool prompted(const QDBusObjectPath &prompt) {
    if (prompt.path().size() <= 1)
        return true;
    PromptWait wait;
    QDBusConnection::sessionBus().connect(service, prompt.path(), "org.freedesktop.Secret.Prompt", "Completed", &wait,
                                          SLOT(completed(bool, QDBusVariant)));
    if (!worked(call(prompt.path(), "org.freedesktop.Secret.Prompt", "Prompt", {QString()})))
        return false;
    // Someone who walked away from the prompt doesn't hold the app for ever.
    QTimer::singleShot(120000, &wait.loop, &QEventLoop::quit);
    wait.loop.exec();
    return !wait.dismissed;
}

} // namespace

Q_DECLARE_METATYPE(Sealed)

bool SessionSecrets::lasting(QString *why) const {
    if (why)
        *why = "Secrets are kept only until Omabook closes.";
    return false;
}

bool SessionSecrets::put(const QString &name, const QString &secret, QString *) {
    m_secrets.insert(name, secret);
    return true;
}

bool SessionSecrets::forget(const QString &name) {
    m_secrets.remove(name);
    return true;
}

struct KeyringSecrets::Private {
    // Asked once: a bus without a keyring doesn't grow one while we run.
    mutable bool asked = false;
    mutable bool there = false;
    mutable QString session;
    // What a desktop without a keyring is given instead.
    SessionSecrets fallback;

    bool open() const {
        if (asked)
            return there;
        asked = true;
        qDBusRegisterMetaType<Sealed>();
        qDBusRegisterMetaType<Attributes>();
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            return false;
        // "plain": the secret crosses the session bus as it is, which only
        // this user's own programs can listen on.
        const QDBusMessage reply = call(servicePath, "org.freedesktop.Secret.Service", "OpenSession",
                                        {QString("plain"), QVariant::fromValue(QDBusVariant(QString()))});
        if (!worked(reply) || reply.arguments().size() < 2)
            return false;
        session = reply.arguments().at(1).value<QDBusObjectPath>().path();
        there = !session.isEmpty();
        return there;
    }

    // The items kept under a name, unlocked first when they are locked.
    QList<QDBusObjectPath> items(const QString &name) const {
        const QDBusMessage found = call(servicePath, "org.freedesktop.Secret.Service", "SearchItems",
                                        {QVariant::fromValue(attributesFor(name))});
        if (!worked(found) || found.arguments().size() < 2)
            return {};
        QList<QDBusObjectPath> unlocked = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(0));
        const QList<QDBusObjectPath> locked = qdbus_cast<QList<QDBusObjectPath>>(found.arguments().at(1));
        if (unlocked.isEmpty() && !locked.isEmpty()) {
            const QDBusMessage opened = call(servicePath, "org.freedesktop.Secret.Service", "Unlock", {QVariant::fromValue(locked)});
            if (worked(opened) && opened.arguments().size() == 2) {
                unlocked = qdbus_cast<QList<QDBusObjectPath>>(opened.arguments().at(0));
                if (unlocked.isEmpty() && prompted(opened.arguments().at(1).value<QDBusObjectPath>()))
                    unlocked = locked;
            }
        }
        return unlocked;
    }
};

KeyringSecrets::KeyringSecrets() : d(std::make_unique<Private>()) {}
KeyringSecrets::~KeyringSecrets() = default;

bool KeyringSecrets::lasting(QString *why) const {
    if (d->open())
        return true;
    if (why)
        *why = "This desktop has no keyring running (the Secret Service), so tokens are kept only until Omabook closes "
               "and are asked for again next time. Install and start gnome-keyring to keep them.";
    return false;
}

QString KeyringSecrets::get(const QString &name) const {
    if (!d->open())
        return d->fallback.get(name);
    for (const QDBusObjectPath &item : d->items(name)) {
        const QDBusMessage reply = call(item.path(), "org.freedesktop.Secret.Item", "GetSecret",
                                        {QVariant::fromValue(QDBusObjectPath(d->session))});
        if (worked(reply) && !reply.arguments().isEmpty())
            return QString::fromUtf8(qdbus_cast<Sealed>(reply.arguments().first()).value);
    }
    return {};
}

bool KeyringSecrets::put(const QString &name, const QString &secret, QString *error) {
    if (!d->open())
        return d->fallback.put(name, secret, error);
    const QVariantMap properties{
        {"org.freedesktop.Secret.Item.Label", "Omabook: " + name},
        {"org.freedesktop.Secret.Item.Attributes", QVariant::fromValue(attributesFor(name))},
    };
    const Sealed sealed{QDBusObjectPath(d->session), {}, secret.toUtf8(), "text/plain; charset=utf8"};
    // "replace": one item per name, however often it is set.
    QDBusMessage reply = call(defaultCollection, "org.freedesktop.Secret.Collection", "CreateItem",
                              {properties, QVariant::fromValue(sealed), true});
    if (worked(reply) && reply.arguments().size() == 2) {
        const QDBusObjectPath prompt = reply.arguments().at(1).value<QDBusObjectPath>();
        // A locked keyring answers with a prompt instead of an item.
        if (reply.arguments().at(0).value<QDBusObjectPath>().path().size() > 1 || prompted(prompt))
            return true;
    }
    if (error)
        *error = "The keyring wouldn't take it" + (reply.errorMessage().isEmpty() ? QString(".") : ": " + reply.errorMessage());
    return false;
}

bool KeyringSecrets::forget(const QString &name) {
    if (!d->open())
        return d->fallback.forget(name);
    bool gone = true;
    for (const QDBusObjectPath &item : d->items(name)) {
        const QDBusMessage reply = call(item.path(), "org.freedesktop.Secret.Item", "Delete");
        gone = gone && worked(reply) && (reply.arguments().isEmpty() || prompted(reply.arguments().first().value<QDBusObjectPath>()));
    }
    return gone;
}

namespace {
SecretStore *replacement = nullptr;
}

SecretStore &Secrets::shared() {
    static KeyringSecrets keyring;
    return replacement ? *replacement : static_cast<SecretStore &>(keyring);
}

void Secrets::useInstead(SecretStore *store) { replacement = store; }

#include "secretstore.moc"
