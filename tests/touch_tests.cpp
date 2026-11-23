// A book under a finger (#9): a long press on a book lists what can be done
// with it, as a right click does, beside where it was pressed and in every
// view of the shelf; its details are one of what is listed, so a finger gets
// to them in a window too narrow to show them beside the shelf. A tap still
// opens the book, and a mouse is as it was: a click selects, a right click
// lists, and holding the mouse button down does nothing more.

#include <QPointingDevice>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>

#include "assistant.h"
#include "backend.h"
#include "customization.h"
#include "devicedesk.h"
#include "fixtures.h"
#include "hubdesk.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "reader.h"
#include "secretstore.h"
#include "settings.h"
#include "snapshotdesk.h"
#include "theme.h"
#include "translator.h"

namespace {

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// The whole window on a temp folder, as in ui_tests.cpp, on a touch screen
// and with no AI.
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
    QPointingDevice *finger = QTest::createTouchDevice();

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
        engine->rootContext()->setContextProperty("touchscreen", true);
        interface.emplace(&*engine, &*custom);
        if (!interface->load())
            return false;
        window = interface->window();
        if (!window || !QTest::qWaitForWindowExposed(window))
            return false;
        window->requestActivate();
        return QTest::qWaitForWindowActive(window);
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
    bool shows(const QString &name) const {
        const auto *found = qobject_cast<QQuickItem *>(find(name));
        return found && found->isVisible() && found->width() > 0 && found->height() > 0;
    }
    bool resize(const QSize &size) {
        window->resize(size);
        if (!QTest::qWaitFor([&] { return window->size() == size; }, 2000))
            return false;
        settle();
        return true;
    }
    void settle() {
        for (int frame = 0; frame < 2; ++frame) {
            QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
            window->update();
            drawn.wait(150);
        }
    }
    [[nodiscard]] qint64 selected() const {
        QVariant id;
        QMetaObject::invokeMethod(window, "selectedBookId", Q_RETURN_ARG(QVariant, id));
        return id.toLongLong();
    }
    // The shelf's view, on its first book.
    void showFirst() const {
        QObject *shelf = value("shelfLoader", "item").value<QObject *>();
        QVERIFY(shelf);
        QMetaObject::invokeMethod(shelf, "show", Q_ARG(QVariant, 0));
    }
    [[nodiscard]] QObject *menu() const { return find("commandMenu"); }
    [[nodiscard]] bool menuOpen() const { return menu() && menu()->property("opened").toBool(); }
    bool closeMenu() const {
        QMetaObject::invokeMethod(menu(), "close");
        return QTest::qWaitFor([&] { return !menu()->property("visible").toBool(); }, 2000);
    }

    void tap(const QPoint &at) const {
        QTest::touchEvent(window, finger).press(0, at, window);
        QTest::touchEvent(window, finger).release(0, at, window);
    }
    // A finger held still for longer than a long press, then lifted.
    void hold(const QPoint &at) const {
        QTest::touchEvent(window, finger).press(0, at, window);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 300);
        QTest::touchEvent(window, finger).release(0, at, window);
    }
    // With OMABOOK_SHOTS set to a folder, a picture of the window there.
    void shot(const QString &name) const {
        if (const QString folder = qEnvironmentVariable("OMABOOK_SHOTS"); !folder.isEmpty()) {
            QTest::qWait(350); // let the popups finish their transitions
            window->grabWindow().save(folder + "/" + name + ".png");
        }
    }
};

// Books enough to fill a shelf, each a file to open, some in a series so
// the stacks have a pile.
void stock(Desk &desk) {
    Library &library = *desk.backend->library();
    for (int i = 1; i <= 8; ++i) {
        BookMeta meta;
        meta.title = QString("The Harbour Book %1").arg(i);
        meta.authors = {i % 2 ? "Mira Okafor" : "Edda Marlowe"};
        if (i <= 3) {
            meta.series = "Salt Roads";
            meta.seriesIndex = i;
        }
        const QString file = desk.dir.filePath(QString("harbour-%1.txt").arg(i));
        QVERIFY(Fixtures::write(file, "Night " + QByteArray::number(i) + " at the harbour, and the lantern over it.\n"));
        QVERIFY(library.add(file, meta).id > 0);
    }
    desk.backend->reload();
    QCOMPARE(desk.backend->books()->count(), 8);
}

// The middle of the last book wholly on screen, the one lowest down and then
// furthest right: away from the first, which is selected.
std::optional<QPoint> lastBook(const Desk &desk) {
    std::optional<QPoint> found;
    const auto later = [&](const QPoint &at) { return !found || at.y() > found->y() || (at.y() == found->y() && at.x() > found->x()); };
    const QRectF screen(0, 0, desk.window->width(), desk.window->height());
    QList<QQuickItem *> items{desk.window->contentItem()};
    while (!items.isEmpty()) {
        QQuickItem *item = items.takeFirst();
        if (!item->isVisible() || item->opacity() <= 0.01)
            continue;
        const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        if (QString::fromLatin1(item->metaObject()->className()).startsWith("BookTaps") && rect.width() >= 8 && screen.contains(rect)
            && later(rect.center().toPoint()))
            found = rect.center().toPoint();
        items << item->childItems();
    }
    return found;
}

