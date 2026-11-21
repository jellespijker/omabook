#include "pluginnews.h"

#include <QJsonArray>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>
#include <algorithm>
#include <utility>

#include "changelog.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto spacePrefix = "plugin:"_L1;

} // namespace

PluginNews::Marks PluginNews::marks(const Library &library) {
    Marks marks;
    // The plugins' spaces are a range of names: "plugin;" is the first name after them.
    for (QSqlQuery query = ChangeLog::query(library,
                                            u"SELECT s.name, d.name, n.seq FROM seen AS n JOIN log_ids AS s ON s.id = n.space "
                                            "JOIN log_ids AS d ON d.id = n.device "
                                            "WHERE s.name >= 'plugin:' AND s.name < 'plugin;' AND d.name <> ?"_s,
                                            {Spaces::deviceId(library)});
         query.next();)
        marks[{query.value(0).toString(), query.value(1).toString()}] = query.value(2).toLongLong();
    return marks;
}

PluginNews::News PluginNews::since(const Library &library, const Marks &before, const Marks &now) {
    News news;
    for (const auto &[where, last] : now) {
        const auto was = before.find(where);
        const qint64 from = was == before.end() ? 0 : was->second;
        if (last <= from)
            continue;
        const QString plugin = where.first.mid(spacePrefix.size());
        const QString own = plugin + u':';
        // Of what compaction took into the snapshot as of what is still in `ops`.
        for (QSqlQuery query = ChangeLog::query(
                 library,
                 u"SELECT DISTINCT o.entity, i.name FROM space_ops AS o JOIN log_ids AS i ON i.id = o.id "
                 "WHERE o.space = (SELECT id FROM log_ids WHERE name = ?) AND o.device = (SELECT id FROM log_ids WHERE name = ?) "
                 "AND o.seq > ? AND o.seq <= ?"_s,
                 {where.first, where.second, from, last});
             query.next();) {
            const QString entity = query.value(0).toString();
            // Another plugin's entity is never taken in (PluginData::whyRefused), nor told of.
            if (entity.startsWith(own) && entity.size() > own.size())
                news[plugin][entity.mid(own.size())].insert(query.value(1).toString());
        }
    }
    return news;
}

void PluginNews::merge(News &into, const News &more) {
    for (const auto &[plugin, things] : more)
        for (const auto &[entity, ids] : things)
            into[plugin][entity].unite(ids);
}

QJsonObject PluginNews::toJson(const Things &things) {
    QJsonObject entities;
    for (const auto &[entity, ids] : things) {
        QStringList ordered(ids.cbegin(), ids.cend());
        std::ranges::sort(ordered);
        entities.insert(entity, QJsonArray::fromStringList(ordered));
    }
    return entities;
}

PluginNews::Feed::Feed(Tell tell, std::chrono::milliseconds quiet) : m_tell(std::move(tell)) {
    m_quiet.setSingleShot(true);
    m_quiet.setInterval(quiet);
    QObject::connect(&m_quiet, &QTimer::timeout, &m_quiet, [this] {
        for (const auto &[plugin, things] : std::exchange(m_news, {}))
            m_tell(plugin, toJson(things));
    });
}

void PluginNews::Feed::look(const Library *library) {
    const QString root = library ? library->root() : QString();
    if (root != m_root) {
        // Another library: what was found of the one before is no news of this one.
        m_root = root;
        m_marks = library ? marks(*library) : Marks();
        m_news.clear();
        return;
    }
    if (!library)
        return;
    Marks now = marks(*library);
    if (now == m_marks)
        return;
    merge(m_news, since(*library, m_marks, now));
    m_marks = std::move(now);
    if (!m_news.empty())
        m_quiet.start();
}
