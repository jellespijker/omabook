#pragma once
// The whole app on a made-up library, at a size, and the survey of what is
// on screen there: what the tests of the window's layout share
// (layout_tests.cpp, chronologyview_tests.cpp). Every Desk is a temp folder
// of its own: its own language, theme, settings and library, never the
// user's.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <optional>

#include "aisearch.h"
#include "assistant.h"
#include "backend.h"
#include "chronology.h"
#include "customization.h"
#include "devicedesk.h"
#include "eventtime.h"
#include "secretstore.h"
#include "fixtures.h"
#include "graph.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "reader.h"
#include "settings.h"
#include "theme.h"
#include "themechoice.h"
#include "translator.h"

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// A model that says one long thing, so the panes that show an answer have
// one to lay out.
class LongAi : public AiProvider {
public:
    QString name() const override { return "Testbot"; }
    void ask(const AiRequest &) override {
        QTimer::singleShot(0, this, [this] {
            emit delta("The harbour is where the first book opens, and the lantern keeper who meets Mira there is the same "
                       "one the second book calls the Tidewarden. ");
            emit delta("Nothing past the page you are on is given away here.");
            emit finished();
        });
    }
    void cancel() override {}
};

class FakeCatalog : public MetadataSource {
public:
    using MetadataSource::MetadataSource;
    QString name() const override { return "Fake Catalog"; }
    QStringList fields() const override { return {"title", "authors"}; }
    // Three answers, one of them as long as a catalog's can be.
    void search(const BookMeta &) override {
        QTimer::singleShot(0, this, [this] {
            QList<MetaCandidate> candidates;
            for (const char *title :
                 {"Moss", "The Unabridged and Thoroughly Annotated Chronicle of the Lantern Keepers of the Northern Salt Roads",
                  "Moss: A Second Edition, Revised"}) {
                MetaCandidate candidate;
                candidate.source = name();
                candidate.meta.title = title;
                candidate.meta.authors = QStringList{"Maximiliana Oluwaseun Featherstonehaugh-Abernathy", "Edda Marlowe"};
                candidate.meta.publisher = "The Press of the Made-Up Harbour and Sons";
                candidate.meta.published = "1987";
                candidate.meta.description = "A catalog's blurb, which is long, and says the same thing three times over in the hope "
                                             "that one of them lands.";
                candidates << candidate;
            }
            emit found(candidates);
        });
    }
};

// A picture decoded off the GUI thread (an asynchronous Image) that has
// not come in yet.
inline bool stillLoading(const QQuickItem *item) {
    return item->inherits("QQuickImageBase") && item->property("status").toInt() == 2; // Image.Loading
}

// The whole app on a temp folder, as in ui_tests.cpp: its own language,
// theme, settings and library, never the user's.
struct Desk {
    QTemporaryDir dir;
    NoPicker picker;
    std::optional<Translator> translator;
    std::optional<Theme> theme;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;
    std::optional<Assistant> assistant;
    std::optional<AiAgent> aiAgent;
    std::optional<Customization> custom;
    std::optional<ThemeChoice> themeChoice;
    std::optional<Look> look;
    std::optional<Librarian> librarian;
    std::optional<Manual> manual;
    SessionSecrets deviceSecrets;
    std::optional<DeviceDesk> devices;
    std::optional<QQmlEngine> engine;
    std::optional<Interface> interface;
    QStringList warnings;
    QQuickWindow *window = nullptr;
    QByteArray language;
    bool touch = false;

