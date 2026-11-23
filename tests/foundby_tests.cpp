// Why a search found a book, on its tile (#10): a book the search found by
// what is known of it says which thing found it on the shelf itself, in
// every view that shows the book's face or its line, by the name the reader
// knows it by and never from past the reader's place; a thing of a graph
// someone shared is marked as theirs. It reads in a long language and right
// to left. The tiles are looked at at every size, in German and under
// fingers, in layout_tests (searchAndTheAiAtEverySize).

#include <QFile>
#include <QFileInfo>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "assistant.h"
#include "backend.h"
#include "booklistmodel.h"
#include "customization.h"
#include "devicedesk.h"
#include "fixtures.h"
#include "graph.h"
#include "hubdesk.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "person.h"
#include "publishedspace.h"
#include "reader.h"
#include "secretstore.h"
#include "settings.h"
#include "snapshotdesk.h"
#include "subscriptions.h"
#include "theme.h"
#include "translator.h"

namespace {

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// The whole window on a temp folder, as in ui_tests.cpp, in a language and
// with no AI.
struct Desk {
    QTemporaryDir dir;
    NoPicker picker;
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
    SessionSecrets deviceSecrets;
    std::optional<DeviceDesk> devices;
    std::optional<SnapshotDesk> snapshotDesk;
    std::optional<HubDesk> hubsDesk;
    std::optional<QQmlEngine> engine;
    std::optional<Interface> interface;
    QStringList warnings;
    QQuickWindow *window = nullptr;

    explicit Desk(const QByteArray &language = "en") {
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        Fixtures::write(dir.filePath("language"), language + "\n");
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
        devices.emplace(&*settings, deviceSecrets, [this] { return backend->libraryPath(); });
        // Snapshots of its own, never this computer's.
        snapshotDesk.emplace(SnapshotPlace::in(dir.filePath("snapshots")),
                             SnapshotContents{.settingsFile = dir.filePath("config.json"), .pluginsFolder = dir.filePath("plugins")},
                             [this] { return backend->libraryPath(); });
        // Hubs of its own, never this computer's.
        hubsDesk.emplace(HubDesk::linkedHere([this] { return backend->libraryPath(); }, dir.filePath("hubs")));
    }
    ~Desk() {
        interface.reset();
        engine.reset();
        QCoreApplication::removeTranslator(&*translator);
    }
    Desk(const Desk &) = delete;
    Desk &operator=(const Desk &) = delete;
    Desk(Desk &&) = delete;
    Desk &operator=(Desk &&) = delete;

    bool show() {
        engine.emplace();
        QObject::connect(&*engine, &QQmlEngine::warnings, &*engine, [this](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                warnings << error.toString();
        });
        engine->addImageProvider("book", reader->imageProvider());
        const QList<std::pair<const char *, QObject *>> objects{
            {"translator", &*translator}, {"theme", &*look},        {"custom", &*custom},   {"librarian", &*librarian},
            {"manual", &*manual},         {"settings", &*settings}, {"backend", &*backend}, {"reader", &*reader},
            {"assistant", &*assistant},   {"aiAgent", &*aiAgent},   {"devices", &*devices}, {"snapshotDesk", &*snapshotDesk},
            {"hubsDesk", &*hubsDesk}};
        for (const auto &[name, object] : objects)
            engine->rootContext()->setContextProperty(name, object);
        engine->rootContext()->setContextProperty("touchscreen", false);
        interface.emplace(&*engine, &*custom);
        if (!interface->load())
            return false;
        window = interface->window();
        if (!window || !QTest::qWaitForWindowExposed(window))
            return false;
        window->resize(1180, 780);
        return QTest::qWaitFor([&] { return window->width() == 1180; }, 2000);
    }
    void settle() {
        for (int frame = 0; frame < 3; ++frame) {
            QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
            window->update();
            drawn.wait(150);
        }
    }
    // The shelf in a view, once it is shown.
    bool view(const QString &name) {
        backend->setView(name);
        const bool loaded = QTest::qWaitFor(
            [&] {
                QObject *loader = find("shelfLoader");
                return loader && loader->property("status").toInt() == 1
                       && loader->property("source").toString().contains(name, Qt::CaseInsensitive);
            },
            3000);
        QTest::qWait(300); // the carousel turns to the book
        settle();
        return loaded;
    }
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
    // Every item of a name that is on screen.
    [[nodiscard]] QList<QQuickItem *> shown(const QString &name) const {
        QList<QQuickItem *> found;
        QList<QQuickItem *> items{window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (!item->isVisible() || item->opacity() <= 0.01)
                continue;
            if (item->objectName() == name && item->width() > 0 && item->height() > 0)
                found << item;
            items << item->childItems();
        }
        return found;
    }
    // What the tiles on screen say found their books.
    [[nodiscard]] QStringList said() const {
        QStringList texts;
        for (QQuickItem *text : shown("foundByText"))
            texts << text->property("text").toString();
        return texts;
    }
    void shot(const QString &name) const {
        if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty())
            window->grabWindow().save(folder + "/" + name + ".png");
    }
};

