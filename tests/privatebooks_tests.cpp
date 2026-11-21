// Books a plugin keeps private (#127): a notebook stays in its owner's own
// collections and travels between their own computers, but nothing of it goes
// into a shared or a published space, whether the collection holds it by hand
// or by its rule, until its owner lets that one book go; and kept private
// again, a published space takes it back. What a plugin keeps private is said
// in the library's log, so it stays so once the plugin is removed.

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "collectionshare.h"
#include "fixtures.h"
#include "library.h"
#include "opkinds.h"
#include "person.h"
#include "pluginprivacy.h"
#include "privatebooks.h"
#include "publishedspace.h"
#include "replication.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"

namespace {

using Fixtures::whyNot;

[[nodiscard]] qint64 paperBook(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Bo Writer"};
    return library.addEntry(meta, true).id;
}

// Whether any of the operations names the book: by its id, as a collection's
// member, or by its title.
[[nodiscard]] bool names(const QList<SpaceOperation> &ops, const QString &gid, const QString &title) {
    return std::ranges::any_of(ops, [&](const SpaceOperation &op) {
        return op.id == gid || op.field == "book:" + gid || op.value.contains(title) || op.value.contains(gid.mid(2));
    });
}

constexpr PublishedSpace::Parts everything{
    .graph = true, .questions = true, .ratings = true, .reading = true, .files = true, .annotations = true};

// A plugin's folder whose plugin.json says which of its books are private.
void plugin(const QString &folder, const QString &id, const QJsonArray &kept) {
    QVERIFY(QDir().mkpath(folder + "/" + id));
    QVERIFY(Fixtures::write(folder + "/" + id + "/plugin.json",
                            QJsonDocument(QJsonObject{{"name", "Notebooks"}, {"script", "main.js"}, {"private", kept}}).toJson()));
}

} // namespace

