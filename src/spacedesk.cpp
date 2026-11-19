#include "spacedesk.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThreadPool>

#include "backend.h"
#include "collectionshare.h"
#include "spacesnapshot.h"
#include "subscribedspace.h"
#include "subscriptions.h"

namespace {

using namespace Qt::StringLiterals;

[[nodiscard]] QVariantMap failed(const QString &why) { return {{"error", why}}; }

[[nodiscard]] PublishedSpace::Parts partsOf(const QVariantMap &parts) {
    return {.graph = parts.value("graph").toBool(),
            .questions = parts.value("questions").toBool(),
            .ratings = parts.value("ratings").toBool(),
            .reading = parts.value("reading").toBool()};
}

// Where a collection's snapshot is offered to be written: among the user's
// documents, named for the collection.
[[nodiscard]] QString suggestedFile(const Collection &collection) {
    const QString documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    const QString folder = QDir(documents).exists() ? documents : QDir::homePath();
    QString name = collection.name.simplified();
    name.replace(u'/', u'-');
    return QDir(folder).filePath(name + QLatin1StringView(SpaceSnapshotFile::suffix));
}

// What publishing `collection` puts in its space with every part switched on:
// the same summary the command line shows (CollectionShare::preview), so
// each switch in the dialog can say what it adds.
[[nodiscard]] QVariantMap lookAt(const Library &library, qint64 id) {
    const auto collection = library.collection(id);
    if (!collection)
        return failed(SpaceDesk::tr("That collection is gone."));
    const auto shown = CollectionShare::preview(library, *collection, {.graph = true, .questions = true, .ratings = true, .reading = true});
    if (!shown)
        return failed(shown.error());
    const QJsonObject in = shown->value("in").toObject();
    int files = 0;
    for (const QJsonValue part : shown->value("left_out").toArray())
        if (part.toObject().value("part").toString() == "files"_L1)
            files = part.toObject().value("count").toInt();
    return {{"collection", id},
            {"path", collection->path},
            {"collections", in.value("collections").toInt()},
            {"books", in.value("books").toInt()},
            {"things", in.value("things").toInt()},
            {"facts", in.value("facts").toInt()},
            {"relations", in.value("relations").toInt()},
            {"questions", in.value("questions").toInt()},
            {"ratings", in.value("ratings").toInt()},
            {"places", in.value("reading_places").toInt()},
            {"days", in.value("reading_days").toInt()},
            {"files", files},
            {"name", CollectionShare::nameGoneBy(library, *collection)},
            {"file", suggestedFile(*collection)},
            {"error", QString()}};
}

[[nodiscard]] QVariantMap bookOf(const Subscriptions::Placed &book) { return {{"title", book.title}, {"authors", book.authors}}; }

// What subscribing to `source` would do here, as a dry run works it out.
[[nodiscard]] QVariantMap readSpace(Library &library, const QString &source) {
    const auto snapshot = Subscriptions::fetch(source);
    if (!snapshot)
        return failed(snapshot.error());
    const auto report = Subscriptions::subscribe(library, *snapshot, {.as = {}, .dryRun = true});
    if (!report)
        return failed(report.error());
    QVariantList matched, wishlist, skipped;
    for (const Subscriptions::Placed &book : report->books) {
        if (book.how == "wishlist"_L1)
            wishlist << bookOf(book);
        else if (book.book > 0)
            matched << bookOf(book);
        else
            skipped << bookOf(book);
    }
    return {{"file", source},
            {"space", report->space.space},
            {"name", report->space.name},
            {"owner", report->space.ownerName},
            {"collection", report->collection},
            {"newCollection", report->newCollection},
            {"matched", matched},
            {"wishlist", wishlist},
            {"skipped", skipped},
            {"things", static_cast<qint64>(report->things)},
            {"facts", static_cast<qint64>(report->facts)},
            {"relations", static_cast<qint64>(report->relations)},
            {"known", SubscribedSpace::isSubscribed(library, report->space.space)},
            {"error", QString()}};
}

} // namespace

SpaceDesk::SpaceDesk(Backend &backend, FilePicker *picker) : m_backend(backend), m_picker(picker) {
    if (m_picker)
        connect(m_picker, &FilePicker::picked, this, &SpaceDesk::picked);
}

void SpaceDesk::beside(std::function<QVariantMap(Library &)> work, std::function<void(const QVariantMap &)> done) {
    const Library *library = m_backend.library();
    if (!library) {
        done(failed(tr("There is no library open.")));
        return;
    }
    ++m_working;
    emit changed();
    const QString root = library->root();
    const QPointer<SpaceDesk> self(this);
    QThreadPool::globalInstance()->start([self, root, work = std::move(work), done = std::move(done)] {
        Library own(root);
        QString error;
        const QVariantMap made = own.open(&error) ? work(own) : failed(error);
        QMetaObject::invokeMethod(qApp, [self, made, done] {
            if (!self)
                return;
            --self->m_working;
            done(made);
            emit self->changed();
        });
    });
}

