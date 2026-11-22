#include "serverreach.h"

#include <QHash>
#include <QTcpSocket>
#include <QUrl>
#include <mutex>

using namespace Qt::StringLiterals;

namespace {

using Clock = std::chrono::steady_clock;

struct Found {
    bool there = false;
    Clock::time_point until;
};

// What each address came to, shared by every thread that asks: a server is
// away for the window and the graph's worker alike.
struct Held {
    std::mutex lock;
    QHash<QString, Found> found;
    std::chrono::milliseconds instead{0};
};

[[nodiscard]] Held &held() {
    static Held all;
    return all;
}

// The host and port a connection goes to: what is the same server for each address.
[[nodiscard]] QString keyOf(const QUrl &url) {
    return url.host().toLower() + u':' + QString::number(url.port(url.scheme() == "https"_L1 ? 443 : 80));
}

[[nodiscard]] bool tryNow(const QUrl &url) {
    QTcpSocket socket;
    socket.connectToHost(url.host(), static_cast<quint16>(url.port(url.scheme() == "https"_L1 ? 443 : 80)));
    const bool there = socket.waitForConnected(static_cast<int>(ServerReach::tryFor.count()));
    socket.abort();
    return there;
}

} // namespace

std::optional<bool> ServerReach::known(const QString &baseUrl) {
    const QUrl url(baseUrl.trimmed());
    if (!url.isValid() || url.host().isEmpty())
        return std::nullopt;
    Held &all = held();
    const std::scoped_lock locked(all.lock);
    const auto found = all.found.constFind(keyOf(url));
    if (found == all.found.cend() || found->until <= Clock::now())
        return std::nullopt;
    return found->there;
}

bool ServerReach::reachable(const QString &baseUrl) {
    const QUrl url(baseUrl.trimmed());
    // Not an address to try: whatever asks it says so itself.
    if (!url.isValid() || url.host().isEmpty())
        return true;
    if (const auto before = known(baseUrl))
        return *before;
    // Tried outside the lock: another server's answer isn't kept waiting by this one.
    const bool there = tryNow(url);
    Held &all = held();
    const std::scoped_lock locked(all.lock);
    const std::chrono::milliseconds hold = all.instead.count() > 0 ? all.instead : std::chrono::milliseconds(there ? heldThere : heldAway);
    all.found.insert(keyOf(url), {.there = there, .until = Clock::now() + hold});
    return there;
}

void ServerReach::forget(const QString &baseUrl) {
    Held &all = held();
    const std::scoped_lock locked(all.lock);
    if (baseUrl.isEmpty())
        all.found.clear();
    else
        all.found.remove(keyOf(QUrl(baseUrl.trimmed())));
}

void ServerReach::holdFor(std::chrono::milliseconds duration) {
    Held &all = held();
    const std::scoped_lock locked(all.lock);
    all.instead = duration.count() > 0 ? duration : std::chrono::milliseconds(0);
    all.found.clear();
}