    Desk(const QByteArray &language, const QByteArray &colors, bool touch) : language(language), touch(touch) {
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        Fixtures::write(dir.filePath("language"), language + "\n");
        if (!colors.isEmpty()) {
            QDir(dir.path()).mkpath("desktop/theme");
            Fixtures::write(dir.filePath("desktop/theme/colors.toml"), colors);
        }
        translator.emplace(dir.filePath("language"));
        QCoreApplication::installTranslator(&*translator);
        theme.emplace(dir.filePath("desktop"));
        custom.emplace(dir.filePath("custom"));
        // The desktop's theme is Omarchy's here, so the picker has it too.
        themeChoice.emplace(&*custom, ThemePlaces{dir.filePath("desktop"), dir.filePath("omarchy/themes"), dir.filePath("aether"),
                                                  dir.filePath("custom/themes")});
        look.emplace(&*theme, &*themeChoice, &*custom);
        settings.emplace(dir.filePath("config.json"));
        settings->setLibraryPath(dir.filePath("Library"));
        auto makeAi = [](QString *) -> std::unique_ptr<AiProvider> { return std::make_unique<LongAi>(); };
        backend.emplace(
            &*settings, &picker, nullptr, makeAi, [](QObject *parent) { return QList<MetadataSource *>{new FakeCatalog(parent)}; },
            dir.filePath("plugins"), "-");
        reader.emplace(&*backend, QString());
        assistant.emplace(&*reader, makeAi);
        aiAgent.emplace(&*settings, dir.filePath("omarchy/defaults/agent"));
        librarian.emplace(&*backend, &*settings, &*custom, [this] { return look->colors(); });
        settings->setTour("done");
        // Finding plugins asks only the registry built in: no test reaches GitHub.
        settings->setPluginDiscovery(false);
        settings->setNewsSeen(OMABOOK_VERSION);
        manual.emplace(&*settings, &*custom);
        devices.emplace(&*settings, deviceSecrets, [this] { return backend->libraryPath(); });
        const auto followPlugins = [this] { custom->setPlugins(backend->scripts()->commands(), backend->scripts()->panels()); };
        QObject::connect(backend->scripts(), &ScriptPlugins::changed, &*custom, followPlugins);
        followPlugins();
    }

    ~Desk() {
        interface.reset();
        engine.reset();
        QCoreApplication::removeTranslator(&*translator);
    }

    bool show() {
        engine.emplace();
        QObject::connect(&*engine, &QQmlEngine::warnings, &*engine, [this](const QList<QQmlError> &errors) {
            for (const auto &error : errors)
                warnings << error.toString();
        });
        engine->addImageProvider("book", reader->imageProvider());
        engine->rootContext()->setContextProperty("translator", &*translator);
        engine->rootContext()->setContextProperty("theme", &*look);
        engine->rootContext()->setContextProperty("custom", &*custom);
        engine->rootContext()->setContextProperty("librarian", &*librarian);
        engine->rootContext()->setContextProperty("manual", &*manual);
        engine->rootContext()->setContextProperty("devices", &*devices);
        engine->rootContext()->setContextProperty("settings", &*settings);
        engine->rootContext()->setContextProperty("backend", &*backend);
        engine->rootContext()->setContextProperty("reader", &*reader);
        engine->rootContext()->setContextProperty("assistant", &*assistant);
        engine->rootContext()->setContextProperty("aiAgent", &*aiAgent);
        engine->rootContext()->setContextProperty("touchscreen", touch);
        interface.emplace(&*engine, &*custom);
        if (!interface->load())
            return false;
        window = interface->window();
        if (!window || !QTest::qWaitForWindowExposed(window))
            return false;
        window->requestActivate();
        return QTest::qWaitForWindowActive(window);
    }

    // An object by its objectName, wherever it is: loaders and views don't
    // keep their items in the QObject tree.
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
    QQuickItem *item(const QString &name) const { return qobject_cast<QQuickItem *>(find(name)); }
    // An item that is really on screen: itself and everything above it
    // visible, and with a size.
    bool shows(const QString &name) const {
        QQuickItem *found = item(name);
        return found && found->isVisible() && found->width() > 0 && found->height() > 0;
    }

