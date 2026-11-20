#include "spaceworlds.h"

#include <QCryptographicHash>
#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <QSqlQuery>
#include <QVariant>
#include <algorithm>
#include <optional>
#include <tuple>

#include "changelog.h"
#include "library.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool scopeIsThere(const Library &library, const QString &scope) {
    return ChangeLog::query(library, "SELECT 1 FROM graph_nodes WHERE scope = ? LIMIT 1", {scope}).next();
}

[[nodiscard]] QJsonObject offerJson(const SpaceWorlds::Offer &offer) {
    return {{"plugin", offer.plugin}, {"book", offer.book}, {"scope", offer.scope}, {"file", offer.file}};
}

// Why the space may not have this of a plugin's, or nothing when it may.
[[nodiscard]] std::optional<QString> refusal(const Library &library, const SpaceWorlds::Asked &asked, const SpaceWorlds::Offer &offer) {
    if (!asked.books.contains(offer.book))
        return u"Book %1 is not in what is shared."_s.arg(offer.book);
    const std::optional<Book> book = library.book(offer.book);
    if (!book)
        return u"There is no book %1."_s.arg(offer.book);
    if (offer.plugin.isEmpty() || book->identifiers.value("generated").toString() != offer.plugin)
        return u"%1 was not made by %2: a book by a person never goes as a world."_s.arg(book->meta.title, offer.plugin);
    if (offer.scope.isEmpty())
        return offer.file ? std::nullopt : std::optional(u"Nothing of %1 was asked for."_s.arg(book->meta.title));
    if (!asked.parts.contains("graph"_L1))
        return u"Its world goes with the graph, which this space doesn't take."_s;
    if (!scopeIsThere(library, offer.scope) || SpaceWorlds::maker(library, offer.scope) != offer.plugin)
        return u"%1 is not a scope %2 made here."_s.arg(offer.scope, offer.plugin);
    return std::nullopt;
}

// When this world was given before, or now when it wasn't.
[[nodiscard]] qint64 givenAt(const Library &library, const QString &space, const QString &book, const SpaceWorlds::Offer &offer) {
    QSqlQuery before =
        ChangeLog::query(library, "SELECT hlc FROM space_worlds WHERE space = ? AND book = ? AND scope = ? AND plugin = ? AND file = ?",
                         {space, book, offer.scope, offer.plugin, offer.file ? 1 : 0});
    return before.next() ? before.value(0).toLongLong() : Spaces::tickHlc(library);
}

[[nodiscard]] QString worldId(const QString &book, const QString &scope) {
    const QByteArray hash = QCryptographicHash::hash((book + u'\n' + scope).toUtf8(), QCryptographicHash::Sha256);
    return u"w:"_s + QString::fromLatin1(hash.toHex().left(32));
}

} // namespace

void SpaceWorlds::madeBy(const Library &library, const QString &scope, const QString &plugin) {
    if (scope.isEmpty() || plugin.isEmpty() || isLayerName(scope) || scopeIsThere(library, scope))
        return;
    ChangeLog::exec(library, "INSERT OR REPLACE INTO plugin_scopes (scope, plugin) VALUES (?, ?)", {scope, plugin});
}

QString SpaceWorlds::maker(const Library &library, const QString &scope) {
    QSqlQuery query = ChangeLog::query(library, "SELECT plugin FROM plugin_scopes WHERE scope = ?", {scope});
    return query.next() ? query.value(0).toString() : QString();
}

QJsonObject SpaceWorlds::Asked::toJson() const {
    QJsonArray ids;
    for (const qint64 book : books)
        ids << book;
    return {{"space", space}, {"kind", kind}, {"books", ids}, {"parts", QJsonArray::fromStringList(parts)}};
}

QJsonObject SpaceWorlds::Granted::toJson() const {
    QJsonArray given;
    QJsonArray notGiven;
    for (const Offer &offer : worlds)
        given << offerJson(offer);
    for (const Left &one : left) {
        QJsonObject object = offerJson(one.offer);
        object.insert("why", one.why);
        notGiven << object;
    }
    return {{"worlds", given}, {"left", notGiven}};
}

SpaceWorlds::Granted SpaceWorlds::grant(const Library &library, const Asked &asked, std::span<const Offer> offered) {
    Granted granted;
    QSet<std::pair<qint64, QString>> seen;
    for (Offer offer : offered) {
        // Without the graph its world stays home, and the book's file it asks for still goes.
        if (!asked.parts.contains("graph"_L1) && offer.file)
            offer.scope.clear();
        if (seen.contains({offer.book, offer.scope}))
            continue;
        seen.insert({offer.book, offer.scope});
        if (const std::optional<QString> why = refusal(library, asked, offer))
            granted.left << Left{.offer = offer, .why = *why};
        else
            granted.worlds << offer;
    }
    QList<std::tuple<QString, Offer, qint64>> rows;
    for (const Offer &offer : std::as_const(granted.worlds)) {
        const QString book = Spaces::bookId(library, offer.book);
        rows.append({book, offer, givenAt(library, asked.space, book, offer)});
    }
    const Library::Batch batch(library);
    for (const qint64 book : asked.books)
        ChangeLog::exec(library, "DELETE FROM space_worlds WHERE space = ? AND book = ?", {asked.space, Spaces::bookId(library, book)});
    for (const auto &[book, offer, hlc] : std::as_const(rows))
        ChangeLog::exec(library, "INSERT OR REPLACE INTO space_worlds (space, book, scope, plugin, file, hlc) VALUES (?, ?, ?, ?, ?, ?)",
                        {asked.space, book, offer.scope, offer.plugin, offer.file ? 1 : 0, hlc});
    return granted;
}

