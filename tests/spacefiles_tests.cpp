// Book files in spaces (ADR 0001, *Book files*; #63): a space's books' files
// travel only when the space's owner lets them (a writer of a shared space,
// the owner of a published one), to everyone the space reaches, each fetched
// only when it is wanted and taken only when it has the hash the space says;
// a member's own copy is never replaced. And a wishlist entry a space made is
// filled when its file is added, not left beside a second copy.

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "collectionshare.h"
#include "fixtures.h"
#include "importer.h"
#include "library.h"
#include "publishedspace.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "spacefiles.h"
#include "spacefolder.h"
#include "spacereducer.h"
#include "spacesnapshot.h"
#include "spacetransport.h"
#include "subscriptions.h"

namespace {

using Fixtures::whyNot;
using SharedSpace::Role;

// One person's computer: their library and their own Omabook data folder.
struct Someone {
    Someone(const QTemporaryDir &dir, const QString &name) : dir(dir), name(name), library(dir.filePath(name + "/Library")) {
        library.usePersonFile(dir.filePath(name + "/home/omabook/person.key"));
        QVERIFY(library.open());
        QVERIFY(library.person() != nullptr);
        id = library.person()->id();
    }

    // A book with a file of its own, its details as given, whatever the file says.
    qint64 bookWithFile(const QString &title, const QString &isbn, const QString &edition = {}) {
        const QString file = dir.filePath(name + "/" + title + edition + ".epub");
        if (!Fixtures::epub(file, title + edition))
            return 0;
        const qint64 id = library.add(file).id;
        BookMeta meta = library.book(id)->meta;
        meta.title = title;
        meta.authors = {"Frank Herbert"};
        meta.isbn = isbn;
        return library.setMeta(id, meta) ? id : 0;
    }

    [[nodiscard]] std::optional<Book> titled(const QString &title) const {
        for (const Book &book : library.books())
            if (book.meta.title == title)
                return book;
        return std::nullopt;
    }

    [[nodiscard]] QString hashOf(qint64 book) const {
        const QList<FormatFile> files = library.formatFiles(book);
        return files.isEmpty() ? QString() : files.first().hash;
    }