    // The window's own commands, by the names the keyboard knows them by.
    bool run(const QString &command) {
        QVariant ran;
        QMetaObject::invokeMethod(window, "run", Q_RETURN_ARG(QVariant, ran), Q_ARG(QVariant, command));
        return ran.toBool();
    }
    void call(const char *method) { QMetaObject::invokeMethod(window, method); }
    void call(const char *method, const QVariant &argument) { QMetaObject::invokeMethod(window, method, Q_ARG(QVariant, argument)); }
    // The list of what can be done here (CommandMenu.qml), of a kind.
    void menu(const QString &kind) {
        QMetaObject::invokeMethod(window, "showMenu", Q_ARG(QVariant, kind), Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant()));
    }

    // The window at a size, laid out and drawn.
    bool resize(const QSize &size) {
        window->resize(size);
        if (!QTest::qWaitFor([&] { return window->size() == size; }, 2000))
            return false;
        settle();
        return true;
    }
    // Bindings, layouts and the zero-interval timers that follow a resize
    // have all run once a couple of frames have been drawn.
    void settle(int frames = 2) {
        for (int i = 0; i < frames; ++i) {
            QSignalSpy drawn(window, &QQuickWindow::frameSwapped);
            window->update();
            drawn.wait(150);
        }
    }

    // Every picture on screen that is decoded off the GUI thread (an
    // asynchronous Image) has come in or failed: until then a cover's tile
    // shows its title instead, and how the window looks depends on how
    // busy the machine is. False when one is still loading after `timeout`.
    bool pictured(int timeout = 5000) {
        settle(1);
        const auto loading = [this] {
            QList<QQuickItem *> items{window->contentItem()};
            while (!items.isEmpty()) {
                QQuickItem *item = items.takeFirst();
                if (!item->isVisible())
                    continue;
                if (stillLoading(item))
                    return true;
                items << item->childItems();
            }
            return false;
        };
        if (!QTest::qWaitFor([&] { return !loading(); }, timeout))
            return false;
        settle(1);
        return true;
    }

    // The layer popups are shown in, and the popups that are up.
    QQuickItem *overlay() const {
        for (QQuickItem *child : window->contentItem()->childItems())
            if (child->inherits("QQuickOverlay"))
                return child;
        return nullptr;
    }
    QList<QObject *> popups() const {
        QList<QObject *> found;
        if (QQuickItem *layer = overlay())
            for (QQuickItem *child : layer->childItems())
                if (child->inherits("QQuickPopupItem") && child->isVisible() && child->parent()
                    && !child->parent()->inherits("QQuickToolTip"))
                    found << child->parent();
        return found;
    }
    // Dialogs fade and grow in and out, a fifth of a second each way. That
    // is most of what looking at every dialog at every size would take, and
    // none of what is being looked at, so here they open and close at once.
    void quicken() const {
        for (QObject *popup : window->findChildren<QObject *>()) {
            if (!popup->inherits("QQuickPopup"))
                continue;
            QQmlProperty(popup, "enter").write(QVariant::fromValue<QObject *>(nullptr));
            QQmlProperty(popup, "exit").write(QVariant::fromValue<QObject *>(nullptr));
        }
    }
    // A popup grows into place, and is measured only once it has stopped.
    bool waitForPopup() {
        const bool up = QTest::qWaitFor(
            [&] {
                const QList<QObject *> open = popups();
                return !open.isEmpty()
                       && std::all_of(open.begin(), open.end(), [](QObject *popup) { return popup->property("opened").toBool(); });
            },
            3000);
        settle();
        return up;
    }
    bool closePopups() {
        for (int round = 0; round < 4 && !popups().isEmpty(); ++round) {
            for (QObject *popup : popups())
                QMetaObject::invokeMethod(popup, "close");
            if (!QTest::qWaitFor([&] { return popups().isEmpty(); }, 1500))
                continue;
        }
        settle(1);
        return popups().isEmpty();
    }

    void shot(const QString &name) const {
        const QString folder = qEnvironmentVariable("OMABOOK_SHOTS");
        if (folder.isEmpty())
            return;
        window->grabWindow().save(folder + "/" + name + ".png");
    }
};

inline QImage madeUpCover(const QString &title, const QColor &color) {
    QImage cover(200, 300, QImage::Format_RGB32);
    cover.fill(color);
    QPainter painter(&cover);
    painter.setPen(color.lightnessF() < 0.55 ? Qt::white : Qt::black);
    QFont font = painter.font();
    font.setPixelSize(20);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(16, 60, 168, 200), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, title);
    return cover;
}

