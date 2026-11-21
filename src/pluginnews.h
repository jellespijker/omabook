#pragma once

#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QtTypes>
#include <chrono>
#include <functional>
#include <map>
#include <utility>

class Library;

// What of the plugins' data came in from elsewhere (#126): from another
// computer at a sync or a pairing, from a space, or taken in once known
// (KeptOps), whatever brought it. A plugin's data is the operations of its
// own space (PluginData, ADR 0002), and what the log holds of each space
// from each computer is its `seen` mark; whatever lies past a mark looked at
// before is new. This computer's own writes are left out: the plugin made
// them, so they are no news to it.
namespace PluginNews {

// The last number the log holds of each plugin's space from each computer
// but this one, by space and device.
using Marks = std::map<std::pair<QString, QString>, qint64>;
[[nodiscard]] Marks marks(const Library &library);

// What changed, by plugin: of each of its entities (as the plugin names it,
// without its id in front), the ids of the things touched.
using Things = std::map<QString, QSet<QString>>;
using News = std::map<QString, Things>;

// The things the operations past `before` up to `now` touched.
[[nodiscard]] News since(const Library &library, const Marks &before, const Marks &now);
void merge(News &into, const News &more);

// What a plugin is told of its things, `{entity: [id, ...]}`, the ids in order.
[[nodiscard]] QJsonObject toJson(const Things &things);

// Follows the log of the library open now and tells each plugin what came
// for it, once a burst is over: a sync writes for a while, and what it
// brought is told as one. The first look at a library only marks where its
// log is: what was there before was there when the plugins loaded.
class Feed {
public:
    using Tell = std::function<void(const QString &plugin, const QJsonObject &entities)>;
    explicit Feed(Tell tell, std::chrono::milliseconds quiet = std::chrono::milliseconds{300});

    // Looks at `library`'s log now (none: there is no library to follow).
    void look(const Library *library);

private:
    Tell m_tell;
    QString m_root;
    Marks m_marks;
    News m_news;
    QTimer m_quiet;
};

} // namespace PluginNews
