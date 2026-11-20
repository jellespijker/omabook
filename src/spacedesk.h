#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>
#include <functional>
#include <memory>

#include "filepicker.h"
#include "spacetransport.h"

class Backend;
class Library;
struct Book;

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
    // "ratings", "places", "days", "files" (the books', a part of its own), "name" (gone by
    // before; empty the first time), "file" (where to write it, suggested),
    // "error", and once published by link "link", its address}; empty until asked.
    Q_PROPERTY(QVariantMap publishing READ publishing NOTIFY changed)
    // What subscribing to the file read (read) would do, nothing written yet:
    // {"file", "space", "name", "owner", "collection" (where its books go
    // here), "newCollection", "matched" and "wishlist" ([{"title",
    // "authors"}]: books found here, and those that become wishlist entries),
    // "skipped" (books removed here before, which stay so), "things",
    // "facts", "relations", "files" (its owner lets the books' files come,
    // each as its book is opened), "known" (whether it is subscribed to already),
    // "link" (it came by a link, which subscribing follows), "error"}; empty
    // until asked.
    Q_PROPERTY(QVariantMap subscribing READ subscribing NOTIFY changed)
    // What following a subscription's link found (lookForUpdates), never
    // taken in by itself: {"space", "operations" (new), "changes" (what they
    // change, in words), "problem"}; empty until asked.
    Q_PROPERTY(QVariantMap following READ following NOTIFY changed)
    // Something above is being worked out, or done.
    Q_PROPERTY(bool working READ working NOTIFY changed)

public:
    // Makes the transport a link goes by (GitHubLink), on the worker that uses it.
    using Transports = std::function<std::unique_ptr<SpaceTransport>()>;

    SpaceDesk(Backend &backend, FilePicker *picker);
    // Tests put a transport of their own in place of GitHub.
    void setTransports(Transports transports) { m_transports = std::move(transports); }

    QVariantMap publishing() const { return m_publishing; }
    QVariantMap subscribing() const { return m_subscribing; }
    QVariantMap following() const { return m_following; }
    bool working() const noexcept { return m_working > 0; }

    // Works out `publishing` for a collection.
    Q_INVOKABLE void look(qint64 collection);
    // Publishes it with the parts switched on ({"graph", "questions",
    // "ratings", "reading", "files"}), as `as`, writes its snapshot to `file` unless
    // that is empty, and with `link` sends it as a link too (a gist, secret
    // unless `open`), its address copied for pasting.
    Q_INVOKABLE void publish(qint64 collection, const QVariantMap &parts, const QString &as, const QString &file, bool link = false,
                             bool open = false);
    // Works out `subscribing` for a space's file or link (#7); empty,
    // forgets the one read before.
    Q_INVOKABLE void read(const QString &source);
    // Subscribes to it, its books in the collection `as` (the space's name when empty).
    Q_INVOKABLE void subscribe(const QString &source, const QString &as);
    // Looks at the link a subscription follows for what is new there
    // (`following`), and takes that in, once asked to.
    Q_INVOKABLE void lookForUpdates(const QString &space);
    Q_INVOKABLE void takeUpdates(const QString &space);
    // Drops a subscribed space; the collection it filled stays the user's.
    Q_INVOKABLE void unsubscribe(const QString &space);
    // Asks where to write a snapshot (fileChosen), or for one to read (offered).
    Q_INVOKABLE void chooseFile(bool save, const QString &suggested = {});
    // Opening a book that has no file here (#63): when a space it is in lets
    // its files travel, fetches one beside the window and then calls
    // `opened`; else says why there is nothing to open, as of a paper book or
    // a wish.
    void bring(const Book &book, std::function<void()> opened);
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
    Transports m_transports;
    QVariantMap m_publishing;
    QVariantMap m_subscribing;
    QVariantMap m_following;
    int m_working = 0;
};