// A made-up library that is hard on a layout: titles of ninety characters
// and more, titles of one word, authors with long names and several of
// them, long series, and every status. Nothing in it is a real book. The
// first book is a file with chapters, to read; the rest are entries.
inline QList<qint64> stock(Desk &desk) {
    struct Made {
        const char *title, *authors, *series;
        double index;
        const char *tag, *color;
    };
    static const QList<Made> books{
        {"The Unabridged and Thoroughly Annotated Chronicle of the Lantern Keepers of the Northern Salt Roads, Volume the First",
         "Maximiliana Oluwaseun Featherstonehaugh-Abernathy", "The Salt Roads and the Harbours Beyond Them", 1, "Fantasy", "#1f4e5f"},
        {"Moss", "Edda Marlowe", "", 0, "Nature", "#3d5a3c"},
        {"A Practical Field Guide to the Imaginary Birds, Moths and Lesser Weathervanes of the Eastern Marshlands and Their Songs",
         "Bartholomew Lindqvist-Okonkwo; Anneke van der Westhuizen de Vries", "", 0, "Nature", "#5b8c5a"},
        {"The Lantern Tide", "Mira Okafor", "The Salt Roads and the Harbours Beyond Them", 2, "Fantasy", "#27667b"},
        {"Donaudampfschifffahrtsgesellschaftskapitänsmützenabzeichen", "Wilhelmina Konstantinopolitanska", "", 0, "History", "#7a3b2e"},
        {"Notes on Slow Rivers", "Edda Marlowe", "", 0, "Essays", "#e8dcc0"},
        {"Thirteen Ways to Fold a Letter So That It Says Something Other Than What Was Written in It, with Diagrams", "Hana Sato", "", 0,
         "Essays", "#d98c5f"},
        {"Granite", "Soren Hale", "", 0, "Mystery", "#6b6f73"},
        {"The Quiet Cartographer", "Idris Vance", "Clockwork Orchard", 1, "Mystery", "#2c2c3a"},
        {"Winter Gears", "Tobias Lindqvist", "Clockwork Orchard", 2, "Fantasy", "#a8742a"},
        {"An Atlas of Forgotten Stations", "Idris Vance", "", 0, "History", "#404e7c"},
        {"Small Hours", "June Abernathy", "", 0, "Essays", "#b23a48"},
        {"The Beekeeper's Algebra", "Lucia Ferrand", "", 0, "Nature", "#e3b505"},
        {"Bread, Salt and Thunder", "Anneke de Vries", "", 0, "History", "#7a3b2e"},
        {"Paper Moons", "June Abernathy", "", 0, "Fantasy", "#51355a"},
        {"The Night Ferry", "Hana Sato", "", 0, "Mystery", "#14213d"},
        {"Seven Kinds of Rain", "Lucia Ferrand", "", 0, "Essays", "#5c80bc"},
        {"Tin Stars", "P. L. Okonkwo", "", 0, "Mystery", "#22333b"},
        // The same book twice and an author spelled two ways, for the tidy.
        {"Granite", "Soren Hale", "", 0, "Mystery", "#6b6f73"},
        {"A Winter Without Lanterns", "Sato, Hana", "", 0, "Mystery", "#14213d"},
    };
    Library *library = desk.backend->library();
    QList<qint64> ids;
    for (int i = 0; i < books.size(); ++i) {
        const Made &made = books.at(i);
        BookMeta meta;
        meta.title = QString::fromUtf8(made.title);
        meta.authors = QString::fromUtf8(made.authors).split("; ");
        meta.series = made.series;
        meta.seriesIndex = made.index;
        meta.tags = QStringList{made.tag, "A rather long tag for the shelf", "Borrowed"};
        meta.language = i % 5 == 4 ? "nl" : "en";
        meta.published = QString::number(1950 + (i * 37) % 70);
        meta.publisher = "The Press of the Made-Up Harbour and Sons";
        meta.description = "A book that exists only so that a window has something long to lay out. It goes on for a while, "
                           "as blurbs do, about a harbour, a lantern and the people who keep both, and says nothing true.";
        qint64 id = 0;
        if (i == 0) {
            QByteArray text;
            for (int chapter = 1; chapter <= 6; ++chapter) {
                text += "CHAPTER " + QByteArray::number(chapter) + "\n\n";
                for (int paragraph = 1; paragraph <= 80; ++paragraph)
                    text += "Paragraph " + QByteArray::number(paragraph)
                            + " of a long chapter, with enough words in it to run "
                              "over more than one line of the page so the pages have something to cut between.\n\n";
            }
            const QString file = desk.dir.filePath("Chronicle - Author.txt");
            if (!Fixtures::write(file, text))
                return {};
            id = library->add(file, meta).id;
        } else {
            id = library->addEntry(meta, i % 3 != 2).id;
        }
        if (id == 0)
            return {};
        if (i % 6 != 5)
            library->setCover(id, madeUpCover(meta.title, QColor(made.color)));
        if (i % 4 == 0)
            library->setReading(id, "read", 3 + i % 3, QString("2026-%1-12").arg(1 + i % 9, 2, 10, QChar('0')));
        else if (i % 4 == 2)
            library->setReading(id, "to-read", 0, {});
        ids << id;
    }
    desk.backend->reload();
    return ids;
}