void SpaceDesk::look(qint64 collection) {
    m_publishing = {{"collection", collection}};
    emit changed();
    beside([collection](Library &library) { return lookAt(library, collection); },
           [this, collection](const QVariantMap &looked) {
               // Only what was asked last: the dialog may have moved on to another.
               if (m_publishing.value("collection").toLongLong() == collection)
                   m_publishing = looked;
           });
}

void SpaceDesk::publish(qint64 collection, const QVariantMap &parts, const QString &as, const QString &file) {
    const PublishedSpace::Parts chosen = partsOf(parts);
    beside(
        [collection, chosen, as, file](Library &library) {
            const auto shared = library.collection(collection);
            if (!shared)
                return failed(tr("That collection is gone."));
            const auto done = CollectionShare::publish(library, *shared, chosen, {.as = as, .output = file});
            return done ? done->toVariantMap() : failed(done.error());
        },
        [this, file](const QVariantMap &done) {
            if (!done.value("error").toString().isEmpty()) {
                m_publishing.insert("error", done.value("error"));
                return;
            }
            m_backend.reload();
            m_backend.say(tr("Published as %1. %2 holds it: give it to whoever you share the collection with.")
                              .arg(done.value("owner").toString(), QFileInfo(file).fileName()));
            emit published(file);
        });
}

void SpaceDesk::read(const QString &source) {
    m_subscribing = source.isEmpty() ? QVariantMap() : QVariantMap{{"file", source}};
    emit changed();
    if (source.isEmpty())
        return;
    beside([source](Library &library) { return readSpace(library, source); },
           [this, source](const QVariantMap &read) {
               if (m_subscribing.value("file").toString() == source)
                   m_subscribing = read;
           });
}

void SpaceDesk::subscribe(const QString &source, const QString &as) {
    beside(
        [source, as](Library &library) {
            const auto snapshot = Subscriptions::fetch(source);
            if (!snapshot)
                return failed(snapshot.error());
            const auto report = Subscriptions::subscribe(library, *snapshot, {.as = as.simplified(), .dryRun = false});
            if (!report)
                return failed(report.error());
            return QVariantMap{{"collection", report->space.collection},
                               {"path", report->collection},
                               {"name", report->space.name},
                               {"owner", report->space.ownerName}};
        },
        [this](const QVariantMap &done) {
            if (!done.value("error").toString().isEmpty()) {
                m_subscribing.insert("error", done.value("error"));
                return;
            }
            m_backend.reload();
            m_backend.setCollection(done.value("collection").toLongLong());
            m_backend.say(tr("Subscribed to %1, shared by %2. Its books are in %3; its graph stays a layer of its own.")
                              .arg(done.value("name").toString(), done.value("owner").toString(), done.value("path").toString()));
            emit subscribed(done.value("collection").toLongLong());
        });
}

void SpaceDesk::unsubscribe(const QString &space) {
    Library *library = m_backend.library();
    if (!library)
        return;
    const auto dropped = Subscriptions::unsubscribe(*library, space);
    if (!dropped) {
        m_backend.say(dropped.error());
        return;
    }
    m_backend.reload();
    m_backend.say(dropped->collection.isEmpty()
                      ? tr("Unsubscribed from %1.").arg(dropped->space.name)
                      : tr("Unsubscribed from %1. The collection %2 stays, with its books and wishlist entries: it is yours.")
                            .arg(dropped->space.name, dropped->collection));
}

void SpaceDesk::chooseFile(bool save, const QString &suggested) {
    if (m_picker)
        m_picker->pick(save ? FilePicker::SaveSpace : FilePicker::OpenSpace, suggested);
}

QList<QUrl> SpaceDesk::takeSpaces(const QList<QUrl> &urls) {
    QList<QUrl> books;
    QString space;
    for (const QUrl &url : urls) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(QLatin1StringView(SpaceSnapshotFile::suffix), Qt::CaseInsensitive)) {
            if (space.isEmpty())
                space = url.toLocalFile();
        } else {
            books << url;
        }
    }
    if (!space.isEmpty()) {
        read(space);
        emit offered();
    }
    return books;
}

void SpaceDesk::picked(FilePicker::Purpose purpose, const QList<QUrl> &urls) {
    if (urls.isEmpty())
        return;
    if (purpose == FilePicker::SaveSpace) {
        emit fileChosen(urls.first().toLocalFile());
    } else if (purpose == FilePicker::OpenSpace) {
        read(urls.first().toLocalFile());
        emit offered();
    }
}
