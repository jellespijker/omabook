#include "spacedesk.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThreadPool>
#include <array>
#include <utility>

#include "backend.h"
#include "collectionshare.h"
#include "following.h"
#include "githublink.h"
#include "library.h"
#include "person.h"
#include "scriptsharing.h"
#include "spacefiles.h"
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
            .reading = parts.value("reading").toBool(),
            .files = parts.value("files").toBool(),
            .annotations = parts.value("annotations").toBool()};
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
    const int files = shown->value("files").toInt();
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

// A link the transport goes by, rather than a file here.
[[nodiscard]] bool isLink(const SpaceTransport &transport, const QString &source) {
    return !QFileInfo::exists(source) && !source.startsWith("file:"_L1) && transport.handles(source);
}

// What subscribing to `source` would do here, as a dry run works it out.
[[nodiscard]] QVariantMap readSpace(Library &library, const QString &source, SpaceTransport &transport) {
    const bool link = isLink(transport, source);
    const auto snapshot = link ? transport.fetch(source) : Subscriptions::fetch(source);
    if (!snapshot)
        return failed(snapshot.error());
    const auto report = Subscriptions::subscribe(library, *snapshot, {.as = {}, .dryRun = true});
    if (!report)
        return failed(report.error());
    QVariantList matched;
    QVariantList wishlist;
    QVariantList skipped;
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
            {"files", snapshot->parts.contains("files"_L1)},
            {"known", SubscribedSpace::isSubscribed(library, report->space.space)},
            {"link", link},
            {"error", QString()}};
}

[[nodiscard]] std::expected<Subscriptions::Report, QString> subscribeToFile(Library &library, const QString &file,
                                                                            const Subscriptions::Options &options) {
    const auto snapshot = Subscriptions::fetch(file);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    auto report = Subscriptions::subscribe(library, *snapshot, options);
    if (report) {
        SpaceFiles::takenFrom(library, report->space.space, file); // its books' files, if it lets them travel, are beside it
        SpaceFiles::bringWorldFiles(library, report->space.space); // and the files a plugin gave with their worlds
    }
    return report;
}

// What following a link would change, in the window's language: "Books: 2
// new, 1 changed; Facts: 4 new".
[[nodiscard]] QString changesSaid(const SpaceDiff::Changes &changes) {
    const std::array<std::pair<SpaceDiff::Count, QString>, 6> kinds{{{changes.books, SpaceDesk::tr("Books")},
                                                                     {changes.collections, SpaceDesk::tr("Collections")},
                                                                     {changes.things, SpaceDesk::tr("Things")},
                                                                     {changes.facts, SpaceDesk::tr("Facts")},
                                                                     {changes.relations, SpaceDesk::tr("Relations")},
                                                                     {changes.days, SpaceDesk::tr("Reading days")}}};
    QStringList said;
    for (const auto &[count, kind] : kinds) {
        QStringList parts;
        if (count.added > 0)
            parts << SpaceDesk::tr("%Ln new", "", static_cast<int>(count.added));
        if (count.changed > 0)
            parts << SpaceDesk::tr("%Ln changed", "", static_cast<int>(count.changed));
        if (count.removed > 0)
            parts << SpaceDesk::tr("%Ln gone", "", static_cast<int>(count.removed));
        if (!parts.isEmpty())
            said << SpaceDesk::tr("%1: %2").arg(kind, parts.join(", "_L1));
    }
    return said.isEmpty() ? SpaceDesk::tr("nothing it says changes") : said.join("; "_L1);
}

} // namespace