// A thing of the graph met in a book that has been read: a search finds
// the book by it (BookListModel::knownFor).
inline void knownIn(Library &library, qint64 book, const QString &name) {
    library.setReading(book, "read", 4, {});
    Graph(library).upsert({}, "character", name, {}, {}, GraphPlace{book, 0, 0.1});
}

// What is wrong with what is on screen. One walk of the visible item tree,
// from the window's content down through its popups, noting:
//
//  - a text, a button or a field that lies outside the window, outside the
//    popup or the side pane it is in, or outside something that clips it,
//    where it can't be scrolled into view;
//  - a text wider than the width it was given that neither elides nor
//    wraps, one that ran out of lines without an ellipsis to say so, and a
//    button whose caption is cut;
//  - a text, a button or a field squeezed to nothing;
//  - two controls or texts under one parent that overlap, or any two
//    children of a row, column, grid or flow that do;
//  - under fingers, something to tap that is smaller than a fingertip.
//
// What scrolls is allowed to run past its viewport in the direction it
// scrolls, and in no other.
struct Survey {
    // The smallest side of something a finger is meant to hit. Material's
    // dense buttons are 36 high, which is what the app's own are.
    static constexpr qreal fingertip = 36;
    QQuickWindow *window = nullptr;
    bool touch = false;
    QStringList problems;

    static bool is(const QQuickItem *item, const char *type) { return item->inherits(type); }
    static bool isText(const QQuickItem *item) { return is(item, "QQuickText") && !item->property("text").toString().trimmed().isEmpty(); }
    static bool isField(const QQuickItem *item) { return is(item, "QQuickTextInput") || is(item, "QQuickTextEdit"); }
    static bool isButton(const QQuickItem *item) { return is(item, "QQuickAbstractButton"); }
    static bool isControl(const QQuickItem *item) {
        // The field inside a spin box or a combo box is a part of it, and
        // the box is what is tapped and what is measured.
        if (item->parentItem() && (is(item->parentItem(), "QQuickSpinBox") || is(item->parentItem(), "QQuickComboBox")))
            return false;
        return isButton(item) || is(item, "QQuickTextInput") || is(item, "QQuickComboBox") || is(item, "QQuickSlider")
               || is(item, "QQuickSpinBox") || (is(item, "QQuickTextEdit") && !item->property("readOnly").toBool());
    }
    static bool tapped(const QQuickItem *item) {
        for (QObject *child : item->children())
            if (child->inherits("QQuickTapHandler") && child->property("enabled").toBool())
                return true;
        return false;
    }
    // The one thing that is tapped and may be thinner than a fingertip: a
    // book's spine on the shelf of spines, which is as thin as the book is
    // short. The shelf is the point of that view, the selected book is
    // pulled out wide enough to hit, and the other four views are a key or
    // a tap away.
    static bool thin(const QQuickItem *item) {
        const QQmlContext *context = qmlContext(item->parentItem() ? item->parentItem() : item);
        return item->inherits("QQuickItem") && QString::fromLatin1(item->metaObject()->className()).startsWith("BookTaps") && context
               && context->baseUrl().fileName() == "ShelfSpines.qml";
    }
    static bool arranges(const QQuickItem *item) { return is(item, "QQuickLayout") || is(item, "QQuickBasePositioner"); }