SpaceWorlds::Granted SpaceWorlds::ask(const Library &library, const Asked &asked, const Ask &ask) {
    if (!ask)
        return {};
    const QList<Offer> offered = ask(asked);
    return grant(library, asked, offered);
}

QList<SpaceWorlds::World> SpaceWorlds::granted(const Library &library, const QString &space, bool graph) {
    QList<World> worlds;
    for (QSqlQuery query = ChangeLog::query(
             library, "SELECT book, scope, plugin, file, hlc FROM space_worlds WHERE space = ? ORDER BY book, scope", {space});
         query.next();)
        worlds << World{.book = query.value(0).toString(),
                        .scope = query.value(1).toString(),
                        .plugin = query.value(2).toString(),
                        .file = query.value(3).toBool(),
                        .hlc = query.value(4).toLongLong()};
    // Asked again of each time the space is written: a book or a scope may
    // have changed hands since.
    worlds.removeIf([&library](const World &world) {
        const std::optional<Book> book = library.book(Spaces::localId(library, u"book"_s, world.book));
        return !book || book->identifiers.value("generated").toString() != world.plugin
               || (!world.scope.isEmpty() && maker(library, world.scope) != world.plugin);
    });
    if (!graph) {
        worlds.removeIf([](const World &world) { return !world.file; });
        for (World &world : worlds)
            world.scope.clear();
    }
    return worlds;
}

QStringList SpaceWorlds::scopesOf(std::span<const World> worlds) {
    QStringList scopes;
    for (const World &world : worlds)
        if (!world.scope.isEmpty())
            scopes << world.scope;
    return scopes;
}

QList<SpaceOperation> SpaceWorlds::said(std::span<const World> worlds, const QString &space, const QString &device) {
    QList<SpaceOperation> ops;
    for (const World &world : worlds) {
        const QString id = worldId(world.book, world.scope);
        for (const auto &[field, value] : {std::pair{u"scope"_s, world.scope},
                                           {u"plugin"_s, world.plugin},
                                           {u"book"_s, world.book},
                                           {u"file"_s, world.file ? u"1"_s : u"0"_s}})
            ops << SpaceOperation{.space = space,
                                  .device = device,
                                  .seq = 0,
                                  .hlc = world.hlc,
                                  .author = device,
                                  .entity = u"world"_s,
                                  .id = id,
                                  .field = field,
                                  .value = value};
    }
    return ops;
}

QList<SpaceWorlds::Declared> SpaceWorlds::declared(std::span<const SpaceOperation> ops) {
    struct Saying {
        qint64 alive = -1, removed = -1;
        QHash<QString, const SpaceOperation *> fields; // the latest of each
    };
    QHash<QString, Saying> worlds;
    for (const SpaceOperation &op : ops) {
        if (op.entity != "world"_L1)
            continue;
        Saying &world = worlds[op.id];
        if (op.field == "removed"_L1) {
            if (op.value == "1"_L1)
                world.removed = std::max(world.removed, op.hlc);
            continue;
        }
        world.alive = std::max(world.alive, op.hlc);
        const SpaceOperation *&latest = world.fields[op.field];
        if (latest == nullptr || std::tie(op.hlc, op.device) > std::tie(latest->hlc, latest->device))
            latest = &op;
    }
    QList<Declared> found;
    for (auto world = worlds.cbegin(); world != worlds.cend(); ++world) {
        const auto value = [&world](const QString &field) {
            const SpaceOperation *op = world->fields.value(field);
            return op != nullptr ? op->value : QString();
        };
        const SpaceOperation *scope = world->fields.value(u"scope"_s);
        if (scope == nullptr || (world->removed >= 0 && world->removed >= world->alive))
            continue;
        found << Declared{.id = world.key(),
                          .scope = scope->value,
                          .plugin = value(u"plugin"_s),
                          .book = value(u"book"_s),
                          .author = scope->signer.isEmpty() ? scope->author : scope->signer,
                          .file = value(u"file"_s) == "1"_L1};
    }
    std::ranges::sort(found, [](const Declared &one, const Declared &other) { return one.id < other.id; });
    return found;
}

QString SpaceWorlds::layerName(const QString &space, const QString &scope) { return space + u'/' + scope; }

bool SpaceWorlds::isLayerName(QStringView scope) {
    return (scope.startsWith("published:"_L1) || scope.startsWith("shared:"_L1)) && scope.contains(u'/');
}