    const QTemporaryDir &dir;
    QString name;
    Library library;
    QString id;
};

[[nodiscard]] SharedSpaces::Synced synced(Someone &who, const QString &space, const QString &folder) {
    auto done = SharedSpaces::sync(who.library, space, folder);
    if (!done)
        qFatal("%s", qPrintable(done.error()));
    return *done;
}

// A household space founded at `founder`'s with Dune and its file, each of
// `others` invited with `role`, joined and in step, all through `folder`.
[[nodiscard]] QString householdOf(Someone &founder, std::initializer_list<Someone *> others, const QString &folder, Role role) {
    const auto created = SharedSpaces::create(founder.library, "Home", {});
    if (!created)
        qFatal("%s", qPrintable(created.error()));
    const QString space = created->space.space;
    if (!founder.library.putInCollection(founder.library.collectionNamed("Home")->id, founder.bookWithFile("Dune", "9780441172719"), true))
        qFatal("no Dune");
    for (const Someone *member : others)
        if (const auto invited = SharedSpaces::invite(founder.library, space, member->id, role, founder.dir.filePath(member->id.left(8)));
            !invited)
            qFatal("%s", qPrintable(invited.error()));
    std::ignore = synced(founder, space, folder);
    for (Someone *member : others) {
        if (const auto joined = SharedSpaces::join(member->library, founder.dir.filePath(member->id.left(8))); !joined)
            qFatal("%s", qPrintable(joined.error()));
        std::ignore = synced(*member, space, folder);
    }
    return space;
}

// Every file under `folder`, by its path below it.
[[nodiscard]] QStringList filesIn(const QString &folder) {
    QStringList files;
    for (QDirIterator it(folder, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
        files << QDir(folder).relativeFilePath(it.next());
    return files;
}

[[nodiscard]] QByteArray contentOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// Whether any operation of `ops` says which files a book has.
[[nodiscard]] bool saysFiles(const QList<SpaceOperation> &ops) {
    return std::ranges::any_of(ops, [](const SpaceOperation &op) {
        return op.entity == "book" && op.field == "formats" && !QJsonDocument::fromJson(op.value.toUtf8()).array().isEmpty();
    });
}

// Someone who publishes "Shelf", Dune with its file in it.
struct Owner {
    explicit Owner(const QTemporaryDir &dir) : who(dir, "Owner") {
        dune = who.bookWithFile("Dune", "9780441172719");
        const auto made = who.library.addCollection("Shelf");
        QVERIFY(made && dune > 0 && who.library.putInCollection(*made, dune, true));
        shelf = *made;
    }

    // Publishes it with `parts`, its file written to `file`.
    [[nodiscard]] QJsonObject publish(const PublishedSpace::Parts &parts, const QString &file) {
        const auto done = CollectionShare::publish(who.library, *who.library.collection(shelf), parts, {.as = "Ada", .output = file});
        if (!done)
            qFatal("%s", qPrintable(done.error()));
        return *done;
    }

    Someone who;
    qint64 dune = 0;
    qint64 shelf = 0;
};

// Subscribes `who` to the space's file, as `omabook collections import` does.
[[nodiscard]] Subscriptions::Report subscribe(Someone &who, const QString &file) {
    const auto snapshot = Subscriptions::fetch(file);
    if (!snapshot)
        qFatal("%s", qPrintable(snapshot.error()));
    auto report = Subscriptions::subscribe(who.library, *snapshot, {});
    if (!report)
        qFatal("%s", qPrintable(report.error()));
    SpaceFiles::takenFrom(who.library, report->space.space, file);
    return *report;
}

// A link that keeps what it is sent, in place of a gist.
class KeptLink final : public SpaceTransport {
public:
    [[nodiscard]] QString name() const override { return "test"; }
    [[nodiscard]] bool handles(const QString &address) const override { return address == "https://link.test/1"; }
    [[nodiscard]] std::expected<SpaceSnapshot, QString> fetch(const QString &) override { return kept; }
    [[nodiscard]] std::expected<Sent, QString> publish(const SpaceSnapshot &snapshot, const Sending &) override {
        kept = snapshot;
        return Sent{.address = "https://link.test/1", .created = true, .open = false, .note = {}};
    }

    SpaceSnapshot kept;
};

} // namespace

class SpaceFilesTests : public QObject {
    Q_OBJECT

private slots:
    void aHouseholdsFilesTravelOnceAWriterLetsThem() {
        const QTemporaryDir dir;
        Someone ada(dir, "ada");
        Someone bo(dir, "bo");
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        const QString space = householdOf(ada, {&bo}, folder, Role::Writer);

        // Off until a writer says otherwise, and saying so is said plainly.
        QVERIFY(!SpaceFiles::on(bo.library, space));
        const auto switched = SpaceFiles::set(ada.library, space, true);
        QVERIFY2(switched, qPrintable(whyNot(switched)));
        QVERIFY(switched->changed && switched->on);
        QVERIFY2(SpaceFiles::said(*switched).contains("to every member"), qPrintable(SpaceFiles::said(*switched)));
        QVERIFY(SharedSpaces::list(ada.library).last().files);
        QCOMPARE(synced(ada, space, folder).files, 1);

        // Bo gets the book, and the switch, but not the file: that comes when it is wanted.
        std::ignore = synced(bo, space, folder);
        QVERIFY(SpaceFiles::on(bo.library, space));
        const auto dune = bo.titled("Dune");
        QVERIFY(dune.has_value());
        QVERIFY(dune->formats.isEmpty());
        const QList<SpaceFiles::Carried> carried = SpaceFiles::of(bo.library, dune->id);
        QCOMPARE(carried.size(), 1);
        const QString hash = ada.hashOf(ada.titled("Dune")->id);
        QCOMPARE(carried.first().hash, hash);

        // The folder holds it sealed, under a name that says nothing of it.
        const QStringList kept = filesIn(SpaceFolder::of(folder, space) + "/files");
        QCOMPARE(kept.size(), 1);
        QVERIFY(!kept.first().contains(hash));
        const QByteArray sealed = contentOf(SpaceFolder::of(folder, space) + "/files/" + kept.first());
        QVERIFY(!sealed.contains("Dune") && !sealed.contains("mimetype"));

        // Opening it fetches it, checked against its hash.
        const auto fetched = SpaceFiles::fetch(bo.library, dune->id);
        QVERIFY2(fetched, qPrintable(whyNot(fetched)));
        QCOMPARE(bo.library.book(dune->id)->formats, QStringList{"epub"});
        QCOMPARE(bo.hashOf(dune->id), hash);
        QCOMPARE(contentOf(bo.library.formatPath(dune->id)), contentOf(ada.library.formatPath(ada.titled("Dune")->id)));
        // And now it has one, there is nothing more to fetch, and syncing again changes nothing of it.
        QVERIFY(SpaceFiles::of(bo.library, dune->id).isEmpty());
        QCOMPARE(synced(bo, space, folder).files, 0);
        QCOMPARE(bo.library.books().size(), 1);
    }

    void aFileChangedInTheFolderIsRefused() {
        const QTemporaryDir dir;
        Someone ada(dir, "ada");
        Someone bo(dir, "bo");
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        const QString space = householdOf(ada, {&bo}, folder, Role::Reader);
        QVERIFY(SpaceFiles::set(ada.library, space, true).has_value());
        std::ignore = synced(ada, space, folder);
        std::ignore = synced(bo, space, folder);

        // Someone with the folder but not the key swaps in another file.
        const QString kept = SpaceFolder::of(folder, space) + "/files/" + filesIn(SpaceFolder::of(folder, space) + "/files").first();
        QByteArray changed = contentOf(kept);
        changed[changed.size() - 40] = static_cast<char>(changed.at(changed.size() - 40) ^ 0x5a);
        QVERIFY(Fixtures::write(kept, changed));
        const qint64 dune = bo.titled("Dune")->id;
        const auto fetched = SpaceFiles::fetch(bo.library, dune);
        QVERIFY(!fetched);
        QVERIFY(bo.library.book(dune)->formats.isEmpty());
        // A reader doesn't switch it.
        QVERIFY(!SpaceFiles::set(bo.library, space, false));
    }

    void withTheSwitchOffNothingOfTheFileTravels() {
        const QTemporaryDir dir;
        Someone ada(dir, "ada");
        Someone bo(dir, "bo");
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        const QString space = householdOf(ada, {&bo}, folder, Role::Writer);
        QCOMPARE(synced(ada, space, folder).files, 0);
        std::ignore = synced(bo, space, folder);

        QVERIFY(!QFileInfo::exists(SpaceFolder::of(folder, space) + "/files"));
        QVERIFY(!saysFiles(SharedSpace::state(bo.library, space).accepted));
        const qint64 dune = bo.titled("Dune")->id;
        QVERIFY(SpaceFiles::of(bo.library, dune).isEmpty());
        QVERIFY(!SpaceFiles::fetch(bo.library, dune));
        QVERIFY(bo.library.book(dune)->formats.isEmpty());

        // Switched on and off again: from then on nothing more goes, and nothing is fetched.
        QVERIFY(SpaceFiles::set(ada.library, space, true).has_value());
        QVERIFY(SpaceFiles::set(ada.library, space, false).has_value());
        QCOMPARE(synced(ada, space, folder).files, 0);
        std::ignore = synced(bo, space, folder);
        QVERIFY(!SpaceFiles::on(bo.library, space));
        QVERIFY(SpaceFiles::of(bo.library, dune).isEmpty());
    }

    void aMembersOwnCopyIsNeverReplaced() {
        const QTemporaryDir dir;
        Someone ada(dir, "ada");
        Someone cy(dir, "cy");
        // Cy has Dune already, another edition of it, matched by its ISBN.
        const qint64 own = cy.bookWithFile("Dune", "9780441172719", " (Cy's)");
        const QString ownHash = cy.hashOf(own);
        const QString folder = dir.filePath("Synced");
        QVERIFY(QDir().mkpath(folder));
        const QString space = householdOf(ada, {&cy}, folder, Role::Writer);
        QVERIFY(SpaceFiles::set(ada.library, space, true).has_value());
        std::ignore = synced(ada, space, folder);
        std::ignore = synced(cy, space, folder);

        QCOMPARE(cy.library.books().size(), 1);
        QVERIFY(SpaceFiles::of(cy.library, own).isEmpty());
        QVERIFY(!SpaceFiles::fetch(cy.library, own));
        QCOMPARE(cy.hashOf(own), ownHash);
        QCOMPARE(cy.library.formatFiles(own).size(), 1);
    }

    void aPublishedSpaceCarriesFilesOnlyWhenItsOwnerLetsThem() {
        const QTemporaryDir dir;
        Owner owner(dir);
        Someone reader(dir, "reader");
        const QString file = dir.filePath("Shelf.omabook-space");

        // Without the switch: nothing of the file goes, beside the file or in it.
        const QJsonObject plain = owner.publish({}, file);
        QVERIFY(!plain.contains("files"));
        QVERIFY(!QFileInfo::exists(SpaceFiles::besideOf(file)));
        QVERIFY(!saysFiles(SpaceSnapshotFile::load(file)->operations));
        const Subscriptions::Report first = subscribe(reader, file);
        const qint64 dune = reader.titled("Dune")->id;
        QVERIFY(reader.library.book(dune)->formats.isEmpty());
        QVERIFY(SpaceFiles::of(reader.library, dune).isEmpty());
        QVERIFY(!SpaceFiles::fetch(reader.library, dune));

        // With it: the snapshot names the file by its hash, and the file goes beside it, not in it.
        const QJsonObject withFiles = owner.publish({.files = true}, file);
        QCOMPARE(withFiles.value("files").toObject().value("copied").toInt(), 1);
        const QString hash = owner.who.hashOf(owner.dune);
        QCOMPARE(filesIn(SpaceFiles::besideOf(file)), QStringList{hash + ".epub"});
        const auto snapshot = SpaceSnapshotFile::load(file);
        QVERIFY(snapshot && snapshot->parts.contains("files") && saysFiles(snapshot->operations));
        QVERIFY(!contentOf(file).contains(contentOf(owner.who.library.formatPath(owner.dune)).left(200)));
        QVERIFY(SpaceFiles::on(owner.who.library, first.space.space));

        // The subscriber takes the newer snapshot, and gets the file when opening the book.
        std::ignore = subscribe(reader, file);
        QVERIFY(SpaceFiles::on(reader.library, first.space.space));
        QVERIFY(reader.library.book(dune)->formats.isEmpty());
        const auto fetched = SpaceFiles::fetch(reader.library, dune);
        QVERIFY2(fetched, qPrintable(whyNot(fetched)));
        QCOMPARE(reader.hashOf(dune), hash);
        QCOMPARE(reader.library.books().size(), 1);
    }

    void theOwnerSwitchesAPublishedSpacesFiles() {
        const QTemporaryDir dir;
        Owner owner(dir);
        std::ignore = owner.publish({}, dir.filePath("Shelf.omabook-space"));
        const auto switched = SpaceFiles::set(owner.who.library, "Shelf", true);
        QVERIFY2(switched, qPrintable(whyNot(switched)));
        QVERIFY2(SpaceFiles::said(*switched).contains("whoever has its file or link"), qPrintable(SpaceFiles::said(*switched)));
        const auto snapshot = PublishedSpace::snapshotOf(owner.who.library, switched->space);
        QVERIFY(snapshot && snapshot->parts.contains("files") && saysFiles(snapshot->operations));
        // Never where the file lies in the owner's library.
        QVERIFY(std::ranges::none_of(snapshot->operations, [](const SpaceOperation &op) { return op.value.contains("\"file\""); }));

        // Off again, the snapshot takes back what it said of the files.
        QVERIFY(SpaceFiles::set(owner.who.library, switched->space, false).has_value());
        const auto after = PublishedSpace::snapshotOf(owner.who.library, switched->space);
        QVERIFY(after && !after->parts.contains("files"));
        const ReducedState state = SpaceReducer::reduce(after->operations);
        QVERIFY(std::ranges::all_of(state.books, [](const ReducedBook &book) {
            return QJsonDocument::fromJson(book.fields.value("formats").value.toUtf8()).array().isEmpty();
        }));
    }

    void aSubscriberWhoFollowsALinkIsToldWhereTheFilesCome() {
        const QTemporaryDir dir;
        Owner owner(dir);
        Someone reader(dir, "reader");
        const QString file = dir.filePath("Shelf.omabook-space");
        std::ignore = owner.publish({.files = true}, file);
        // Taken in as a link would bring it: nothing remembered of a file beside it.
        const auto snapshot = Subscriptions::fetch(file);
        QVERIFY(snapshot.has_value());
        QVERIFY(Subscriptions::subscribe(reader.library, *snapshot, {}).has_value());
        const auto fetched = SpaceFiles::fetch(reader.library, reader.titled("Dune")->id);
        QVERIFY(!fetched);
        QVERIFY2(whyNot(fetched).contains(".omabook-space"), qPrintable(whyNot(fetched)));
    }

    void aLinkCarriesNoFilesAndSaysSo() {
        const QTemporaryDir dir;
        Owner owner(dir);
        KeptLink link;
        const auto done = CollectionShare::publish(owner.who.library, *owner.who.library.collection(owner.shelf), {.files = true},
                                                   {.as = "Ada", .output = {}, .link = &link, .open = false});
        QVERIFY2(done, qPrintable(whyNot(done)));
        const QString note = done->value("sent").toObject().value("note").toString();
        QVERIFY2(note.contains("don't go by a link"), qPrintable(note));
        // The link holds the space, naming the files by their hashes, and none of their content.
        QVERIFY(link.kept.parts.contains("files") && saysFiles(link.kept.operations));
        QVERIFY(!SpaceSnapshotFile::write(link.kept).contains(contentOf(owner.who.library.formatPath(owner.dune)).left(200)));
    }

    void addingAWishedForFileFillsTheEntry() {
        const QTemporaryDir dir;
        Owner owner(dir);
        // The owner's details of it are their own: another title and ISBN than the file says.
        BookMeta meta = owner.who.library.book(owner.dune)->meta;
        meta.title = "Dune: Deluxe";
        QVERIFY(owner.who.library.setMeta(owner.dune, meta));
        Someone reader(dir, "reader");
        const QString file = dir.filePath("Shelf.omabook-space");
        std::ignore = owner.publish({}, file);
        std::ignore = subscribe(reader, file);
        const auto wish = reader.titled("Dune: Deluxe");
        QVERIFY(wish && wish->formats.isEmpty());

        // The reader gets the very file elsewhere, and adds it: it fills the wish.
        const QString copy = dir.filePath("Downloaded.epub");
        QVERIFY(QFile::copy(owner.who.library.formatPath(owner.dune), copy));
        const ImportSummary added = importBooks(reader.library, {copy});
        QCOMPARE(added.joined, 1);
        QCOMPARE(added.added, 0);
        QCOMPARE(reader.library.books().size(), 1);
        QCOMPARE(reader.library.book(wish->id)->formats, QStringList{"epub"});
        QVERIFY(reader.library.collectionNamed("Shelf")->contains(*reader.library.book(wish->id)));
    }

    void addingAnotherFileOfAWishedForBookFillsTheEntry() {
        const QTemporaryDir dir;
        Owner owner(dir);
        // The ISBN the fixture's files say of themselves.
        BookMeta meta = owner.who.library.book(owner.dune)->meta;
        meta.isbn = "9780000000002";
        QVERIFY(owner.who.library.setMeta(owner.dune, meta));
        Someone reader(dir, "reader");
        const QString file = dir.filePath("Shelf.omabook-space");
        std::ignore = owner.publish({}, file);
        std::ignore = subscribe(reader, file);
        const qint64 wish = reader.titled("Dune")->id;
        // Another file of it, another edition, which says the ISBN the space does.
        const QString paperback = dir.filePath("Dune Paperback.epub");
        QVERIFY(Fixtures::epub(paperback, "Dune, the paperback"));
        const ImportSummary added = importBooks(reader.library, {paperback});
        QCOMPARE(added.joined, 1);
        QCOMPARE(reader.library.books().size(), 1);
        QCOMPARE(reader.library.book(wish)->formats, QStringList{"epub"});
    }
};

QTEST_GUILESS_MAIN(SpaceFilesTests)
#include "spacefiles_tests.moc"