    // "Label “Filter” (filterLabel) in LibraryView.qml at 412,18 60x20, under touchBar < header"
    static QString describe(const QQuickItem *item, const QRectF &rect) {
        QString type = QString::fromLatin1(item->metaObject()->className());
        type.remove("QQuick").remove(QRegularExpression("_QML(TYPE)?_\\d+$"));
        QString said = type;
        const QString text = item->property("text").toString().simplified();
        if (!text.isEmpty())
            said += " “" + (text.size() > 48 ? text.left(47) + "…" : text) + "”";
        if (!item->objectName().isEmpty())
            said += " (" + item->objectName() + ")";
        if (const QQmlContext *context = qmlContext(item))
            if (!context->baseUrl().fileName().isEmpty())
                said += " in " + context->baseUrl().fileName();
        said += QString(" at %1,%2 %3x%4").arg(qRound(rect.x())).arg(qRound(rect.y())).arg(qRound(rect.width())).arg(qRound(rect.height()));
        QStringList above;
        for (const QQuickItem *up = item->parentItem(); up && above.size() < 3; up = up->parentItem())
            if (!up->objectName().isEmpty())
                above << up->objectName();
        if (!above.isEmpty())
            said += ", under " + above.join(" < ");
        return said;
    }
    void note(const QString &what, const QQuickItem *item, const QRectF &rect) { problems << what + ": " + describe(item, rect); }

    void walk() {
        problems.clear();
        walk(window->contentItem(), QRectF(0, 0, window->width(), window->height()), false, false);
    }

    // `room` is where an item under here can be seen, in the window's
    // coordinates; `across` and `down` say whether something above scrolls
    // that way, so that being out of the room in that direction is fine.
    void walk(QQuickItem *item, QRectF room, bool across, bool down) {
        if (!item->isVisible() || item->opacity() <= 0.01)
            return;
        const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        const bool sized = item->width() >= 1 && item->height() >= 1;
        // What a scrolling view has scrolled out of sight is not on screen,
        // and is looked at when it is.
        if ((across || down) && sized && !rect.intersects(room))
            return;

        if (is(item, "QQuickPopupItem")) {
            // A dialog is inside the window, whole, and what is in it is inside the dialog.
            if (!room.adjusted(-1, -1, 1, 1).contains(rect))
                note("a dialog larger than the window", item, rect);
            room = rect & room;
            across = down = false;
        }
        // A side pane is a room of its own: what is in it stays in it, and
        // never runs over the page or the header beside it (#122).
        if (QString::fromLatin1(item->metaObject()->className()).startsWith("SidePane_QML"))
            room = rect & room;
        lookAt(item, rect, room, across, down);
        narrow(item, rect, room, across, down);
        overlaps(item);
        for (QQuickItem *child : item->childItems())
            walk(child, room, across, down);
    }

    // What is wrong with an item itself: out of where it can be seen, its
    // words cut, too small to tap.
    void lookAt(QQuickItem *item, const QRectF &rect, const QRectF &room, bool across, bool down) {
        const bool text = isText(item), control = isControl(item);
        const bool sized = item->width() >= 1 && item->height() >= 1;
        if ((text || control) && !sized)
            note("squeezed to nothing", item, rect);
        else if (text || control)
            lookWhere(item, rect, room, across, down);
        if (text && sized)
            lookAtWords(item, rect);
        if (touch && sized && (control || tapped(item)) && tooSmall(item))
            note("too small to tap", item, rect);
    }

    // Something to tap that a finger can't hit.
    static bool tooSmall(const QQuickItem *item) {
        return item->isEnabled() && (item->width() < fingertip - 0.5 || item->height() < fingertip - 0.5) && !thin(item);
    }

    void lookWhere(QQuickItem *item, const QRectF &rect, const QRectF &room, bool across, bool down) {
        const QRectF fair = room.adjusted(-1.5, -1.5, 1.5, 1.5);
        const bool outAcross = rect.left() < fair.left() || rect.right() > fair.right();
        const bool outDown = rect.top() < fair.top() || rect.bottom() > fair.bottom();
        if ((outAcross && !across) || (outDown && !down))
            note(rect.intersects(room) ? "cut off by its window, dialog or pane" : "off screen", item, rect);
    }