// What a press on a book left behind: which book is selected, and the list
// of what can be done with it, of which kind, and where.
struct Answer {
    qint64 selected = 0;
    bool listed = false;
    QString kind;
    QPointF near;
    bool operator==(const Answer &) const = default;
};
Answer answer(const Desk &desk) {
    QTest::qWait(150);
    const bool listed = desk.menuOpen() || QTest::qWaitFor([&] { return desk.menuOpen(); }, 1000);
    return {.selected = desk.selected(),
            .listed = listed,
            .kind = listed ? desk.menu()->property("kind").toString() : QString(),
            .near = listed ? desk.menu()->property("near").toPointF() : QPointF()};
}

} // namespace

class TouchTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // In every view, holding a book does what a right click on it does: it
    // is selected and what can be done with it is listed, beside the finger.
    // Nothing is opened.
    void aLongPressOnABookListsWhatCanBeDoneWithIt() {
        Desk desk;
        stock(desk);
        if (QTest::currentTestFailed())
            return;
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        for (const QString view : {"covers", "spines", "carousel", "list", "stacks"}) {
            desk.backend->setView(view);
            QTRY_COMPARE(desk.value("shelfLoader", "status").toInt(), 1);
            const auto pressed = [&](const std::function<void(const QPoint &)> &press) {
                desk.showFirst();
                QTest::qWait(400); // the carousel turns to it
                desk.settle();
                const std::optional<QPoint> at = lastBook(desk);
                if (!at)
                    return Answer{};
                press(*at);
                return answer(desk);
            };
            const qint64 first = desk.backend->books()->idAt(0);
            const Answer held = pressed([&](const QPoint &at) { desk.hold(at); });
            QVERIFY2(held.listed, qPrintable(view + ": a long press on a book listed nothing"));
            QCOMPARE(held.kind, QString("book"));
            QVERIFY2(!desk.reader->isOpen(), qPrintable(view + ": a long press opened the book"));
            if (view != "stacks")
                QVERIFY2(held.selected != first, qPrintable(view + ": a long press didn't select the book held"));
            desk.shot("touch-held-" + view);
            QVERIFY(desk.closeMenu());
            const Answer clicked = pressed([&](const QPoint &at) { QTest::mouseClick(desk.window, Qt::RightButton, Qt::NoModifier, at); });
            QVERIFY(desk.closeMenu());
            QVERIFY2(held == clicked, qPrintable(view + ": a long press and a right click did different things"));
        }
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Where the window has no room for the details beside the shelf, a
    // finger gets them from that list.
    void theDetailsAreWhatALongPressListsInANarrowWindow() {
        Desk desk;
        stock(desk);
        if (QTest::currentTestFailed())
            return;
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(560, 780)));
        QVERIFY(!desk.value("library", "detailsFit").toBool());
        QVERIFY(!desk.shows("details"));
        const std::optional<QPoint> at = lastBook(desk);
        QVERIFY(at);
        desk.hold(*at);
        QTRY_VERIFY(desk.menuOpen());
        QVERIFY(!desk.shows("details"));
        QTRY_VERIFY(desk.shows("menuEntry-library.details"));
        auto *entry = qobject_cast<QQuickItem *>(desk.find("menuEntry-library.details"));
        QVERIFY(entry);
        desk.tap(entry->mapToScene(QPointF(entry->width() / 2, entry->height() / 2)).toPoint());
        QTRY_VERIFY(!desk.menu()->property("visible").toBool());
        QTRY_VERIFY(desk.shows("details"));
        QCOMPARE(desk.value("details", "book").toMap().value("bookId").toLongLong(), desk.selected());
        desk.shot("touch-details-narrow");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A tap still opens the book; with a mouse, holding the button down on
    // a book lists nothing and opens nothing, and a click selects it.
    void aTapOpensAndAMouseIsAsItWas() {
        Desk desk;
        stock(desk);
        if (QTest::currentTestFailed())
            return;
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        const std::optional<QPoint> at = lastBook(desk);
        QVERIFY(at);
        const qint64 first = desk.selected();
        QTest::mousePress(desk.window, Qt::LeftButton, Qt::NoModifier, *at);
        QTest::qWait(QGuiApplication::styleHints()->mousePressAndHoldInterval() + 300);
        QTest::mouseRelease(desk.window, Qt::LeftButton, Qt::NoModifier, *at);
        QTest::qWait(300);
        QVERIFY(!desk.menuOpen());
        QVERIFY(!desk.reader->isOpen());
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, *at);
        QTRY_VERIFY(desk.selected() != first);
        QVERIFY(!desk.menuOpen());
        QVERIFY(!desk.reader->isOpen());
        desk.tap(*at);
        QTRY_VERIFY(desk.reader->isOpen());
        QVERIFY(!desk.menuOpen());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(TouchTests)
#include "touch_tests.moc"
