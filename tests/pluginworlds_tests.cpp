// A plugin's worlds in spaces (#65): what a collection takes of a plugin's
// when it is published or shared (omabook.sharing.register), the worlds
// shared with a library as layers (omabook.graph.layers), and a plugin's own
// book put into a shared space (omabook.spaces.add). A stand-in plugin keeps
// a world of a book it made, and a plan beside it.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <optional>

#include "changelog.h"
#include "collectionshare.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcut.h"
#include "library.h"
#include "publishedspace.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "scriptsharing.h"
#include "sharedspace.h"
#include "sharedspaces.h"
#include "spacefiles.h"
#include "spaces.h"
#include "spaceworlds.h"
#include "subscriptions.h"
#include "tenancy.h"
#include "tenants.h"

namespace {

using Fixtures::whyNot;

// A storyteller of sorts: a world of the book it says it made, as met there
// chapter by chapter, and a plan with what is still to come.
constexpr const char *teller = R"(
var offered = [], asked = null;
omabook.sharing.register({worlds: function (context) { asked = context; return offered; }});
function build(scope, book) {
    const ilse = omabook.graph.upsert({scope: scope, kind: "character", name: "Ilse", summary: "A cartographer.", book: book, section: 0, fraction: 0});
    const hollow = omabook.graph.upsert({scope: scope, kind: "place", name: "Hollow", summary: "A city under the sea.", book: book, section: 2, fraction: 0});
    omabook.graph.link(ilse, hollow, "was born in");
    omabook.graph.upsert({scope: scope + ":plan", kind: "twist", name: "PLANSECRET", summary: "Ilse drew the sea in.", book: book, section: 0, fraction: 0});
    return true;
}
function offer(list) { offered = list; return true; }
function context() { return asked; }
function layers() { return omabook.graph.layers(); }
function scopes() { return omabook.graph.scopes(); }
function names(scope) { return omabook.graph.whole(scope).nodes.map(function (node) { return node.name; }).sort(); }
function relations(scope) { return omabook.graph.whole(scope).edges.map(function (edge) { return edge.kind; }); }
function copy(from, to) { return omabook.graph.copyScope(from, to); }
function write(scope) { return omabook.graph.upsert({scope: scope, kind: "place", name: "Nowhere"}); }
function add(space, book) { omabook.spaces.add(space, book); return true; }
// A plan as a storyteller lays it: things of no book and the relations
// between them, then one learned in the story's first chapter, then another.
function plan(scope, book) {
    const ilse = omabook.graph.upsert({scope: scope, kind: "character", name: "Ilse"});
    const teodor = omabook.graph.upsert({scope: scope, kind: "character", name: "Teodor"});
    const court = omabook.graph.upsert({scope: scope, kind: "group", name: "Tide Court"});
    const market = omabook.graph.upsert({scope: scope, kind: "place", name: "Market"});
    omabook.graph.link(teodor, ilse, "godfather of");
    omabook.graph.link(ilse, court, "keeps tallies for");
    omabook.graph.link(teodor, court, "sits in");
    omabook.graph.link(ilse, market, "works in", {book: book, section: 0, fraction: 0.5});
    omabook.graph.link(ilse, teodor, "owes");
    return true;
}
)";

void writePlugin(const QString &folder) {
    QVERIFY(QDir().mkpath(folder + "/teller"));
    const QJsonObject manifest{{"name", "Teller"}, {"version", "1.0"}, {"script", "main.js"}};
    QVERIFY(Fixtures::write(folder + "/teller/plugin.json", QJsonDocument(manifest).toJson()));
    QVERIFY(Fixtures::write(folder + "/teller/main.js", teller));
}

[[nodiscard]] bool threeChapters(const QString &path, const QString &title) {
    return Fixtures::chapteredEpub(
        path, title, {{"One", "<p>Ilse drew maps.</p>"}, {"Two", "<p>The sea rose.</p>"}, {"Three", "<p>Hollow, below.</p>"}});
}