    void lookAtWords(QQuickItem *item, const QRectF &rect) {
        const bool elides = item->property("elide").toInt() != Qt::ElideNone;
        const bool cut = item->property("truncated").toBool();
        const bool caption = item->parentItem() && isButton(item->parentItem());
        if (!elides && item->property("contentWidth").toReal() > item->width() + 1.5)
            note("text wider than its width, with nothing to elide or wrap it", item, rect);
        else if (!elides && cut)
            note("text cut off with no ellipsis", item, rect);
        else if (cut && caption)
            note("a button's caption cut short", item, rect);
        else if (cut && item->width() < 24)
            note("text elided to nothing", item, rect);
    }

    // What is under a clip can't be seen outside it; what is in a
    // scrolling view can be scrolled to.
    static void narrow(QQuickItem *item, const QRectF &rect, QRectF &room, bool &across, bool &down) {
        if (is(item, "QQuickFlickable")) {
            room = rect & room;
            const bool list = is(item, "QQuickListView");
            const bool sideways = list && item->property("orientation").toInt() == Qt::Horizontal;
            across = across || sideways || (!list && item->property("contentWidth").toReal() > item->width() + 1);
            down = down || (list && !sideways) || is(item, "QQuickGridView")
                   || (!list && item->property("contentHeight").toReal() > item->height() + 1);
        } else if (item->clip()) {
            room = rect & room;
        }
    }

    // Brothers and sisters that overlap.
    void overlaps(QQuickItem *item) {
        const bool arranged = arranges(item);
        QList<QPair<QQuickItem *, QRectF>> placed;
        for (QQuickItem *child : item->childItems()) {
            if (!child->isVisible() || child->opacity() <= 0.01 || child->width() < 1 || child->height() < 1)
                continue;
            if (arranged || isText(child) || isControl(child))
                placed.append({child, child->mapRectToScene(QRectF(0, 0, child->width(), child->height()))});
        }
        if (placed.size() > 80)
            return;
        for (int i = 0; i < placed.size(); ++i) {
            for (int j = i + 1; j < placed.size(); ++j) {
                const QRectF both = placed[i].second & placed[j].second;
                if (both.width() > 2 && both.height() > 2)
                    problems << "overlapping: " + describe(placed[i].first, placed[i].second) + " AND "
                                    + describe(placed[j].first, placed[j].second);
            }
        }
    }
};

// What the commands need to be there: a plugin with a command for the
// shelf, one for the page and a view of its own, and a thing known of
// `book` for its graph with an event of it, timed, for its chronology,
// which is answered (0 when it couldn't be).
inline qint64 forEveryCommand(Desk &desk, qint64 book) {
    QDir().mkpath(desk.dir.filePath("plugins/shelver"));
    Fixtures::write(desk.dir.filePath("plugins/shelver/View.qml"), "import QtQuick\nItem {}\n");
    Fixtures::write(desk.dir.filePath("plugins/shelver/plugin.json"), "{\"name\": \"Shelver\", \"script\": \"main.js\"}");
    Fixtures::write(desk.dir.filePath("plugins/shelver/main.js"),
                    "omabook.commands.register({id: \"shelve\", title: \"Shelve it\", place: \"library\", key: \"Ctrl+Shift+E\", "
                    "run: function () {}});\n"
                    "omabook.commands.register({id: \"note\", title: \"Note the page\", place: \"reader\", run: function () {}});\n"
                    "omabook.ui.view({id: \"shelf\", title: \"Shelf notes\", source: \"View.qml\"});\n");
    desk.backend->scripts()->reload();
    Graph known(*desk.backend->library());
    known.upsert({}, "character", "Wilhelmina Overbeck", "Keeps the long lighthouse.", {}, GraphPlace{book, 0, 0.1}, "Wilhelmina");
    const qint64 storm = known.upsert({}, "event", "The storm", {}, {}, GraphPlace{book, 0, 0.2});
    const auto year = EventTime::parse(QString(R"({"calendar": "gregorian", "at": "1911"})"));
    return year && Chronology::setWhen(known, storm, *year) ? storm : 0;
}

// A voice that holds its sentence and never finishes it, for the reader's
// controls of reading aloud to be there (#73).
class HeldVoice : public Voice {
public:
    void say(const QString &, const Speech &) override {}
    void stop() override {}
    [[nodiscard]] QString name() const override { return QStringLiteral("Stand-in"); }
};

