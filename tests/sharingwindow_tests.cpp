// Publishing and subscribing in the window (#6): the publish dialog says what
// goes in, counted as the command line counts it, and each part of the
// user's own with its switch, the books' files too (#63), and writes the file; a file dropped
// on the window says what matched and what becomes a wish before anything is
// written; the collection a subscribed space fills is marked as someone
// else's, and unsubscribing from it keeps the collection. The keys are in the
// help. The dialogs are looked at at every size in layout_tests.

#include <QDir>
#include <QFile>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>
#include <optional>

#include "assistant.h"
#include "backend.h"
#include "collectionshare.h"
#include "customization.h"
#include "fixtures.h"
#include "graph.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "person.h"
#include "publishedspace.h"
#include "reader.h"
#include "settings.h"
#include "spacefiles.h"
#include "spacesnapshot.h"
#include "subscribedspace.h"
#include "theme.h"
#include "translator.h"

namespace {

class Picker : public FilePicker {
public:
    QList<Purpose> asked;
    void pick(Purpose purpose, const QString &) override { asked << purpose; }
};

// A transport of links into a folder, in place of GitHub: a snapshot sent is
// a file there, and its link names it.
class FolderLink final : public SpaceTransport {
public:
    explicit FolderLink(QString folder) : m_folder(std::move(folder)) {}
    [[nodiscard]] QString name() const override { return "test"; }
    [[nodiscard]] bool handles(const QString &address) const override { return address.startsWith(prefix); }
    [[nodiscard]] std::expected<SpaceSnapshot, QString> fetch(const QString &address) override {
        return SpaceSnapshotFile::load(m_folder + "/" + address.mid(prefix.size()));
    }
    [[nodiscard]] std::expected<Sent, QString> publish(const SpaceSnapshot &snapshot, const Sending &sending) override {
        const QString address = sending.previous.isEmpty() ? prefix + "shared.omabook-space" : sending.previous;
        if (const auto saved = SpaceSnapshotFile::save(snapshot, m_folder + "/" + address.mid(prefix.size())); !saved)
            return std::unexpected(saved.error());
        return Sent{.address = address, .created = sending.previous.isEmpty(), .open = sending.open, .note = {}};
    }

private:
    inline static const QString prefix = "https://link.test/";
    QString m_folder;
};

// The whole window on a temp folder, as in ui_tests.cpp, with no AI.
struct Desk {
    QTemporaryDir dir;
    Picker picker;
    std::optional<Translator> translator;
    std::optional<Theme> theme;
    std::optional<Customization> custom;
    std::optional<Look> look;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;
    std::optional<Assistant> assistant;
    std::optional<AiAgent> aiAgent;
    std::optional<Librarian> librarian;
    std::optional<Manual> manual;
    std::optional<QQmlEngine> engine;
    std::optional<Interface> interface;
    QStringList warnings;
    QQuickWindow *window = nullptr;

    Desk() {
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        Fixtures::write(dir.filePath("language"), "en\n");
        translator.emplace(dir.filePath("language"));
        QCoreApplication::installTranslator(&*translator);
        theme.emplace(dir.filePath("desktop"));
        custom.emplace(dir.filePath("custom"));
        look.emplace(&*theme, &*custom);
        settings.emplace(dir.filePath("config.json"));
        settings->setLibraryPath(dir.filePath("Library"));
        const auto noAi = [](QString *why) -> std::unique_ptr<AiProvider> {
            if (why)
                *why = "No AI is set up for this test.";
            return nullptr;
        };
        backend.emplace(&*settings, &picker, nullptr, noAi, Backend::Catalogs{}, dir.filePath("plugins"), "-");
        reader.emplace(&*backend, QString());
        assistant.emplace(&*reader, noAi);
        aiAgent.emplace(&*settings, dir.filePath("omarchy/defaults/agent"));
        librarian.emplace(&*backend, &*settings, &*custom, [this] { return look->colors(); });
        settings->setTour("done");
        settings->setPluginDiscovery(false);
        settings->setNewsSeen(OMABOOK_VERSION);
        manual.emplace(&*settings, &*custom);
    }
    ~Desk() {
        interface.reset();
        engine.reset();
        QCoreApplication::removeTranslator(&*translator);
    }

