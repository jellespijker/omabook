#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <functional>

#include "filepicker.h"

class Backend;
class Library;

// Publishing a collection and subscribing to one, as the window's dialogs do
// it (#6): the same operations the command line and the MCP server run
// (CollectionShare, Subscriptions), beside the window, with what they find
// and do as maps for QML. Nothing is published or subscribed to before the
// dialog has shown what it means and the user has said yes.
class SpaceDesk : public QObject {
    Q_OBJECT
    // What publishing the collection asked about (look) would put in its
    // space, counted as the command line's summary counts it, with every
    // part of the user's own: {"collection" (its number), "path",
    // "collections", "books", "things", "facts", "relations", "questions",
    // "ratings", "places", "days", "files" (never shared), "name" (gone by
    // before; empty the first time), "file" (where to write it, suggested),
    // "error"}; empty until asked.
    Q_PROPERTY(QVariantMap publishing READ publishing NOTIFY changed)
    // What subscribing to the file read (read) would do, nothing written yet:
    // {"file", "space", "name", "owner", "collection" (where its books go
    // here), "newCollection", "matched" and "wishlist" ([{"title",
    // "authors"}]: books found here, and those that become wishlist entries),
    // "skipped" (books removed here before, which stay so), "things",
    // "facts", "relations", "known" (whether it is subscribed to already),
    // "error"}; empty until asked.
    Q_PROPERTY(QVariantMap subscribing READ subscribing NOTIFY changed)
    // Something above is being worked out, or done.
    Q_PROPERTY(bool working READ working NOTIFY changed)

public:
    SpaceDesk(Backend &backend, FilePicker *picker);

    QVariantMap publishing() const { return m_publishing; }
    QVariantMap subscribing() const { return m_subscribing; }
    bool working() const noexcept { return m_working > 0; }

    // Works out `publishing` for a collection.
    Q_INVOKABLE void look(qint64 collection);
    // Publishes it with the parts switched on ({"graph", "questions",
    // "ratings", "reading"}), as `as`, and writes its snapshot to `file`.
    Q_INVOKABLE void publish(qint64 collection, const QVariantMap &parts, const QString &as, const QString &file);
    // Works out `subscribing` for a space's file (a link, once there is a
    // transport for one, #7); empty, forgets the one read before.
    Q_INVOKABLE void read(const QString &source);
    // Subscribes to it, its books in the collection `as` (the space's name when empty).
    Q_INVOKABLE void subscribe(const QString &source, const QString &as);
    // Drops a subscribed space; the collection it filled stays the user's.
    Q_INVOKABLE void unsubscribe(const QString &space);
    // Asks where to write a snapshot (fileChosen), or for one to read (offered).
    Q_INVOKABLE void chooseFile(bool save, const QString &suggested = {});
    // Of files dropped on the window, the first space's is read (offered);
    // the others are handed back, to be added as books.
    Q_INVOKABLE QList<QUrl> takeSpaces(const QList<QUrl> &urls);

signals:
    void changed();
    // A space's file is being read, to be shown for subscribing to.
    void offered();
    void fileChosen(const QString &file);
    void published(const QString &file);
    void subscribed(qint64 collection);

private:
    // Runs `work` on a worker with a library of its own, and `done` with
    // what it made back here, unless this is gone by then.
    void beside(std::function<QVariantMap(Library &)> work, std::function<void(const QVariantMap &)> done);
    void picked(FilePicker::Purpose purpose, const QList<QUrl> &urls);

    Backend &m_backend;
    FilePicker *m_picker;
    QVariantMap m_publishing;
    QVariantMap m_subscribing;
    int m_working = 0;
};