// A book open in the reader and read aloud by a voice that holds its
// sentence, so its control is over the page (#73); whether it is.
inline bool readingAloud(Desk &desk, qint64 book) {
    desk.reader->openBook(book);
    if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.shows("touchArea"); }, 5000))
        return false;
    desk.reader->readAloud()->setVoiceMaker(
        [] { return std::expected<std::unique_ptr<Voice>, Failure>(std::make_unique<HeldVoice>()); }, "Stand-in");
    desk.reader->readAloud()->toggle();
    return QTest::qWaitFor([&] { return desk.shows("readAloudBar"); }, 5000);
}

// Of every command and every plugin's command, those without a control
// (`reached`: the smaller side of the largest of each), under fingers
// those smaller than a fingertip, and anything said to be the way to a
// command there isn't (layout_tests: everyCommandIsThereForAMouseAndAFinger).
inline QStringList unreached(Desk &desk, const QHash<QString, qreal> &reached, const QHash<QString, QString> &keyboardOnly) {
    QStringList wanted, missing, small, problems;
    for (const Customization::Command &command : Customization::commands())
        wanted << command.id;
    for (const QVariant &command : desk.custom->pluginCommands())
        wanted << command.toMap().value("id").toString();
    if (!wanted.contains("library.shelver:shelve") || !wanted.contains("reader.shelver:note"))
        problems << "the plugin's commands are not there";
    for (const QString &id : std::as_const(wanted)) {
        if (keyboardOnly.contains(id))
            continue;
        if (!reached.contains(id))
            missing << id;
        else if (desk.touch && reached.value(id) < Survey::fingertip - 0.5)
            small << QString("%1 (%2 px)").arg(id).arg(reached.value(id));
    }
    if (!missing.isEmpty())
        problems << "no control for " + missing.join(", ");
    if (!small.isEmpty())
        problems << "smaller than a fingertip: " + small.join(", ");
    QStringList unknown;
    for (auto it = reached.cbegin(); it != reached.cend(); ++it)
        if (!wanted.contains(it.key()))
            unknown << it.key();
    if (!unknown.isEmpty())
        problems << "no such command: " + unknown.join(", ");
    return problems;
}

inline const QByteArray lightTheme = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                     "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";

inline bool whole() { return qEnvironmentVariableIntValue("OMABOOK_LAYOUT_FULL") > 0; }

// The sizes a tiled window comes in. Every width and every height is in
// the sample that runs on every push once; the whole matrix, every
// width with every height, is for looking a change over.
inline QList<QSize> sizes() {
    // OMABOOK_LAYOUT_SIZES=480x800,1180x780 looks at those alone.
    const QString only = qEnvironmentVariable("OMABOOK_LAYOUT_SIZES");
    if (!only.isEmpty()) {
        QList<QSize> named;
        for (const QString &size : only.split(','))
            named << QSize(size.section('x', 0, 0).toInt(), size.section('x', 1, 1).toInt());
        return named;
    }
    static const QList<int> widths{420, 480, 560, 640, 720, 800, 960, 1180, 1440, 1920, 2560};
    static const QList<int> heights{360, 480, 640, 800, 1200};
    if (!whole())
        return {QSize(420, 360), QSize(480, 800),  QSize(560, 480),  QSize(640, 640),   QSize(720, 480), QSize(800, 1200),
                QSize(960, 640), QSize(1180, 780), QSize(1440, 360), QSize(1920, 1200), QSize(2560, 800)};
    QList<QSize> all;
    for (int width : widths)
        for (int height : heights)
            all << QSize(width, height);
    return all;
}
// Dialogs stop growing at about a thousand pixels, so they are looked
// at where they are squeezed, and once where they are not.
inline QList<QSize> dialogSizes() {
    if (qEnvironmentVariableIsSet("OMABOOK_LAYOUT_SIZES"))
        return sizes();
    if (!whole())
        return {QSize(420, 360), QSize(480, 800), QSize(640, 480), QSize(800, 640), QSize(1180, 780)};
    return {QSize(420, 360), QSize(420, 800), QSize(480, 480), QSize(480, 800),  QSize(560, 360),  QSize(640, 480),
            QSize(720, 640), QSize(800, 640), QSize(960, 480), QSize(1180, 780), QSize(1920, 1200)};
}