    bool show() {
        engine.emplace();
        QObject::connect(&*engine, &QQmlEngine::warnings, &*engine, [this](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                warnings << error.toString();
        });
        engine->addImageProvider("book", reader->imageProvider());
        const QList<std::pair<const char *, QObject *>> objects{
            {"translator", &*translator}, {"theme", &*look},      {"custom", &*custom}, {"librarian", &*librarian}, {"manual", &*manual},
            {"settings", &*settings},     {"backend", &*backend}, {"reader", &*reader}, {"assistant", &*assistant}, {"aiAgent", &*aiAgent}};
        for (const auto &[name, object] : objects)
            engine->rootContext()->setContextProperty(name, object);
        engine->rootContext()->setContextProperty("touchscreen", false);
        interface.emplace(&*engine, &*custom);
        if (!interface->load())
            return false;
        window = interface->window();
        return window && QTest::qWaitForWindowExposed(window);
    }

    // An object by its objectName, wherever it is (loaders and views don't
    // keep their items in the QObject tree).
    QObject *find(const QString &name) const {
        if (QObject *object = window->findChild<QObject *>(name))
            return object;
        QList<QQuickItem *> items{window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (item->objectName() == name)
                return item;
            if (QObject *object = item->findChild<QObject *>(name))
                return object;
            items << item->childItems();
        }
        return nullptr;
    }
    QVariant value(const QString &name, const char *property) const {
        QObject *object = find(name);
        return object ? object->property(property) : QVariant();
    }
    bool set(const QString &name, const char *property, const QVariant &to) {
        QObject *object = find(name);
        return object && object->setProperty(property, to);
    }
    bool click(const QString &name) {
        QObject *object = find(name);
        return object && object->property("enabled").toBool() && QMetaObject::invokeMethod(object, "clicked");
    }
    bool run(const QString &command) {
        QVariant ran;
        QMetaObject::invokeMethod(window, "run", Q_RETURN_ARG(QVariant, ran), Q_ARG(QVariant, command));
        return ran.toBool();
    }
    // With OMABOOK_SHOTS set to a folder, a picture of the window there.
    void shot(const QString &name) const {
        if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty()) {
            QTest::qWait(350); // let the popups finish their transitions
            window->grabWindow().save(folder + "/" + name + ".png");
        }
    }
};

[[nodiscard]] BookMeta meta(const QString &title, const QString &author) {
    BookMeta made;
    made.title = title;
    made.authors = {author};
    return made;
}

// Someone else's library, with a collection of two books to share: one the
// receiver has too, one they don't.
[[nodiscard]] QString sharedFile(const QString &folder) {
    Library owner(folder + "/Owner");
    if (!owner.open())
        return {};
    const qint64 tide = owner.addEntry(meta("The Long Tide", "Mira Okafor"), true).id;
    const qint64 hers = owner.addEntry(meta("Only Hers", "Ann Other"), true).id;
    const auto shelf = owner.addCollection("Tides");
    const auto person = Person::generate();
    if (!shelf || !person || !owner.putInCollection(*shelf, tide, true) || !owner.putInCollection(*shelf, hers, true))
        return {};
    const auto snapshot = PublishedSpace::snapshot(owner, *shelf, {}, {.name = "Ada Fixture", .person = person->id()});
    const QString file = folder + "/Tides.omabook-space";
    return snapshot && SpaceSnapshotFile::save(*snapshot, file) ? file : QString();
}

} // namespace

class SharingWindowTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void publishingSaysWhatGoesInAndWritesTheFile() {
        Desk desk;
        Library &library = *desk.backend->library();
        QVERIFY(Fixtures::epub(desk.dir.filePath("tide.epub"), "The Long Tide"));
        const qint64 tide = library.add(desk.dir.filePath("tide.epub")).id;
        const qint64 wish = library.addEntry(meta("Only Hers", "Ann Other"), false).id;
        const auto shelf = library.addCollection("Tides");
        QVERIFY(shelf && library.putInCollection(*shelf, tide, true) && library.putInCollection(*shelf, wish, true));
        QVERIFY(library.setReading(tide, "reading", 4, {}));
        library.setProgress(tide, {0, 0.5, 50, 0});
        Graph(library).upsert({}, "character", "Mara", "A ferry pilot.", {}, GraphPlace{tide, 0, 0.1});
        desk.backend->reload();
        desk.backend->setCollection(*shelf);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));

        QVERIFY(desk.run("library.publish"));
        QTRY_VERIFY(desk.value("publishDialog", "opened").toBool());
        QTRY_VERIFY(desk.value("publishDialog", "counted").toBool());
        // What goes in, never goes, and what is the user's own, counted as
        // `omabook collections share` counts it.
        const QVariantMap seen = desk.backend->spaces()->publishing();
        QCOMPARE(seen.value("books").toInt(), 2);
        QCOMPARE(seen.value("files").toInt(), 1);
        QCOMPARE(seen.value("ratings").toInt(), 1);
        QVERIFY(seen.value("places").toInt() >= 1);
        QVERIFY(desk.value("publishGoesIn", "text").toString().contains("2 books"));
        QVERIFY(desk.value("publishFiles", "text").toString().contains("files (1)"));
        // Counted in the singular and the plural, as the language has them.
        QCOMPARE(seen.value("things").toInt(), 1);
        QCOMPARE(seen.value("facts").toInt(), 1);
        QVERIFY2(desk.value("publishGraph", "text").toString().startsWith("The graph: 1 thing, 1 fact and 0 relations,"),
                 qPrintable(desk.value("publishGraph", "text").toString()));
        QVERIFY(desk.value("publishReading", "text").toString().contains("0 days"));
        QVERIFY(desk.value("publishRatings", "text").toString().contains("(1)"));
        // Every part of the user's own is off until switched on, and the
        // first time it asks the name the user goes by.
        for (const QString part : {"publishGraph", "publishQuestions", "publishRatings", "publishReading", "publishFiles", "publishLink"})
            QVERIFY2(!desk.value(part, "checked").toBool(), qPrintable(part));
        QVERIFY(desk.value("publishName", "visible").toBool());
        QVERIFY(desk.value("publishFile", "text").toString().endsWith("Tides.omabook-space"));
        QVERIFY(!desk.value("publishConfirm", "enabled").toBool());
        desk.shot("publish-dialog");

        // Choose… asks the desktop where to save it.
        QVERIFY(desk.click("publishChooseFile"));
        QCOMPARE(desk.picker.asked.value(0), FilePicker::SaveSpace);
        const QString file = desk.dir.filePath("Shared Tides.omabook-space");
        emit desk.picker.picked(FilePicker::SaveSpace, {QUrl::fromLocalFile(file)});
        QCOMPARE(desk.value("publishFile", "text").toString(), file);
        QVERIFY(desk.set("publishName", "text", "Ada"));
        QVERIFY(desk.set("publishGraph", "checked", true));
        // The books' files, once switched on, are said to go to everyone it reaches.
        QVERIFY(!desk.value("publishFilesReach", "visible").toBool());
        QVERIFY(desk.set("publishFiles", "checked", true));
        QVERIFY(desk.value("publishFilesReach", "visible").toBool());
        QVERIFY(desk.value("publishFilesReach", "text").toString().contains("everyone you give the saved file to"));
        QVERIFY(desk.click("publishConfirm"));
        QTRY_VERIFY(!desk.value("publishDialog", "opened").toBool());
        QTRY_VERIFY(desk.backend->message().contains("Published as Ada"));

        // The file holds the graph switched on, and nothing switched off.
        const auto written = SpaceSnapshotFile::load(file);
        QVERIFY2(written.has_value(), qPrintable(Fixtures::whyNot(written)));
        QCOMPARE(written->owner.name, QString("Ada"));
        QCOMPARE(written->parts, QStringList({"graph", "files"}));
        QCOMPARE(QDir(SpaceFiles::besideOf(file)).entryList(QDir::Files).size(), 1);
        const auto fields = [&written](const QString &field) {
            return std::ranges::count_if(written->operations, [&field](const SpaceOperation &op) { return op.field == field; });
        };
        QVERIFY(fields("rating") == 0 && fields("progress") == 0 && fields("formats") >= 1);
        QVERIFY(std::ranges::any_of(written->operations, [](const SpaceOperation &op) { return op.value.contains("Mara"); }));

        // Again, the name is the one gone by before.
        QVERIFY(desk.run("library.publish"));
        QTRY_VERIFY(desk.value("publishDialog", "counted").toBool());
        QVERIFY(!desk.value("publishName", "visible").toBool());
        QCOMPARE(desk.backend->spaces()->publishing().value("name").toString(), QString("Ada"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void openingAWishedForBookFetchesItsFileWhenTheSpaceCarriesIt() {
        Desk desk;
        // Someone else's collection, its book's file beside its file.
        Library owner(desk.dir.filePath("Owner"));
        QVERIFY(owner.open());
        QVERIFY(Fixtures::epub(desk.dir.filePath("tide.epub"), "The Long Tide"));
        const qint64 tide = owner.add(desk.dir.filePath("tide.epub")).id;
        const auto shelf = owner.addCollection("Tides");
        const auto person = Person::generate();
        QVERIFY(shelf && person && owner.putInCollection(*shelf, tide, true));
        const auto snapshot = PublishedSpace::snapshot(owner, *shelf, {.files = true}, {.name = "Ada Fixture", .person = person->id()});
        const QString file = desk.dir.filePath("Tides.omabook-space");
        QVERIFY(snapshot && SpaceSnapshotFile::save(*snapshot, file));
        QCOMPARE(SpaceFiles::writeBeside(owner, *snapshot, file).value_or(0), 1);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));

        // Subscribing says the files come, and brings none of them.
        desk.backend->spaces()->read(file);
        QTRY_VERIFY(desk.backend->spaces()->subscribing().contains("space"));
        QVERIFY(desk.backend->spaces()->subscribing().value("files").toBool());
        desk.backend->spaces()->subscribe(file, {});
        Library &library = *desk.backend->library();
        QTRY_COMPARE(library.books().size(), 1);
        const Book wish = library.books().first();
        QVERIFY(wish.formats.isEmpty());

        // Opening it fetches it, checked against its hash, and opens it.
        desk.reader->openBook(wish.id);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->bookId(), wish.id);
        QCOMPARE(library.book(wish.id)->formats, QStringList{"epub"});
        QCOMPARE(library.formatFiles(wish.id).first().hash, owner.formatFiles(tide).first().hash);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aDroppedFileSaysWhatMatchesThenSubscribes() {
        Desk desk;
        Library &library = *desk.backend->library();
        const QString file = sharedFile(desk.dir.path());
        QVERIFY(!file.isEmpty());
        const qint64 mine = library.addEntry(meta("The Long Tide", "Mira Okafor"), true).id;
        QVERIFY(mine > 0);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));

        // Dropped with a book: the book is added, the space is read and shown.
        QVERIFY(Fixtures::epub(desk.dir.filePath("other.epub"), "Another Book"));
        const QList<QUrl> books =
            desk.backend->spaces()->takeSpaces({QUrl::fromLocalFile(desk.dir.filePath("other.epub")), QUrl::fromLocalFile(file)});
        QCOMPARE(books, QList<QUrl>({QUrl::fromLocalFile(desk.dir.filePath("other.epub"))}));
        QTRY_VERIFY(desk.value("subscribeDialog", "opened").toBool());
        QTRY_VERIFY(desk.value("subscribeDialog", "ready").toBool());
        QCOMPARE(desk.value("subscribeMatched", "text").toString(), QString("1 of its books is in your library"));
        QCOMPARE(desk.value("subscribeWishlist", "text").toString(),
                 QString("1 becomes a wishlist entry: a book without a file, never downloaded"));
        QVERIFY(desk.value("subscribeOwner", "text").toString().contains("Ada Fixture"));
        QVERIFY(desk.value("subscribeGraph", "text").toString().contains("No graph"));
        QCOMPARE(desk.value("subscribeName", "text").toString(), QString("Tides"));
        // Nothing is written before the yes.
        QVERIFY(SubscribedSpace::list(library).isEmpty());
        QCOMPARE(library.books().size(), 1);
        desk.shot("subscribe-dialog");

        // Under a name of the user's own.
        QVERIFY(desk.set("subscribeName", "text", "From Ada"));
        QVERIFY(desk.click("subscribeConfirm"));
        QTRY_VERIFY(!desk.value("subscribeDialog", "opened").toBool());
        const auto made = library.collectionNamed("From Ada");
        QVERIFY(made.has_value());
        QCOMPARE(desk.backend->collection(), made->id);
        QCOMPARE(SubscribedSpace::list(library).size(), 1);
        QCOMPARE(made->of(library.books()).size(), 2);
        QVERIFY(desk.backend->message().contains("shared by Ada Fixture"));
        // Its tab is marked as someone else's.
        QTRY_VERIFY(desk.find("collectionTab-" + QString::number(made->id)));
        QObject *tab = desk.find("collectionTab-" + QString::number(made->id));
        QVERIFY(tab->findChild<QQuickItem *>("collectionTabShared")->isVisible());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void unsubscribingKeepsTheCollection() {
        Desk desk;
        Library &library = *desk.backend->library();
        const QString file = sharedFile(desk.dir.path());
        QVERIFY(!file.isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        // Picked from the ⋯ menu's Subscribe, as the desktop's picker answers.
        QVERIFY(desk.run("library.subscribe"));
        QTRY_VERIFY(desk.value("subscribeDialog", "opened").toBool());
        QCOMPARE(desk.picker.asked.value(0), FilePicker::OpenSpace);
        emit desk.picker.picked(FilePicker::OpenSpace, {QUrl::fromLocalFile(file)});
        QTRY_VERIFY(desk.value("subscribeDialog", "ready").toBool());
        QVERIFY(desk.click("subscribeConfirm"));
        QTRY_COMPARE(SubscribedSpace::list(library).size(), 1);
        const qint64 tides = library.collectionNamed("Tides")->id;
        QTRY_VERIFY(!desk.backend->collections().isEmpty());

        // The collection's dialog says whose it is, and unsubscribes, asking first.
        QMetaObject::invokeMethod(desk.window, "editCollection", Q_ARG(QVariant, tides));
        QTRY_VERIFY(desk.value("collectionDialog", "opened").toBool());
        QVERIFY(desk.value("collectionShared", "text").toString().contains("Ada Fixture"));
        QVERIFY(desk.click("collectionUnsubscribe"));
        QVERIFY(desk.value("collectionShared", "text").toString().startsWith("Unsubscribe from"));
        QVERIFY(!SubscribedSpace::list(library).isEmpty());
        QVERIFY(desk.click("collectionUnsubscribe"));
        QTRY_VERIFY(SubscribedSpace::list(library).isEmpty());
        // The collection stays the user's, with its books, no longer marked.
        QVERIFY(library.collection(tides).has_value());
        QCOMPARE(library.collection(tides)->of(library.books()).size(), 2);
        QTRY_VERIFY(!desk.find("collectionTab-" + QString::number(tides))->findChild<QQuickItem *>("collectionTabShared")->isVisible());
        QVERIFY(desk.backend->message().contains("The collection Tides stays"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aLinkIsSubscribedToAndFollowed() {
        Desk desk;
        Library &library = *desk.backend->library();
        const QString links = desk.dir.filePath("links");
        QVERIFY(QDir().mkpath(links));
        desk.backend->spaces()->setTransports([links] { return std::make_unique<FolderLink>(links); });
        // Someone else's collection, published by link.
        Library owner(desk.dir.filePath("Owner"));
        QVERIFY(owner.open());
        const qint64 tide = owner.addEntry(meta("The Long Tide", "Mira Okafor"), true).id;
        const auto shelf = owner.addCollection("Tides");
        QVERIFY(shelf && owner.putInCollection(*shelf, tide, true));
        FolderLink sender(links);
        const auto sent =
            CollectionShare::publish(owner, *owner.collection(*shelf), {}, {.as = "Ada", .output = {}, .link = &sender, .open = false});
        QVERIFY2(sent.has_value(), qPrintable(Fixtures::whyNot(sent)));
        const QString address = sent->value("sent").toObject().value("address").toString();
        QVERIFY(address.startsWith("https://link.test/"));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));

        // Pasted into the subscribe dialog, read, and subscribed to.
        QVERIFY(desk.run("library.subscribe"));
        QTRY_VERIFY(desk.value("subscribeDialog", "opened").toBool());
        QVERIFY(desk.set("subscribeLinkField", "text", address));
        QVERIFY(desk.click("subscribeReadLink"));
        QTRY_VERIFY(desk.value("subscribeDialog", "ready").toBool());
        QVERIFY(desk.value("subscribeFollows", "visible").toBool());
        QVERIFY(desk.click("subscribeConfirm"));
        QTRY_COMPARE(SubscribedSpace::list(library).size(), 1);
        const qint64 tides = library.collectionNamed("Tides")->id;

        // The owner adds a book; the collection's dialog finds it, and takes it in only when asked.
        const qint64 hers = owner.addEntry(meta("Only Hers", "Ann Other"), true).id;
        QVERIFY(owner.putInCollection(*shelf, hers, true));
        QVERIFY(CollectionShare::publish(owner, *owner.collection(*shelf), {}, {.as = {}, .output = {}, .link = &sender, .open = false}));
        QTRY_VERIFY(desk.backend->collections().size() == 1);
        QMetaObject::invokeMethod(desk.window, "editCollection", Q_ARG(QVariant, tides));
        QTRY_VERIFY(desk.value("collectionDialog", "opened").toBool());
        QTRY_VERIFY(desk.value("collectionLookForUpdates", "visible").toBool());
        QVERIFY(desk.click("collectionLookForUpdates"));
        QTRY_VERIFY(desk.value("collectionUpdates", "text").toString().contains("new there"));
        QCOMPARE(library.collection(tides)->of(library.books()).size(), 1);
        QVERIFY(desk.click("collectionTakeUpdates"));
        QTRY_COMPARE(library.collection(tides)->of(library.books()).size(), 2);
        QTRY_VERIFY(desk.backend->message().contains("up to date"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theKeysAreInTheHelpAndTheMenus() {
        Desk desk;
        QVERIFY(desk.backend->library()->addCollection("Tides").has_value());
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QMetaObject::invokeMethod(desk.window, "showHelp");
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        QHash<QString, QString> keys;
        for (const QVariant &row : desk.value("help", "rows").toList())
            keys.insert(row.toMap().value("id").toString(), row.toMap().value("keys").toString());
        QCOMPARE(keys.value("library.publish"), QString("Shift+P"));
        QCOMPARE(keys.value("library.subscribe"), QString("Shift+O"));
        // The ⋯ of the shelf has both, and so does the list of collections.
        QMetaObject::invokeMethod(desk.window, "showMenu", Q_ARG(QVariant, "library"), Q_ARG(QVariant, QVariant()),
                                  Q_ARG(QVariant, QVariant()));
        QStringList listed;
        for (const QVariant &entry : desk.find("commandMenu")->property("entries").toList())
            listed << entry.toMap().value("id").toString();
        QVERIFY(listed.contains("library.publish") && listed.contains("library.subscribe"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(SharingWindowTests)
#include "sharingwindow_tests.moc"