// One person's computer, with the stand-in plugin running on its library.
struct Someone {
    Someone(const QTemporaryDir &dir, const QString &name)
        : folder(dir.filePath(name + "/plugins")), library(dir.filePath(name + "/Library")) {
        library.usePersonFile(dir.filePath(name + "/home/omabook/person.key"));
        QVERIFY(library.open());
        writePlugin(folder);
        ScriptServices services;
        services.library = [this] { return &library; };
        plugins = std::make_unique<ScriptPlugins>(services, folder, nullptr, false);
        QVERIFY(plugins->plugin("teller") != nullptr);
    }

    // A book the plugin made (as library.create marks one), from a copy of `file`.
    qint64 story(const QString &file, const QString &as) {
        const QString copy = QFileInfo(library.root()).dir().filePath(as);
        if (!QFile::exists(copy) && !QFile::copy(file, copy))
            return 0;
        const qint64 book = library.add(copy).id;
        return book > 0 && library.setIdentifier(book, "generated", "teller") ? book : 0;
    }

    QJsonValue call(const QString &function, const QJsonArray &arguments = {}) {
        QString problem;
        const QJsonValue answer = plugins->plugin("teller")->callNow(function, arguments, &problem);
        last = problem;
        return answer;
    }

    // What the plugin answers when it is asked what goes.
    bool offer(const QJsonArray &worlds) { return call("offer", QJsonArray{QJsonValue(worlds)}).toBool(); }

    QString folder;
    Library library;
    std::unique_ptr<ScriptPlugins> plugins;
    QString last; // why the last call failed
};

// The scopes the operations of a space name, and the names of its things.
[[nodiscard]] QSet<QString> scopesIn(const QList<SpaceOperation> &ops) {
    QSet<QString> scopes;
    for (const SpaceOperation &op : ops)
        if (op.entity == "node" && op.field == "scope")
            scopes << op.value;
    return scopes;
}

[[nodiscard]] QSet<QString> namesIn(const QList<SpaceOperation> &ops) {
    QSet<QString> names;
    for (const SpaceOperation &op : ops)
        if (op.entity == "node" && op.field == "name")
            names << op.value;
    return names;
}

[[nodiscard]] QJsonArray strings(const QStringList &list) { return QJsonArray::fromStringList(list); }

// The book called `title` in the library, if it is there.
[[nodiscard]] std::optional<Book> titled(const Library &library, const QString &title) {
    for (const Book &book : library.books())
        if (book.meta.title == title)
            return book;
    return std::nullopt;
}

// The hash of the book's one file, or nothing when it has none.
[[nodiscard]] QString hashOf(const Library &library, qint64 book) {
    const QList<FormatFile> files = library.formatFiles(book);
    return files.size() == 1 ? files.first().hash : QString();
}

// The relations a plugin reads of a scope, sorted.
[[nodiscard]] QStringList relationsOf(Someone &someone, const QString &scope) {
    QStringList kinds = someone.call("relations", {scope}).toVariant().toStringList();
    kinds.sort();
    return kinds;
}

} // namespace

