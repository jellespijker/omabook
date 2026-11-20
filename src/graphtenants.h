#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class Backend;

// Which tenant of the knowledge graph (ADR 0003) the graph window and the
// chronology opened from it show: one of the library's top-level
// collections, or Unfiled, the books in none. There is no view of them all
// at once; one switches from one to another, and the one shown last is
// shown again next time, remembered per library. With one tenant for the
// whole library (Settings::graphOneTenant) there is only that one, and
// nothing to switch.
//
// One of these is shared by every view in the window, so a switch made in
// the chronology is the graph's too.
class GraphTenants : public QObject {
    Q_OBJECT
    // The Backend whose library it is. Not called `backend`, so a view can
    // bind it to the window's `backend` without meaning this property.
    Q_PROPERTY(QObject *source READ backend WRITE setBackend NOTIFY backendChanged)
    // The tenants to switch between, each {id, name, path, books, unfiled}:
    // the top-level collections in their order, then Unfiled while a book
    // is in it (or nothing else is there). `path` is the collection's, to
    // tell the collections inside it; `name` is empty for Unfiled, which
    // the window names in its language.
    Q_PROPERTY(QVariantList tenants READ tenants NOTIFY changed)
    // The one shown, by its global id (Tenant::globalId), and its entry of
    // `tenants`; setting it to one of them switches to it and remembers it.
    Q_PROPERTY(QString current READ current WRITE choose NOTIFY changed)
    Q_PROPERTY(QVariantMap shown READ shown NOTIFY changed)
    // Whether there is another one to switch to.
    Q_PROPERTY(bool switchable READ switchable NOTIFY changed)

public:
    explicit GraphTenants(QObject *parent = nullptr);

    QObject *backend() const;
    void setBackend(QObject *backend);

    QVariantList tenants() const noexcept { return m_tenants; }
    QString current() const noexcept { return m_current; }
    QVariantMap shown() const;
    bool switchable() const noexcept { return m_tenants.size() > 1; }
    void choose(const QString &tenant);
    // The next (1) or the previous (-1) one, round from the last to the first.
    Q_INVOKABLE void step(int by);
    // The tenant a book is seen in: the one shown when the book is in it,
    // else the first of its own; the one shown for a book in none here.
    Q_INVOKABLE QString tenantOf(qint64 bookId) const;
    // Reads the tenants again: the collections changed, or the choice of
    // one tenant for the whole library (which it follows of itself).
    Q_INVOKABLE void refresh();

signals:
    void backendChanged();
    void changed();

private:
    Backend *m_backend = nullptr;
    QVariantList m_tenants;
    QString m_current;
    // The choice of one tenant for the whole library as last read, to see it change.
    bool m_one = false;
};
