#include "graphtenants.h"

#include <QHash>
#include <algorithm>
#include <ranges>
#include <utility>

#include "backend.h"
#include "settings.h"
#include "spaces.h"
#include "tenancy.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString idOf(const QVariant &tenant) { return tenant.toMap().value(u"id"_s).toString(); }

// The tenants of a library as the window lists them (see GraphTenants::tenants).
[[nodiscard]] QVariantList listed(const Library &library, const QString &libraryName) {
    const QHash<QString, qint64> books = Tenancy::bookCounts(library);
    if (Tenancy::oneTenant()) {
        const QString whole = Tenant::wholeLibrary().globalId();
        return {QVariantMap{
            {u"id"_s, whole}, {u"name"_s, libraryName}, {u"path"_s, QString()}, {u"books"_s, books.value(whole)}, {u"unfiled"_s, false}}};
    }
    QVariantList list;
    for (const Collection &collection : library.collections()) {
        if (collection.depth != 0)
            continue;
        const QString id = Spaces::collectionId(library, collection.id);
        list << QVariantMap{{u"id"_s, id},
                            {u"name"_s, collection.name},
                            {u"path"_s, collection.path},
                            {u"books"_s, books.value(id)},
                            {u"unfiled"_s, false}};
    }
    // Unfiled is a tenant only while a book is in it: once every book is
    // in a collection, there is nothing to show there.
    const QString unfiled = Tenant::unfiled().globalId();
    if (books.value(unfiled) > 0 || list.isEmpty())
        list << QVariantMap{
            {u"id"_s, unfiled}, {u"name"_s, QString()}, {u"path"_s, QString()}, {u"books"_s, books.value(unfiled)}, {u"unfiled"_s, true}};
    return list;
}

} // namespace

GraphTenants::GraphTenants(QObject *parent) : QObject(parent) {}

QObject *GraphTenants::backend() const { return m_backend; }

void GraphTenants::setBackend(QObject *backend) {
    auto *given = qobject_cast<Backend *>(backend);
    if (given == m_backend)
        return;
    if (m_backend) {
        m_backend->disconnect(this);
        m_backend->settings()->disconnect(this);
    }
    m_backend = given;
    m_current.clear();
    if (m_backend) {
        // A collection made, renamed or removed at the top, or a book put in one.
        connect(m_backend, &Backend::collectionsChanged, this, &GraphTenants::refresh);
        // Another library has tenants of its own, and remembers its own.
        connect(m_backend, &Backend::libraryChanged, this, [this] {
            m_current.clear();
            refresh();
        });
        // The choice of one tenant for the whole library, made in the settings.
        connect(m_backend->settings(), &Settings::changed, this, [this] {
            if (Tenancy::oneTenant() != m_one)
                refresh();
        });
        connect(m_backend, &QObject::destroyed, this, [this] { m_backend = nullptr; });
    }
    emit backendChanged();
    refresh();
}

void GraphTenants::refresh() {
    const Library *library = m_backend ? m_backend->library() : nullptr;
    // One tenant for the whole library chosen, or no longer (in the
    // settings, or in config.json by hand): the books' tenants follow it
    // first, so the list counts them where they now are.
    const bool one = Tenancy::oneTenant();
    if (library && one != m_one)
        Tenancy::follow(*library);
    m_one = one;
    const QVariantList list = library ? listed(*library, m_backend->libraryName()) : QVariantList();
    const auto has = [&list](const QString &id) { return std::ranges::contains(list, id, idOf); };
    // The one shown stays; one that is gone (its collection removed, or
    // the library made one tenant) gives way to the one remembered, else
    // to the first.
    QString current = m_current;
    if (!has(current))
        current = library ? m_backend->settings()->graphTenant(library->root()) : QString();
    if (!has(current))
        current = list.isEmpty() ? QString() : idOf(list.first());
    if (list == m_tenants && current == m_current)
        return;
    m_tenants = list;
    m_current = std::move(current);
    emit changed();
}

QVariantMap GraphTenants::shown() const {
    const auto found = std::ranges::find(m_tenants, m_current, idOf);
    return found == m_tenants.cend() ? QVariantMap() : found->toMap();
}

void GraphTenants::choose(const QString &tenant) {
    if (tenant == m_current || !std::ranges::contains(m_tenants, tenant, idOf))
        return;
    m_current = tenant;
    if (const Library *library = m_backend ? m_backend->library() : nullptr)
        m_backend->settings()->setGraphTenant(library->root(), tenant);
    emit changed();
}

void GraphTenants::step(int by) {
    if (m_tenants.isEmpty())
        return;
    const auto size = m_tenants.size();
    const auto at = std::ranges::find(std::as_const(m_tenants), m_current, idOf) - m_tenants.cbegin();
    choose(idOf(m_tenants.at(((at + by) % size + size) % size)));
}

QString GraphTenants::tenantOf(qint64 bookId) const {
    const Library *library = m_backend ? m_backend->library() : nullptr;
    if (!library)
        return m_current;
    const QList<Tenant> own = Tenancy::ofBook(*library, bookId);
    if (own.isEmpty() || std::ranges::contains(own, m_current, &Tenant::globalId))
        return m_current;
    return own.first().globalId();
}