class PluginWorldsTests : public QObject {
    Q_OBJECT

private slots:
    // Publishing a collection takes of the plugin's only the world it made,
    // of a book it made, with the graph, as far as the owner has read; never
    // a person's book, a scope it didn't make, or a plan it didn't name.
    void aCollectionTakesOnlyThePluginsOwnWorld() {
        QTemporaryDir dir;
        Someone ann(dir, "Ann");
        QVERIFY(threeChapters(dir.filePath("story.epub"), "The Drowned Map"));
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "By A Person"));
        const qint64 story = ann.story(dir.filePath("story.epub"), "story.epub");
        const qint64 person = ann.library.add(dir.filePath("person.epub")).id;
        QVERIFY(story > 0 && person > 0);
        QCOMPARE(ann.call("build", {"world:s1", story}).toBool(), true);
        // A scope the person made, and one of a book by a person.
        Graph(ann.library).upsert("world:theirs", "place", "Elsewhere", "Not the plugin's.", {}, GraphPlace{story, 0, 0});
        QCOMPARE(SpaceWorlds::maker(ann.library, "world:s1"), QString("teller"));
        QCOMPARE(SpaceWorlds::maker(ann.library, "world:theirs"), QString());
        ann.library.setProgress(story, {1, 0.5, 50});
        const auto shelf = ann.library.addCollection("Shelf");
        QVERIFY(shelf && ann.library.putInCollection(*shelf, story, true) && ann.library.putInCollection(*shelf, person, true));
        QCOMPARE(
            ann.offer(QJsonArray{QJsonObject{{"book", story}, {"scope", "world:s1"}}, QJsonObject{{"book", person}, {"scope", "world:s1"}},
                                 QJsonObject{{"book", story}, {"scope", "world:theirs"}}}),
            true);

        const QString file = dir.filePath("shelf.omabook-space");
        const auto publish = [&](const PublishedSpace::Parts &parts) {
            return CollectionShare::publish(ann.library, *ann.library.collection(*shelf), parts,
                                            {.as = "Ann", .output = file, .worlds = worldsOf(*ann.plugins)});
        };
        const auto done = publish({.graph = true});
        QVERIFY2(done.has_value(), qPrintable(whyNot(done)));
        const QJsonObject asked = ann.call("context").toObject();
        QCOMPARE(asked.value("kind").toString(), QString("published"));
        QCOMPARE(asked.value("space").toString(), done->value("space").toString());
        QCOMPARE(asked.value("books").toArray().size(), 2);
        QVERIFY(asked.value("books").toArray().contains(story) && asked.value("books").toArray().contains(person));
        QCOMPARE(asked.value("parts").toArray(), strings({"graph"}));
        const QJsonObject worlds = done->value("worlds").toObject();
        QVERIFY2(worlds.value("worlds").toArray().size() == 1, QJsonDocument(worlds).toJson().constData());
        QCOMPARE(worlds.value("worlds").toArray().at(0).toObject().value("scope").toString(), QString("world:s1"));
        const QJsonArray left = worlds.value("left").toArray();
        QCOMPARE(left.size(), 2);
        QVERIFY(left.at(0).toObject().value("why").toString().contains("a book by a person never goes"));
        QVERIFY(left.at(1).toObject().value("why").toString().contains("not a scope teller made"));

        auto snapshot = PublishedSpace::snapshotOf(ann.library, done->value("space").toString());
        QVERIFY2(snapshot.has_value(), qPrintable(whyNot(snapshot)));
        QCOMPARE(scopesIn(snapshot->operations), QSet<QString>{"world:s1"});
        // Ilse was met in the first chapter; Hollow only in the third, which Ann hasn't reached.
        QVERIFY(namesIn(snapshot->operations).contains("Ilse"));
        QVERIFY(!namesIn(snapshot->operations).contains("Hollow"));
        QVERIFY(!namesIn(snapshot->operations).contains("PLANSECRET") && !namesIn(snapshot->operations).contains("Elsewhere"));
        const auto declared = SpaceWorlds::declared(snapshot->operations);
        QCOMPARE(declared.size(), 1);
        QCOMPARE(declared.first().plugin, QString("teller"));

        // Without the graph, the world stays home, and what went is taken back.
        QVERIFY(publish({}).has_value());
        snapshot = PublishedSpace::snapshotOf(ann.library, done->value("space").toString());
        QVERIFY(snapshot.has_value());
        QVERIFY(SpaceWorlds::declared(snapshot->operations).isEmpty());
        QVERIFY(scopesIn(snapshot->operations).isEmpty());
        QVERIFY(!namesIn(snapshot->operations).contains("Ilse"));
    }

    // A subscriber's plugin gets the world as a layer: named apart, among the
    // scopes, read whole and copied as far as the subscriber has read the
    // book, and never written to.
    void aSubscriberGetsTheWorldAsALayerCutAtTheirPlace() {
        QTemporaryDir dir;
        Someone ann(dir, "Ann");
        QVERIFY(threeChapters(dir.filePath("story.epub"), "The Drowned Map"));
        const qint64 story = ann.story(dir.filePath("story.epub"), "story.epub");
        QVERIFY(story > 0);
        QVERIFY(ann.call("build", {"world:s1", story}).toBool());
        ann.library.setProgress(story, {2, 1.0, 100});
        const auto shelf = ann.library.addCollection("Shelf");
        QVERIFY(shelf && ann.library.putInCollection(*shelf, story, true));
        QVERIFY(ann.offer(QJsonArray{QJsonObject{{"book", story}, {"scope", "world:s1"}}}));
        const QString file = dir.filePath("shelf.omabook-space");
        const auto done = CollectionShare::publish(ann.library, *ann.library.collection(*shelf), {.graph = true},
                                                   {.as = "Ann", .output = file, .worlds = worldsOf(*ann.plugins)});
        QVERIFY2(done.has_value(), qPrintable(whyNot(done)));

        Someone bo(dir, "Bo");
        // A scope of Bo's own by the same name is no layer.
        Graph(bo.library).upsert("world:s1", "place", "Bo's Own", "", {});
        const qint64 mine = bo.story(dir.filePath("story.epub"), "mine.epub");
        QVERIFY(mine > 0);
        const auto fetched = Subscriptions::fetch(file);
        QVERIFY2(fetched.has_value(), qPrintable(whyNot(fetched)));
        QVERIFY(Subscriptions::subscribe(bo.library, *fetched, {.as = "From Ann", .dryRun = false}).has_value());
        const QJsonArray layers = bo.call("layers").toArray();
        QCOMPARE(layers.size(), 1);
        const QJsonObject layer = layers.at(0).toObject();
        const QString scope = layer.value("scope").toString();
        QCOMPARE(layer.value("of").toString(), QString("world:s1"));
        QCOMPARE(layer.value("kind").toString(), QString("published"));
        QCOMPARE(layer.value("role").toString(), QString("reader"));
        QCOMPARE(layer.value("owner").toString(), QString("Ann"));
        QCOMPARE(layer.value("plugin").toString(), QString("teller"));
        QCOMPARE(layer.value("title").toString(), QString("The Drowned Map"));
        QCOMPARE(layer.value("book").toInteger(), mine);
        QVERIFY(scope != "world:s1" && SpaceWorlds::isLayerName(scope));
        QVERIFY(bo.call("scopes").toArray().contains(scope));
        // Nothing of it before Bo begins the book; then what was met where Bo is, and nothing later.
        QCOMPARE(bo.call("names", {scope}).toArray(), QJsonArray());
        bo.library.setProgress(mine, {0, 0.9, 0}); // a place with no percent said is begun too
        QCOMPARE(bo.call("names", {scope}).toArray(), strings({"Ilse"}));
        bo.library.setProgress(mine, {2, 0.5, 90});
        QCOMPARE(bo.call("names", {scope}).toArray(), strings({"Hollow", "Ilse"}));
        QCOMPARE(bo.call("relations", {scope}).toArray(), strings({"was born in"}));
        // The command line and the window show it the same (graph show --scope).
        QCOMPARE(GraphCut::whole(Graph::ofLibrary(bo.library), scope, 50).view.nodes.size(), 2);
        QCOMPARE(bo.call("names", {"world:s1"}).toArray(), strings({"Bo's Own"}));

        // Copied into a world of Bo's plugin's own; the layer itself is never written to.
        QVERIFY(bo.call("copy", {scope, "world:b1"}).toBool());
        QCOMPARE(bo.call("names", {"world:b1"}).toArray(), strings({"Hollow", "Ilse"}));
        QCOMPARE(SpaceWorlds::maker(bo.library, "world:b1"), QString("teller"));
        bo.call("write", {scope});
        QVERIFY(bo.last.contains("can be read and copied, not written to"));
        QVERIFY(!Graph(bo.library).scopes().contains(scope));
        QVERIFY(!bo.call("copy", {"world:b1", scope}).toBool());
        QCOMPARE(bo.call("names", {scope}).toArray(), strings({"Hollow", "Ilse"}));
    }

    // In a shared space a writer's plugin puts a book of its own in, with its
    // world, which the others get as a layer once it is synced; a reader's
    // can't, nor anyone a book the plugin didn't make.
    void aWriterPutsAStoryOfTheirOwnIntoASharedSpace() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("space-folder");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(threeChapters(dir.filePath("story.epub"), "The Drowned Map"));
        QVERIFY(threeChapters(dir.filePath("theirs.epub"), "The Lantern Ferry"));
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "By A Person"));
        Someone ann(dir, "Ann");
        Someone bo(dir, "Bo");
        Someone cy(dir, "Cy");
        const qint64 story = ann.story(dir.filePath("story.epub"), "story.epub");
        QVERIFY(story > 0 && ann.call("build", {"world:s1", story}).toBool());
        const auto shelf = ann.library.addCollection("Shelf");
        QVERIFY(shelf && ann.library.putInCollection(*shelf, story, true));
        QVERIFY(ann.offer(QJsonArray{QJsonObject{{"book", story}, {"scope", "world:s1"}}}));
        const auto created = SharedSpaces::create(ann.library, "Shelf", {"graph"}, "Shelf", worldsOf(*ann.plugins));
        QVERIFY2(created.has_value(), qPrintable(whyNot(created)));
        const QString space = created->space.space;
        QCOMPARE(ann.call("context").toObject().value("kind").toString(), QString("shared"));
        for (Someone *member : {&bo, &cy}) {
            const auto invited = SharedSpaces::invite(ann.library, space, member->library.person()->id(),
                                                      member == &bo ? SharedSpace::Role::Writer : SharedSpace::Role::Reader,
                                                      dir.filePath(member->library.person()->id().left(8)));
            QVERIFY2(invited.has_value(), qPrintable(whyNot(invited)));
            QVERIFY(SharedSpaces::sync(ann.library, space, folder).has_value());
            QVERIFY(SharedSpaces::join(member->library, invited->file).has_value());
            QVERIFY(SharedSpaces::sync(member->library, space, folder).has_value());
        }

        // Ann's world is a layer at Bo's, who writes the space; none of her own at hers.
        QJsonArray layers = bo.call("layers").toArray();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers.at(0).toObject().value("kind").toString(), QString("shared"));
        QCOMPARE(layers.at(0).toObject().value("role").toString(), QString("writer"));
        QCOMPARE(layers.at(0).toObject().value("of").toString(), QString("world:s1"));
        QCOMPARE(ann.call("layers").toArray(), QJsonArray());
        QCOMPARE(cy.call("layers").toArray().at(0).toObject().value("role").toString(), QString("reader"));

        // Bo's own story, with a world of its own, goes in; a person's book doesn't, nor anything from a reader.
        const qint64 theirs = bo.story(dir.filePath("theirs.epub"), "theirs.epub");
        QVERIFY(theirs > 0 && bo.call("build", {"world:b2", theirs}).toBool());
        bo.library.setProgress(theirs, {2, 1.0, 100});
        QVERIFY(bo.offer(QJsonArray{QJsonObject{{"book", theirs}, {"scope", "world:b2"}}}));
        const qint64 person = bo.library.add(dir.filePath("person.epub")).id;
        bo.call("add", {space, person});
        QVERIFY(bo.last.contains("Only a book this plugin made"));
        const qint64 cys = cy.story(dir.filePath("theirs.epub"), "cys.epub");
        cy.call("add", {space, cys});
        QVERIFY(cy.last.contains("only its writers add to it"));
        QCOMPARE(bo.call("add", {space, theirs}).toBool(), true);
        QVERIFY2(bo.last.isEmpty(), qPrintable(bo.last));
        QVERIFY(bo.library.collectionNamed("Shelf")->contains(*bo.library.book(theirs)));
        QVERIFY(SharedSpaces::sync(bo.library, space, folder).has_value());
        QVERIFY(SharedSpaces::sync(ann.library, space, folder).has_value());

        layers = ann.call("layers").toArray();
        QCOMPARE(layers.size(), 1);
        QCOMPARE(layers.at(0).toObject().value("of").toString(), QString("world:b2"));
        QCOMPARE(layers.at(0).toObject().value("title").toString(), QString("The Lantern Ferry"));
        QCOMPARE(layers.at(0).toObject().value("role").toString(), QString("writer"));
        QCOMPARE(bo.call("layers").toArray().size(), 1);
        const QList<SpaceOperation> held = SharedSpace::state(ann.library, space).accepted;
        QVERIFY(namesIn(held).contains("Hollow"));
        QVERIFY(!namesIn(held).contains("PLANSECRET"));
    }

    // A book the plugin made and marked to go with its world takes its file
    // to the members of a shared space whose files switch is off, and only
    // it: a person's book never goes so, not even when a writer says its
    // world goes with its file (#107).
    void aStorysFileGoesWithItsWorldToASharedSpace() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("space-folder");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(threeChapters(dir.filePath("story.epub"), "The Drowned Map"));
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "By A Person"));
        Someone ann(dir, "Ann");
        Someone bo(dir, "Bo");
        const qint64 story = ann.story(dir.filePath("story.epub"), "story.epub");
        const qint64 person = ann.library.add(dir.filePath("person.epub")).id;
        QVERIFY(story > 0 && person > 0 && ann.call("build", {"world:s1", story}).toBool());
        const auto shelf = ann.library.addCollection("Shelf");
        QVERIFY(shelf && ann.library.putInCollection(*shelf, story, true) && ann.library.putInCollection(*shelf, person, true));
        QVERIFY(ann.offer(QJsonArray{QJsonObject{{"book", story}, {"scope", "world:s1"}, {"file", true}},
                                     QJsonObject{{"book", person}, {"file", true}}}));
        const auto created = SharedSpaces::create(ann.library, "Shelf", {"graph"}, "Shelf", worldsOf(*ann.plugins));
        QVERIFY2(created.has_value(), qPrintable(whyNot(created)));
        const QString space = created->space.space;
        QVERIFY(!SpaceFiles::on(ann.library, space));
        const auto invited =
            SharedSpaces::invite(ann.library, space, bo.library.person()->id(), SharedSpace::Role::Writer, dir.filePath("invite-bo"));
        QVERIFY2(invited.has_value(), qPrintable(whyNot(invited)));
        const auto annSynced = SharedSpaces::sync(ann.library, space, folder);
        QVERIFY2(annSynced.has_value(), qPrintable(whyNot(annSynced)));
        QCOMPARE(annSynced->files, 1);
        QVERIFY(SharedSpaces::join(bo.library, invited->file).has_value());
        const auto boSynced = SharedSpaces::sync(bo.library, space, folder);
        QVERIFY2(boSynced.has_value(), qPrintable(whyNot(boSynced)));

        // Bo has the story with its file, the one Ann has, and the person's book without one.
        QCOMPARE(boSynced->fetched, 1);
        const std::optional<Book> theirs = titled(bo.library, "The Drowned Map");
        QVERIFY(theirs.has_value());
        QCOMPARE(theirs->formats, QStringList{"epub"});
        QCOMPARE(hashOf(bo.library, theirs->id), hashOf(ann.library, story));
        const std::optional<Book> notTheirs = titled(bo.library, "By A Person");
        QVERIFY(notTheirs.has_value());
        QVERIFY(notTheirs->formats.isEmpty());
        QVERIFY(SpaceFiles::of(bo.library, notTheirs->id).isEmpty());
        QCOMPARE(bo.call("layers").toArray().at(0).toObject().value("book").toInteger(), theirs->id);

        // A writer who says the person's book goes with a world, and its file with it, carries nothing of it.
        const QString personId = Spaces::bookId(ann.library, person);
        QJsonArray formats;
        for (const FormatFile &one : ann.library.formatFiles(person))
            formats << QJsonObject{{"format", one.format}, {"hash", one.hash}, {"size", one.size}};
        const QString said = QString::fromUtf8(QJsonDocument(formats).toJson(QJsonDocument::Compact));
        QList<ChangeLog::Entry> forged{{.space = space, .entity = "book", .id = personId, .field = "formats", .value = said, .hlc = {}}};
        for (const auto &[field, value] :
             {std::pair<QString, QString>{"scope", "world:s1"}, {"plugin", "teller"}, {"book", personId}, {"file", "1"}})
            forged << ChangeLog::Entry{.space = space, .entity = "world", .id = "w:forged", .field = field, .value = value, .hlc = {}};
        QVERIFY(SharedSpace::write(ann.library, space, forged).has_value());
        const auto again = SharedSpaces::sync(ann.library, space, folder);
        QVERIFY2(again.has_value(), qPrintable(whyNot(again)));
        QCOMPARE(again->files, 0);
        const auto boAgain = SharedSpaces::sync(bo.library, space, folder);
        QVERIFY2(boAgain.has_value(), qPrintable(whyNot(boAgain)));
        QCOMPARE(boAgain->fetched, 0);
        QVERIFY(SpaceFiles::of(bo.library, notTheirs->id).isEmpty());
        QVERIFY(bo.library.book(notTheirs->id)->formats.isEmpty());
    }

    // Published to a file with no book files, the story's file still goes
    // beside it when the plugin marked it, and a subscriber has it (#107).
    void aStorysFileGoesBesideAPublishedSpacesFile() {
        QTemporaryDir dir;
        Someone ann(dir, "Ann");
        QVERIFY(threeChapters(dir.filePath("story.epub"), "The Drowned Map"));
        QVERIFY(Fixtures::epub(dir.filePath("person.epub"), "By A Person"));
        const qint64 story = ann.story(dir.filePath("story.epub"), "story.epub");
        const qint64 person = ann.library.add(dir.filePath("person.epub")).id;
        QVERIFY(story > 0 && person > 0 && ann.call("build", {"world:s1", story}).toBool());
        const auto shelf = ann.library.addCollection("Shelf");
        QVERIFY(shelf && ann.library.putInCollection(*shelf, story, true) && ann.library.putInCollection(*shelf, person, true));
        QVERIFY(ann.offer(QJsonArray{QJsonObject{{"book", story}, {"scope", "world:s1"}, {"file", true}},
                                     QJsonObject{{"book", person}, {"file", true}}}));
        const QString file = dir.filePath("shelf.omabook-space");
        const auto done = CollectionShare::publish(ann.library, *ann.library.collection(*shelf), {.graph = true},
                                                   {.as = "Ann", .output = file, .worlds = worldsOf(*ann.plugins)});
        QVERIFY2(done.has_value(), qPrintable(whyNot(done)));
        QCOMPARE(done->value("files").toObject().value("copied").toInteger(), 1);
        const QStringList beside = QDir(SpaceFiles::besideOf(file)).entryList(QDir::Files);
        QCOMPARE(beside, QStringList{hashOf(ann.library, story) + ".epub"});

        Someone bo(dir, "Bo");
        const auto fetched = Subscriptions::fetch(file);
        QVERIFY2(fetched.has_value(), qPrintable(whyNot(fetched)));
        QVERIFY(Subscriptions::subscribe(bo.library, *fetched, {.as = "From Ann", .dryRun = false}).has_value());
        const QString space = done->value("space").toString();
        SpaceFiles::takenFrom(bo.library, space, file);
        QVERIFY(!SpaceFiles::on(bo.library, space));
        QCOMPARE(SpaceFiles::bringWorldFiles(bo.library, space), 1);
        const std::optional<Book> theirs = titled(bo.library, "The Drowned Map");
        QVERIFY(theirs.has_value());
        QCOMPARE(hashOf(bo.library, theirs->id), hashOf(ann.library, story));
        const std::optional<Book> notTheirs = titled(bo.library, "By A Person");
        QVERIFY(notTheirs.has_value() && notTheirs->formats.isEmpty());
        QVERIFY(SpaceFiles::of(bo.library, notTheirs->id).isEmpty());
    }

    // A plugin's plan keeps every relation it wrote, read back by the plugin,
    // when one of its things is learned in a book and so leaves what every
    // tenant sees for the book's tenant, while the others stay there: in a
    // library of no collections, and in one whose biggest collection is not
    // the story's; and the same once the membership is placed again whole.
    void aPluginsPlanKeepsTheRelationsItWrote_data() {
        QTest::addColumn<bool>("collections");
        QTest::newRow("no collections") << false;
        QTest::newRow("collections") << true;
    }
    void aPluginsPlanKeepsTheRelationsItWrote() {
        QFETCH(bool, collections);
        QTemporaryDir dir;
        Someone ann(dir, "Ann");
        QVERIFY(threeChapters(dir.filePath("story.epub"), "The Glass Tithe"));
        QVERIFY(Fixtures::epub(dir.filePath("one.epub"), "A Person's Book"));
        QVERIFY(Fixtures::epub(dir.filePath("two.epub"), "Another Person's Book"));
        const qint64 story = ann.story(dir.filePath("story.epub"), "story.epub");
        const qint64 one = ann.library.add(dir.filePath("one.epub")).id;
        const qint64 two = ann.library.add(dir.filePath("two.epub")).id;
        QVERIFY(story > 0 && one > 0 && two > 0);
        if (collections) {
            const auto stories = ann.library.addCollection("Stories");
            const auto study = ann.library.addCollection("Study");
            QVERIFY(stories && study);
            QVERIFY(ann.library.putInCollection(*stories, story, true));
            QVERIFY(ann.library.putInCollection(*study, one, true) && ann.library.putInCollection(*study, two, true));
        }
        const QStringList all{"godfather of", "keeps tallies for", "owes", "sits in", "works in"};
        QCOMPARE(ann.call("plan", {"world:s1:plan", story}).toBool(), true);
        QCOMPARE(relationsOf(ann, "world:s1:plan"), all);
        const QStringList kept = memberships(ann.library);
        QCOMPARE(rebuilt(ann.library), kept);
        QCOMPARE(relationsOf(ann, "world:s1:plan"), all);
        // The story moved to another collection, and back out of every one.
        if (collections) {
            const auto stories = ann.library.collectionNamed("Stories");
            const auto study = ann.library.collectionNamed("Study");
            QVERIFY(stories && study);
            QVERIFY(ann.library.putInCollection(stories->id, story, false) && ann.library.putInCollection(study->id, story, true));
            QCOMPARE(relationsOf(ann, "world:s1:plan"), all);
            QVERIFY(ann.library.putInCollection(study->id, story, false));
            QCOMPARE(relationsOf(ann, "world:s1:plan"), all);
            const QStringList moved = memberships(ann.library);
            QCOMPARE(rebuilt(ann.library), moved);
        }
    }
};

QTEST_MAIN(PluginWorldsTests)
#include "pluginworlds_tests.moc"