SpaceDesk::SpaceDesk(Backend &backend, FilePicker *picker)
    : m_backend(backend), m_picker(picker), m_transports([] { return std::make_unique<GitHubLink>(); }) {
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

void SpaceDesk::publish(qint64 collection, const QVariantMap &parts, const QString &as, const QString &file, bool link, bool open) {
    const PublishedSpace::Parts chosen = partsOf(parts);
    // The plugins are asked here, where they run, what of theirs goes with it (#65).
    SpaceWorlds::Ask worlds;
    const Library *here = m_backend.library();
    const auto shelf = here != nullptr ? here->collection(collection) : std::nullopt;
    if (shelf && here->person() != nullptr && m_backend.scripts() != nullptr) {
        QList<SpaceWorlds::Offer> offered =
            worldsOf(*m_backend.scripts())(CollectionShare::asked(*here, *shelf, chosen, here->person()->id()));
        worlds = [offered = std::move(offered)](const SpaceWorlds::Asked &) { return offered; };
    }
    beside(
        [collection, chosen, as, file, link, open, worlds, transports = m_transports](Library &library) {
            const auto shared = library.collection(collection);
            if (!shared)
                return failed(tr("That collection is gone."));
            const std::unique_ptr<SpaceTransport> transport = link ? transports() : nullptr;
            const auto done = CollectionShare::publish(library, *shared, chosen,
                                                       {.as = as, .output = file, .link = transport.get(), .open = open, .worlds = worlds});
            return done ? done->toVariantMap() : failed(done.error());
        },
        [this, file](const QVariantMap &done) {
            if (!done.value("error").toString().isEmpty()) {
                m_publishing.insert("error", done.value("error"));
                return;
            }
            m_backend.reload();
            const QString address = done.value("sent").toMap().value("address").toString();
            const QString owner = done.value("owner").toString();
            m_publishing.insert("link", address);
            // The link, ready to paste where it is given.
            if (!address.isEmpty())
                QGuiApplication::clipboard()->setText(address);
            if (address.isEmpty())
                m_backend.say(tr("Published as %1. %2 holds it: give it to whoever you share the collection with.")
                                  .arg(owner, QFileInfo(file).fileName()));
            else if (file.isEmpty())
                m_backend.say(
                    tr("Published as %1, by the link %2 (copied): whoever has it can subscribe and follow it.").arg(owner, address));
            else
                m_backend.say(tr("Published as %1, in %2 and by the link %3 (copied): whoever has either can subscribe.")
                                  .arg(owner, QFileInfo(file).fileName(), address));
            emit published(file);
        });
}

void SpaceDesk::read(const QString &source) {
    m_subscribing = source.isEmpty() ? QVariantMap() : QVariantMap{{"file", source}};
    emit changed();
    if (source.isEmpty())
        return;
    beside([source, transports = m_transports](Library &library) { return readSpace(library, source, *transports()); },
           [this, source](const QVariantMap &read) {
               if (m_subscribing.value("file").toString() == source)
                   m_subscribing = read;
           });
}

void SpaceDesk::subscribe(const QString &source, const QString &as) {
    beside(
        [source, as, transports = m_transports](Library &library) {
            const Subscriptions::Options options{.as = as.simplified(), .dryRun = false};
            const std::unique_ptr<SpaceTransport> transport = transports();
            // By a link, the subscription follows it (Following).
            const auto report = isLink(*transport, source) ? Following::subscribe(library, source, *transport, options)
                                                           : subscribeToFile(library, source, options);
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

void SpaceDesk::lookForUpdates(const QString &space) {
    m_following = {{"space", space}};
    emit changed();
    beside(
        [space, transports = m_transports](Library &library) {
            const std::unique_ptr<SpaceTransport> transport = transports();
            const auto offers = Following::check(library, space, {transport.get()}, false);
            if (!offers || offers->isEmpty())
                return QVariantMap{{"space", space}, {"problem", offers ? tr("It follows no link.") : offers.error()}};
            const Following::Offer &offer = offers->first();
            return QVariantMap{{"space", space},
                               {"operations", static_cast<qint64>(offer.operations)},
                               {"changes", changesSaid(offer.changes)},
                               {"problem", offer.problem}};
        },
        [this, space](const QVariantMap &found) {
            if (m_following.value("space").toString() == space)
                m_following = found;
        });
}

void SpaceDesk::takeUpdates(const QString &space) {
    beside(
        [space, transports = m_transports](Library &library) {
            const std::unique_ptr<SpaceTransport> transport = transports();
            const auto offers = Following::check(library, space, {transport.get()}, true);
            if (!offers || offers->isEmpty())
                return failed(offers ? tr("It follows no link.") : offers.error());
            if (!offers->first().problem.isEmpty())
                return failed(offers->first().problem);
            return QVariantMap{{"operations", static_cast<qint64>(offers->first().operations)}, {"name", offers->first().space.name}};
        },
        [this, space](const QVariantMap &done) {
            m_following = {};
            if (const QString error = done.value("error").toString(); !error.isEmpty()) {
                m_following = {{"space", space}, {"problem", error}};
                return;
            }
            m_backend.reload();
            m_backend.say(
                tr("%1 is up to date: %Ln changes taken in.", "", done.value("operations").toInt()).arg(done.value("name").toString()));
        });
}

void SpaceDesk::unsubscribe(const QString &space) {
    const Library *library = m_backend.library();
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

void SpaceDesk::bring(const Book &book, std::function<void()> opened) {
    const Library *library = m_backend.library();
    const QList<SpaceFiles::Carried> carried = library ? SpaceFiles::of(*library, book.id) : QList<SpaceFiles::Carried>{};
    if (carried.isEmpty()) {
        m_backend.say(book.paper ? tr("%1 is on your shelf, not in a file. Add a file to it to read it here.").arg(book.meta.title)
                                 : tr("%1 is on your list, but you have no file of it yet.").arg(book.meta.title));
        return;
    }
    m_backend.say(tr("Fetching the file of %1 from %2…").arg(book.meta.title, carried.first().name));
    beside(
        [id = book.id](Library &library) {
            const auto fetched = SpaceFiles::fetch(library, id);
            return fetched ? QVariantMap{{"name", fetched->name}} : failed(fetched.error());
        },
        [this, opened = std::move(opened)](const QVariantMap &done) {
            if (const QString error = done.value("error").toString(); !error.isEmpty()) {
                m_backend.say(error);
                return;
            }
            m_backend.reload();
            opened();
        });
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