[[nodiscard]] QString textBook(const QString &folder, const QString &name, const QString &line) {
    const QString file = folder + "/" + name + ".txt";
    return Fixtures::write(file, line.toUtf8() + "\n") ? file : QString();
}

// A shelf of three books: one read, of which a lighthouse keeper is known;
// one whose title has the word the search is for; one besides.
struct Shelf {
    qint64 keeper = 0, lantern = 0, other = 0;
};
Shelf stock(Desk &desk) {
    Library &library = *desk.backend->library();
    const auto add = [&](const QString &title, const QString &author) {
        BookMeta meta;
        meta.title = title;
        meta.authors = {author};
        return library.add(textBook(desk.dir.path(), title, "The book called " + title + "."), meta).id;
    };
    Shelf shelf{.keeper = add("The Long Mole", "Mira Okafor"),
                .lantern = add("Lighthouse Hours", "Edda Marlowe"),
                .other = add("Granite", "Soren Hale")};
    library.setReading(shelf.keeper, "read", 4, {});
    Graph graph(library);
    graph.upsert({}, "character", "Wilhelmina Lightfoot", "Keeps the lamp on the long mole.", {}, GraphPlace{shelf.keeper, 0, 0.1},
                 "Wilhelmina");
    desk.backend->reload();
    return shelf;
}

// Of ten chapters, as a book someone shared is.
[[nodiscard]] bool tenChapters(const QString &path) {
    QList<Fixtures::Chapter> chapters;
    for (int at = 1; at <= 10; ++at)
        chapters << Fixtures::Chapter{QString("Chapter %1").arg(at), QString("<p>The ferry crossed again, the %1th time.</p>").arg(at)};
    return Fixtures::chapteredEpub(path, "The Long Tide", chapters);
}

// Ada read The Long Tide to its end and shares it, graph and all: Glass
// Harbour early on, and Wick near the end. The reader has the same book and
// is in its third chapter, with a Mara of their own in it.
[[nodiscard]] qint64 subscribed(Desk &desk, const QString &folder) {
    Library owner(folder + "/Owner");
    const QString file = folder + "/The Long Tide.epub";
    if (!owner.open() || !tenChapters(file))
        return 0;
    const qint64 theirs = owner.add(file).id;
    owner.setProgress(theirs, {9, 1.0, 100});
    const auto shelf = owner.addCollection("Tides");
    if (!shelf || !owner.putInCollection(*shelf, theirs, true))
        return 0;
    Graph graph(owner);
    graph.upsert({}, "place", "Glass Harbour", "Where the ferries berth.", {}, GraphPlace{theirs, 1, 0.5});
    graph.upsert({}, "character", "Wick", "A lamplighter turned informer.", {}, GraphPlace{theirs, 7, 0.5});
    const auto person = Person::generate();
    if (!person)
        return 0;
    const auto snapshot = PublishedSpace::snapshot(owner, *shelf, {.graph = true}, {.name = "Ada Fixture", .person = person->id()});
    if (!snapshot)
        return 0;

    Library &library = *desk.backend->library();
    const QString copy = desk.dir.filePath("tide.epub");
    if (!QFile::copy(file, copy))
        return 0;
    const qint64 mine = library.add(copy).id;
    library.setProgress(mine, {2, 0.5, 30});
    Graph(library).upsert({}, "character", "Mara", "She rows at night.", {}, GraphPlace{mine, 0, 0.2});
    if (!Subscriptions::subscribe(library, *snapshot, {}))
        return 0;
    desk.backend->reload();
    return mine;
}

} // namespace

class FoundByTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // "light" finds the book whose title says it by its details, and the
    // book Wilhelmina Lightfoot is known in by her: only that one's tile
    // says why, in each view, and the tiles say nothing once the search is
    // cleared.
    void aTileSaysWhatFoundItsBook() {
        Desk desk;
        const Shelf shelf = stock(desk);
        QVERIFY(shelf.keeper > 0 && shelf.lantern > 0 && shelf.other > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        BookListModel *books = desk.backend->books();
        books->setQuery("wilhelmina");
        QTRY_COMPARE(books->count(), 1);
        books->setQuery("light");
        QTRY_COMPARE(books->count(), 2);
        QTRY_VERIFY(!books->knownFor(shelf.keeper).isEmpty());
        QVERIFY(books->knownFor(shelf.lantern).isEmpty());
        const QStringList wanted{"Wilhelmina Lightfoot (Character)"};
        for (const QString view : {"covers", "carousel", "list", "spines"}) {
            QVERIFY2(desk.view(view), qPrintable(view));
            // The spines show a book's face only pulled out.
            if (view == "spines")
                QMetaObject::invokeMethod(desk.window, "selectOnShelf", Q_ARG(QVariant, shelf.keeper));
            QTRY_COMPARE(desk.said(), wanted);
            QVERIFY(desk.shown("foundByShared").isEmpty());
            desk.shot("found-" + view);
        }
        books->setQuery({});
        QTRY_COMPARE(books->count(), 3);
        QTRY_VERIFY(desk.said().isEmpty());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A thing of a graph someone shared is marked as theirs, and what the
    // reader hasn't reached in it finds nothing and says nothing.
    void aTileMarksWhatCameFromASharedGraph() {
        Desk desk;
        QTemporaryDir folder;
        const qint64 tide = subscribed(desk, folder.path());
        QVERIFY(tide > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        BookListModel *books = desk.backend->books();
        books->setQuery("harbour");
        QTRY_COMPARE(books->count(), 1);
        QTRY_COMPARE(desk.said(), QStringList{"Glass Harbour (Place)"});
        const QList<QQuickItem *> marks = desk.shown("foundByShared");
        QCOMPARE(marks.size(), 1);
        // Whose it is is said to whoever can't see the mark.
        QQuickItem *why = marks.first()->parentItem();
        QCOMPARE(QQmlProperty(why, "Accessible.name", qmlContext(why)).read().toString(),
                 QString("Found by what you know from Ada Fixture's shared graph: Glass Harbour (Place)"));
        desk.shot("found-shared");
        // Their Wick is met in chapter eight; the reader is in the third.
        books->setQuery("wick");
        QTest::qWait(400);
        QCOMPARE(books->count(), 0);
        QVERIFY(desk.said().isEmpty());
        // The reader's own Mara is their own: found, and not marked.
        books->setQuery("mara");
        QTRY_COMPARE(books->count(), 1);
        QTRY_COMPARE(desk.said(), QStringList{"Mara (Character)"});
        QVERIFY(desk.shown("foundByShared").isEmpty());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // In German the kind is German, and right to left the mark and the
    // words are mirrored and stay on the tile.
    void inGermanAndRightToLeft() {
        for (const QByteArray &language : {QByteArray("de"), QByteArray("ar")}) {
            Desk desk(language);
            QTemporaryDir folder;
            QVERIFY(subscribed(desk, folder.path()) > 0);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.backend->books()->setQuery("harbour");
            QTRY_COMPARE(desk.backend->books()->count(), 1);
            for (const QString view : {"covers", "list"}) {
                QVERIFY2(desk.view(view), qPrintable(view));
                QTRY_COMPARE(desk.said().size(), 1);
                if (language == "de")
                    QCOMPARE(desk.said().first(), QString("Glass Harbour (Ort)"));
                QQuickItem *text = desk.shown("foundByText").first();
                QQuickItem *mark = desk.shown("foundByShared").value(0);
                QVERIFY(mark);
                const QRectF said = text->mapRectToScene(QRectF(0, 0, text->width(), text->height()));
                const QRectF marked = mark->mapRectToScene(QRectF(0, 0, mark->width(), mark->height()));
                // The mark goes first, in the way the language reads.
                if (language == "ar")
                    QVERIFY2(marked.left() >= said.right() - 1, qPrintable(view));
                else
                    QVERIFY2(marked.right() <= said.left() + 1, qPrintable(view));
                // Both lie in what holds them: the band across the cover, or the line.
                QQuickItem *holder = text->parentItem()->parentItem();
                const QRectF room = holder->mapRectToScene(QRectF(0, 0, holder->width(), holder->height())).adjusted(-1, -1, 1, 1);
                QVERIFY2(room.contains(said) && room.contains(marked), qPrintable(view));
                QVERIFY2(text->property("implicitWidth").toReal() <= text->width() + 1 || text->property("truncated").toBool(),
                         qPrintable(view + ": cut without an ellipsis"));
                desk.shot("found-" + view + "-" + language);
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }
};

QTEST_MAIN(FoundByTests)
#include "foundby_tests.moc"
