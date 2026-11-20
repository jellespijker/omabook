#include "scriptsharing.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "library.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "sharedspace.h"
#include "subscribedspace.h"
#include "worldlayers.h"

using namespace Qt::StringLiterals;

namespace {

// Why `plugin` may not put book `id` into the shared space: empty when it may.
[[nodiscard]] QString whyNotAdded(const Library &library, const SharedSpace::State &state, const QString &space, qint64 id,
                                  const QString &plugin) {
    if (!SharedSpace::isShared(library, space))
        return u"%1 is not a shared space this library is in (omabook spaces lists them)."_s.arg(space);
    const std::optional<Book> found = library.book(id);
    if (!found || found->identifiers.value("generated").toString() != plugin)
        return u"Only a book this plugin made goes into a space this way; book %1 isn't one."_s.arg(id);
    if (state.roleOf(SharedSpace::memberAs(library, state)) != SharedSpace::Role::Writer)
        return u"You read %1: only its writers add to it."_s.arg(state.name.isEmpty() ? space : state.name);
    if (SubscribedSpace::linkedCollection(library, space) == 0)
        return u"No collection here is linked to %1."_s.arg(space);
    return {};
}

} // namespace

Library *ScriptSharing::library() {
    Library *library = m_plugin->m_services.library ? m_plugin->m_services.library() : nullptr;
    if (library == nullptr)
        fail(u"There is no library open."_s);
    return library;
}

void ScriptSharing::fail(const QString &why) { m_plugin->m_engine.throwError(why); }

QList<SpaceWorlds::Offer> ScriptSharing::offers(const SpaceWorlds::Asked &asked) {
    if (!m_worlds.isCallable())
        return {};
    QString problem;
    const QJSValue answer = m_plugin->call(m_worlds, {m_plugin->fromJson(asked.toJson())}, &problem);
    const QJSValue json =
        problem.isEmpty() ? m_plugin->call(m_plugin->m_engine.evaluate(u"JSON.stringify"_s), {answer}, &problem) : QJSValue();
    const QJsonDocument document = QJsonDocument::fromJson(json.toString().toUtf8());
    if (problem.isEmpty() && !document.isArray())
        problem = u"worlds should answer a list of {book, scope, file}, not a promise or anything else."_s;
    if (!problem.isEmpty()) {
        m_plugin->m_log << u"warn: sharing worlds: "_s + problem;
        return {};
    }
    QList<SpaceWorlds::Offer> offered;
    for (const auto &one : document.array()) {
        const QJsonObject world = one.toObject();
        offered << SpaceWorlds::Offer{.plugin = m_plugin->m_manifest.id,
                                      .book = world.value("book").toInteger(),
                                      .scope = world.value("scope").toString(),
                                      .file = world.value("file").toBool()};
    }
    return offered;
}

bool ScriptSharing::writing(const Library &library, const QString &scope) {
    if (SpaceWorlds::isLayerName(scope)) {
        fail(u"%1 was shared with you: it can be read and copied, not written to."_s.arg(scope));
        return false;
    }
    SpaceWorlds::madeBy(library, scope, m_plugin->m_manifest.id);
    return true;
}

bool ScriptSharing::copyScope(const Library &library, const QString &from, const QString &to) {
    return writing(library, to) && WorldLayers::copy(library, from, to);
}

void ScriptSharing::registerWorlds(const QJSValue &worlds) { m_worlds = worlds; }

QString ScriptSharing::layers() {
    QJsonArray list;
    if (const Library *library = m_plugin->m_services.library ? m_plugin->m_services.library() : nullptr)
        for (const WorldLayers::Layer &layer : WorldLayers::list(*library, false))
            list << layer.toJson();
    return QString::fromUtf8(QJsonDocument(list).toJson(QJsonDocument::Compact));
}

void ScriptSharing::addToSpace(const QString &space, double book) {
    Library *library = this->library();
    if (library == nullptr)
        return;
    const auto id = static_cast<qint64>(book);
    const SharedSpace::State state = SharedSpace::state(*library, space);
    if (const QString why = whyNotAdded(*library, state, space, id, m_plugin->m_manifest.id); !why.isEmpty()) {
        fail(why);
        return;
    }
    if (!library->putInCollection(SubscribedSpace::linkedCollection(*library, space), id, true)) {
        fail(u"Book %1 could not be put in the collection linked to %2."_s.arg(id).arg(space));
        return;
    }
    const SpaceWorlds::Asked asked{.space = space, .kind = u"shared"_s, .books = {id}, .parts = state.parts};
    for (const SpaceWorlds::Left &left : SpaceWorlds::grant(*library, asked, offers(asked)).left)
        m_plugin->m_log << u"warn: not shared: "_s + left.why;
}

SpaceWorlds::Ask worldsOf(ScriptPlugins &plugins) {
    return [&plugins](const SpaceWorlds::Asked &asked) {
        QList<SpaceWorlds::Offer> offered;
        for (const ScriptPlugins::Info &info : plugins.list())
            if (ScriptPlugin *plugin = plugins.plugin(info.manifest.id))
                offered += plugin->sharing().offers(asked);
        return offered;
    };
}