class PrivateBooksTest : public QObject {
    Q_OBJECT

private slots:
    void aPluginSaysOnceThatItsBooksArePrivateAndItStaysSoWhenRemoved() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString plugins = dir.filePath("plugins");
        // One of Omabook's own formats is no plugin's to keep, nor is a name that is no extension.
        plugin(plugins, "notebooks", {"OmaNotebook", "epub", "../x"});
        QCOMPARE(PluginPrivacy::record(library, plugins), 1);
        QCOMPARE(PluginPrivacy::record(library, plugins), 0);
        const QList<PrivateBooks::Kind> kinds = PrivateBooks::kinds(library);
        QCOMPARE(kinds.size(), 1);
        QCOMPARE(kinds.first().format, QString("omanotebook"));
        QCOMPARE(kinds.first().plugin, QString("notebooks"));
        QCOMPARE(kinds.first().title, QString("Notebooks"));
        // Said in the personal space, it goes to one's other computers, and is known there.
        const QList<SpaceOperation> said = SpaceReplay::operations(library);
        const auto kind = std::ranges::find_if(said, [](const SpaceOperation &op) { return op.entity == "private"; });
        QVERIFY(kind != said.end());
        QVERIFY(kind->space.startsWith("personal:"));
        QVERIFY(Replication::travels(*kind));
        QVERIFY(OpKinds::known(*kind));
        // The plugin removed, its books stay private.
        QVERIFY(QDir(plugins + "/notebooks").removeRecursively());
        QCOMPARE(PluginPrivacy::record(library, plugins), 0);
        QCOMPARE(PrivateBooks::kinds(library).size(), 1);
        const qint64 notebook = Fixtures::privateNotebook(library, dir.path(), "Ideas");
        QVERIFY(notebook > 0);
        QCOMPARE(PrivateBooks::withheld(library, library.books()), QSet<qint64>{notebook});
    }

    void onlyABookAPluginKeepsPrivateIsChosenFor() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 notebook = Fixtures::privateNotebook(library, dir.path(), "Ideas");
        const qint64 paper = paperBook(library, "The Hobbit");
        const auto notPrivate = PrivateBooks::share(library, paper, true);
        QVERIFY(!notPrivate);
        QCOMPARE(notPrivate.error().code(), Failure::Code::NotKeptPrivate);
        const auto missing = PrivateBooks::share(library, 9999, true);
        QVERIFY(!missing);
        QCOMPARE(missing.error().code(), Failure::Code::NoSuchBook);
        QVERIFY(!PrivateBooks::shared(library, notebook));
        QVERIFY2(PrivateBooks::share(library, notebook, true), "let go");
        QVERIFY(PrivateBooks::shared(library, notebook));
        QVERIFY(PrivateBooks::withheld(library, library.books()).isEmpty());
        QVERIFY2(PrivateBooks::share(library, notebook, false), "kept again");
        QVERIFY(!PrivateBooks::shared(library, notebook));
    }

    void aPublishedSpaceLeavesItOutUntilLetGoAndTakesItBackWhenKeptAgain() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        library.usePersonFile(dir.filePath("home/person.key"));
        QVERIFY(library.open());
        const qint64 notebook = Fixtures::privateNotebook(library, dir.path(), "Ideas");
        const qint64 hobbit = paperBook(library, "The Hobbit");
        const auto desk = library.addCollection("Desk");
        QVERIFY(desk && library.putInCollection(*desk, notebook, true) && library.putInCollection(*desk, hobbit, true));
        // It stays in the owner's own collection.
        QCOMPARE(library.collection(*desk)->of(library.books()).size(), 2);
        const QString gid = Spaces::bookId(library, notebook);

        // With every part on, nothing of it goes, not even that the collection holds it.
        const auto ops = PublishedSpace::operations(library, *desk, everything, "published:test");
        QVERIFY2(ops, qPrintable(whyNot(ops)));
        QVERIFY(!names(*ops, gid, "Ideas"));
        QVERIFY(names(*ops, Spaces::bookId(library, hobbit), "The Hobbit"));
        // And publishing says why: it was put in by hand, and its plugin keeps it private.
        const auto shown = CollectionShare::preview(library, *library.collection(*desk), everything);
        QVERIFY2(shown, qPrintable(whyNot(shown)));
        QCOMPARE(shown->value("in").toObject().value("books").toInt(), 1);
        QCOMPARE(shown->value("files").toInt(), 0);
        const QJsonArray kept = shown->value("kept_private").toArray();
        QCOMPARE(kept.size(), 1);
        QCOMPARE(kept.first()["title"].toString(), QString("Ideas"));
        QVERIFY(!kept.first()["by_rule"].toBool());
        QVERIFY(!kept.first()["shared"].toBool());
        QVERIFY(CollectionShare::keptPrivate(kept).first().startsWith("Kept out: Ideas is in the collection, but Notebooks"));

        const SpaceSnapshot::Owner owner{.name = "Ada", .person = library.person()->id()};
        const auto published = PublishedSpace::publish(library, *desk, everything, owner);
        QVERIFY2(published, qPrintable(whyNot(published)));
        // Let go, the next time the space follows the collection it is in.
        QVERIFY2(PrivateBooks::share(library, notebook, true), "let go");
        auto snapshot = PublishedSpace::snapshotOf(library, published->space);
        QVERIFY2(snapshot, qPrintable(whyNot(snapshot)));
        ReducedState state = SpaceReducer::reduce(snapshot->operations);
        QVERIFY(state.books.contains(gid));
        QVERIFY(!SpaceReducer::isRemoved(state.books.value(gid).alive, state.books.value(gid).removed));
        QVERIFY(CollectionShare::preview(library, *library.collection(*desk), everything)
                    ->value("kept_private")
                    .toArray()
                    .first()["shared"]
                    .toBool());
        // Kept private again, the space takes it back.
        QVERIFY2(PrivateBooks::share(library, notebook, false), "kept again");
        snapshot = PublishedSpace::snapshotOf(library, published->space);
        QVERIFY2(snapshot, qPrintable(whyNot(snapshot)));
        state = SpaceReducer::reduce(snapshot->operations);
        QVERIFY(!state.books.contains(gid) || SpaceReducer::isRemoved(state.books.value(gid).alive, state.books.value(gid).removed));
    }

    void aRuleThatTakesItInSaysWhyItStaysOut() {
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 notebook = Fixtures::privateNotebook(library, dir.path(), "Ideas");
        QVERIFY(notebook > 0);
        const auto rule = library.addCollection("Everything mine", "format:omanotebook");
        QVERIFY(rule);
        QVERIFY(library.collection(*rule)->contains(*library.book(notebook)));
        const auto shown = CollectionShare::preview(library, *library.collection(*rule), everything);
        QVERIFY2(shown, qPrintable(whyNot(shown)));
        QCOMPARE(shown->value("in").toObject().value("books").toInt(), 0);
        const QJsonArray kept = shown->value("kept_private").toArray();
        QCOMPARE(kept.size(), 1);
        QVERIFY(kept.first()["by_rule"].toBool());
        const QString said = CollectionShare::keptPrivate(kept).first();
        QVERIFY2(said.contains("matches the collection's rule") && said.contains("omabook spaces private"), qPrintable(said));
    }

    void aSharedSpaceLeavesItOutUntilItsOwnerLetsItGo() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("Share");
        QVERIFY(QDir().mkpath(folder));
        Library ada(dir.filePath("ada/Library"));
        ada.usePersonFile(dir.filePath("ada/home/omabook/person.key"));
        QVERIFY(ada.open());
        const qint64 notebook = Fixtures::privateNotebook(ada, dir.path(), "Ideas");
        const qint64 hobbit = paperBook(ada, "The Hobbit");
        const auto shelf = ada.addCollection("Our books");
        QVERIFY(shelf && ada.putInCollection(*shelf, notebook, true) && ada.putInCollection(*shelf, hobbit, true));
        const auto created = SharedSpaces::create(ada, "Home", {"graph", "annotations"}, "Our books");
        QVERIFY2(created, qPrintable(whyNot(created)));
        const QString space = created->space.space;
        const QString gid = Spaces::bookId(ada, notebook);
        auto synced = SharedSpaces::sync(ada, space, folder);
        QVERIFY2(synced, qPrintable(whyNot(synced)));
        QList<SpaceOperation> held = SharedSpace::state(ada, space).accepted;
        QVERIFY(!names(held, gid, "Ideas"));
        QVERIFY(std::ranges::any_of(held, [](const SpaceOperation &op) { return op.value == "The Hobbit"; }));
        // Let go, it goes in at the next sync.
        QVERIFY2(PrivateBooks::share(ada, notebook, true), "let go");
        synced = SharedSpaces::sync(ada, space, folder);
        QVERIFY2(synced, qPrintable(whyNot(synced)));
        held = SharedSpace::state(ada, space).accepted;
        QVERIFY(std::ranges::any_of(held, [](const SpaceOperation &op) { return op.value == "Ideas"; }));
    }
};

QTEST_GUILESS_MAIN(PrivateBooksTest)
#include "privatebooks_tests.moc"
