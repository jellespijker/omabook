#include "scriptgraph.h"

#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <expected>
#include <memory>
#include <utility>

#include "aicall.h"
#include "graphlinks.h"
#include "library.h"
#include "scriptplugin.h"
#include "scriptsharing.h"
#include "tenancy.h"
#include "worldlayers.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString compact(const QJsonValue &value) {
    // A document is an object or an array, so anything else rides in one.
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(wrapped.mid(1, wrapped.size() - 2));
}

[[nodiscard]] QJsonObject object(const QString &json) { return QJsonDocument::fromJson(json.toUtf8()).object(); }

[[nodiscard]] qint64 number(double value) { return static_cast<qint64>(value); }

// The tenant a plugin's find reads the general graph in: the one it names (a
// collection, "unfiled" or a tenant's global id, as Tenancy::chosen takes
// it), else the tenant of the book it names, else none, which is
// Tenancy::preferred's.
[[nodiscard]] std::expected<std::optional<Tenant>, QString> tenantOf(const Library &library, const QJsonObject &options) {
    if (const QString named = options.value("tenant").toString(); !named.trimmed().isEmpty())
        return Tenancy::chosen(library, named).transform([](Tenant tenant) { return std::optional(std::move(tenant)); });
    if (const qint64 book = options.value("book").toInteger(); book > 0)
        return std::optional(Tenancy::ofReader(library, book));
    return std::optional<Tenant>();
}

} // namespace

Library *ScriptGraph::library(bool changing) {
    Library *library = m_plugin->m_services.library ? m_plugin->m_services.library() : nullptr;
    if (!library && changing)
        fail(u"There is no library open."_s);
    return library;
}

void ScriptGraph::fail(const QString &why) { m_plugin->m_engine.throwError(why); }

std::optional<GraphPlace> ScriptGraph::place(qint64 bookId, const QJsonObject &at) {
    if (bookId <= 0 || !at.contains("section"))
        return std::nullopt;
    return GraphPlace{bookId, BookPlace::fromJson(at, {.section = 0, .fraction = 1.0})};
}

QString ScriptGraph::find(const QString &words, const QString &options) {
    const Library *library = this->library(false);
    if (!library)
        return u"[]"_s;
    const QJsonObject given = object(options);
    const auto tenant = tenantOf(*library, given);
    if (!tenant) {
        fail(tenant.error());
        return u"[]"_s;
    }
    return compact(WorldLayers::find(*library, {.words = words,
                                                .kind = given.value("kind").toString(),
                                                .scope = given.value("scope").toString(),
                                                .everywhere = given.value("everywhere").toBool(),
                                                .limit = given.value("limit").toInt(50),
                                                .tenant = *tenant}));
}

QString ScriptGraph::node(double id) {
    const Library *library = this->library(false);
    const auto found = library ? Graph::ofThing(*library, number(id)).node(number(id)) : std::nullopt;
    return found ? compact(found->toJson()) : u"null"_s;
}

QString ScriptGraph::around(double id, double depth) {
    const Library *library = this->library(false);
    return compact(library ? Graph::ofThing(*library, number(id)).around(number(id), static_cast<int>(depth)).toJson()
                           : GraphView().toJson());
}

QString ScriptGraph::ofBook(double id, const QString &upTo) {
    const Library *library = this->library(false);
    return compact(library ? Graph::ofReader(*library, number(id)).ofBook(number(id), place(number(id), object(upTo))).toJson()
                           : GraphView().toJson());
}

QString ScriptGraph::beyond(double id, const QString &upTo) {
    const Library *library = this->library(false);
    return compact(library ? Graph::ofReader(*library, number(id)).beyond(number(id), place(number(id), object(upTo))).toJson()
                           : GraphView().toJson());
}

void ScriptGraph::connectBook(double id, const QJSValue &resolve, const QJSValue &reject) {
    const Library *library = this->library(true);
    const qint64 bookId = number(id);
    const auto at = library ? GraphBuild::builtTo(*library, bookId) : std::nullopt;
    const GraphLinks::Offer offer = at ? GraphLinks::offer(*library, bookId, *at) : GraphLinks::Offer();
    // Nothing to compare, or the same comparison as before: nothing is sent.
    if (!library || !at || !GraphLinks::due(*library, bookId)) {
        resolve.call({0});
        return;
    }
    QString why = u"No AI is set up."_s;
    std::unique_ptr<AiProvider> provider = m_plugin->m_services.ai ? m_plugin->m_services.ai(&why) : nullptr;
    if (!provider) {
        reject.call({why});
        return;
    }
    auto *asking = new AiCall(std::move(provider), m_plugin);
    m_plugin->busy(1);
    QObject::connect(asking, &AiCall::done, m_plugin, [this, asking, resolve, bookId, at, offer](const QString &answer) {
        int added = 0;
        if (Library *now = this->library(false)) {
            const GraphLinks::Applied applied = GraphLinks::apply(*now, bookId, *at, offer, answer);
            GraphLinks::setConnected(*now, bookId, *at, offer);
            added = applied.links + applied.topics;
        }
        resolve.call({added});
        asking->deleteLater();
        m_plugin->busy(-1);
    });
    QObject::connect(asking, &AiCall::failed, m_plugin, [this, asking, reject](const Failure &problem) {
        reject.call({problem.text()});
        asking->deleteLater();
        m_plugin->busy(-1);
    });
    asking->ask(GraphLinks::request(*library, bookId, offer));
}

QString ScriptGraph::whole(const QString &scope, double limit) {
    const Library *library = this->library(false);
    return compact(library ? WorldLayers::whole(Graph::ofScope(*library, scope), scope, qMax(1, static_cast<int>(limit))).toJson()
                           : GraphView().toJson());
}

double ScriptGraph::bookNode(double id) {
    const Library *library = this->library(true);
    if (!library)
        return 0;
    if (!library->book(number(id))) {
        fail(u"There is no book %1 in the library."_s.arg(number(id)));
        return 0;
    }
    return static_cast<double>(Graph(*library).bookNode(number(id)));
}

bool ScriptGraph::unsee(double id, double book) {
    const Library *library = this->library(true);
    return library && Graph::ofReader(*library, number(book)).unsee(number(id), number(book));
}

bool ScriptGraph::unlink(double edge) {
    const Library *library = this->library(true);
    return library
           && Graph::forTenant(*library, Tenancy::ofEdge(*library, number(edge)).value(0, Tenancy::preferred(*library)))
                  .unlink(number(edge));
}

bool ScriptGraph::merge(double keep, double other) {
    const Library *library = this->library(true);
    return library && WorldLayers::merge(*library, number(keep), number(other));
}

bool ScriptGraph::remove(double id) {
    const Library *library = this->library(true);
    return library && Graph::ofThing(*library, number(id)).remove(number(id));
}

QString ScriptGraph::scopes() {
    const Library *library = this->library(false);
    return compact(QJsonArray::fromStringList(library ? WorldLayers::scopes(*library) : QStringList()));
}

bool ScriptGraph::copyScope(const QString &from, const QString &to) {
    const Library *library = this->library(true);
    return library && m_plugin->sharing().copyScope(*library, from, to);
}

bool ScriptGraph::removeScope(const QString &scope) {
    const Library *library = this->library(true);
    return library && Graph(*library).removeScope(scope);
}
