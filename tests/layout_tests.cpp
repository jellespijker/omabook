#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPointingDevice>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <functional>
#include <optional>

#include "aisearch.h"
#include "assistant.h"
#include "backend.h"
#include "customization.h"
#include "fixtures.h"
#include "graph.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "pageformat.h"
#include "pagelayout.h"
#include "reader.h"
#include "settings.h"
#include "subscriptions.h"
#include "theme.h"
#include "themechoice.h"
#include "translator.h"

// The window at every size it is given. Omabook lives on a tiling desktop,
// so it is a narrow column one moment and a whole 4K screen the next, and a
// layout that was only ever looked at 1180 pixels wide breaks at the others
// without anyone seeing it. These tests show the real Main.qml with a
// made-up library at a matrix of sizes, in English and in German (whose
// words are a third longer), walk every item that is on screen, and fail
// when one of them is somewhere it can't be read or reached. See `Survey`
// for exactly what counts.
//
// OMABOOK_SHOTS=<folder> saves a picture of every state at every size;
// OMABOOK_LAYOUT_FULL=1 walks the whole matrix instead of the sample that
// runs on every push; OMABOOK_LAYOUT_REPORT=<file> writes every problem
// found to a file as well as failing.

namespace {

// The reading font's character width (PageLayout::characterWidth) on the
// machines the window tests were measured on; a wider or narrower font
// scales them.
constexpr double referenceCharacterWidth = 9.04;

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
            for (const char *title : {"Moss", "The Unabridged and Thoroughly Annotated Chronicle of the Lantern Keepers of the Northern Salt Roads",
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
        backend.emplace(&*settings, &picker, nullptr, makeAi,
                        [](QObject *parent) { return QList<MetadataSource *>{new FakeCatalog(parent)}; },
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
        const bool up = QTest::qWaitFor([&] {
            const QList<QObject *> open = popups();
            return !open.isEmpty() && std::all_of(open.begin(), open.end(), [](QObject *popup) { return popup->property("opened").toBool(); });
        }, 3000);
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

QImage madeUpCover(const QString &title, const QColor &color) {
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
QList<qint64> stock(Desk &desk) {
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
        {"Donaudampfschifffahrtsgesellschaftskapitänsmützenabzeichen", "Wilhelmina Konstantinopolitanska", "", 0, "History",
         "#7a3b2e"},
        {"Notes on Slow Rivers", "Edda Marlowe", "", 0, "Essays", "#e8dcc0"},
        {"Thirteen Ways to Fold a Letter So That It Says Something Other Than What Was Written in It, with Diagrams",
         "Hana Sato", "", 0, "Essays", "#d98c5f"},
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
                    text += "Paragraph " + QByteArray::number(paragraph) + " of a long chapter, with enough words in it to run "
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

// What is wrong with what is on screen. One walk of the visible item tree,
// from the window's content down through its popups, noting:
//
//  - a text, a button or a field that lies outside the window, outside the
//    popup it is in, or outside something that clips it, where it can't be
//    scrolled into view;
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
        return item->inherits("QQuickItem") && QString::fromLatin1(item->metaObject()->className()).startsWith("BookTaps")
               && context && context->baseUrl().fileName() == "ShelfSpines.qml";
    }
    static bool arranges(const QQuickItem *item) {
        return is(item, "QQuickLayout") || is(item, "QQuickBasePositioner");
    }

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
        const bool text = isText(item), control = isControl(item);
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

        if (text || control) {
            if (!sized) {
                note("squeezed to nothing", item, rect);
            } else {
                const QRectF fair = room.adjusted(-1.5, -1.5, 1.5, 1.5);
                const bool outAcross = rect.left() < fair.left() || rect.right() > fair.right();
                const bool outDown = rect.top() < fair.top() || rect.bottom() > fair.bottom();
                if ((outAcross && !across) || (outDown && !down))
                    note(rect.intersects(room) ? "cut off by its window, dialog or pane" : "off screen", item, rect);
            }
        }
        if (text && sized) {
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
        if (touch && sized && (control || tapped(item)) && item->isEnabled()
            && (item->width() < fingertip - 0.5 || item->height() < fingertip - 0.5) && !thin(item))
            note("too small to tap", item, rect);

        // What is under a clip can't be seen outside it; what is in a
        // scrolling view can be scrolled to.
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

        // Brothers and sisters that overlap.
        const bool arranged = arranges(item);
        QList<QPair<QQuickItem *, QRectF>> placed;
        for (QQuickItem *child : item->childItems()) {
            if (!child->isVisible() || child->opacity() <= 0.01 || child->width() < 1 || child->height() < 1)
                continue;
            if (arranged || isText(child) || isControl(child))
                placed.append({child, child->mapRectToScene(QRectF(0, 0, child->width(), child->height()))});
        }
        if (placed.size() <= 80) {
            for (int i = 0; i < placed.size(); ++i) {
                for (int j = i + 1; j < placed.size(); ++j) {
                    const QRectF both = placed[i].second & placed[j].second;
                    if (both.width() > 2 && both.height() > 2)
                        problems << "overlapping: " + describe(placed[i].first, placed[i].second) + " AND "
                                        + describe(placed[j].first, placed[j].second);
                }
            }
        }

        for (QQuickItem *child : item->childItems())
            walk(child, room, across, down);
    }
};

const QByteArray lightTheme = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                              "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";

} // namespace

class LayoutTests : public QObject {
    Q_OBJECT

    // Every problem found, by where it was found: "de 480x800 list".
    QStringList found;
    int looked = 0;

    static bool whole() { return qEnvironmentVariableIntValue("OMABOOK_LAYOUT_FULL") > 0; }

    // The sizes a tiled window comes in. Every width and every height is in
    // the sample that runs on every push once; the whole matrix, every
    // width with every height, is for looking a change over.
    static QList<QSize> sizes() {
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
            return {QSize(420, 360),  QSize(480, 800),  QSize(560, 480),   QSize(640, 640),   QSize(720, 480),  QSize(800, 1200),
                    QSize(960, 640),  QSize(1180, 780), QSize(1440, 360),  QSize(1920, 1200), QSize(2560, 800)};
        QList<QSize> all;
        for (int width : widths)
            for (int height : heights)
                all << QSize(width, height);
        return all;
    }
    // Dialogs stop growing at about a thousand pixels, so they are looked
    // at where they are squeezed, and once where they are not.
    static QList<QSize> dialogSizes() {
        if (qEnvironmentVariableIsSet("OMABOOK_LAYOUT_SIZES"))
            return sizes();
        if (!whole())
            return {QSize(420, 360), QSize(480, 800), QSize(640, 480), QSize(800, 640), QSize(1180, 780)};
        return {QSize(420, 360), QSize(420, 800), QSize(480, 480), QSize(480, 800), QSize(560, 360), QSize(640, 480),
                QSize(720, 640), QSize(800, 640), QSize(960, 480), QSize(1180, 780), QSize(1920, 1200)};
    }

    void look(Desk &desk, const QString &state) {
        desk.settle(1);
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        ++looked;
        const QString where = QString("%1%2 %3x%4 %5").arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width()).arg(desk.window->height()).arg(state);
        QSet<QString> once;
        for (const QString &problem : std::as_const(survey.problems))
            if (!once.contains(problem)) {
                once.insert(problem);
                found << where + " — " + problem;
            }
        desk.shot(QString("%1%2-%3-%4x%5").arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width()).arg(desk.window->height()));
    }

    // A dialog, opened, looked at, and closed again.
    template <typename Open> void dialog(Desk &desk, const QString &state, Open open) {
        desk.quicken();
        open();
        if (!desk.waitForPopup())
            found << QString("%1 %2x%3 %4 — the dialog did not open").arg(QString::fromLatin1(desk.language))
                         .arg(desk.window->width()).arg(desk.window->height()).arg(state);
        look(desk, state);
        if (!desk.closePopups())
            found << state + " — the dialog did not close";
    }

    // The shelf in every view, with and without the details, narrowed by
    // filters and a search, and saying something.
    void shelves(Desk &desk, const QList<qint64> &ids) {
        QObject *books = desk.backend->books();
        for (const QSize &size : sizes()) {
            QVERIFY(desk.resize(size));
            for (const char *view : {"covers", "spines", "carousel", "list", "stacks"}) {
                desk.backend->setView(view);
                QTRY_VERIFY(desk.find("shelfLoader")->property("status").toInt() == 1);
                look(desk, view);
            }
            // The details hidden, in the two views that have a pane for them.
            QVERIFY(desk.run("library.details"));
            look(desk, "stacks-bare");
            desk.backend->setView("list");
            look(desk, "list-bare");
            desk.backend->setView("covers");
            look(desk, "covers-bare");
            QVERIFY(desk.run("library.details"));

            // Narrowed five ways, which is a row of chips, and by a search.
            QMetaObject::invokeMethod(books, "setFilter", Q_ARG(QString, "status"), Q_ARG(QVariant, "read"));
            QMetaObject::invokeMethod(books, "setFilter", Q_ARG(QString, "tag"), Q_ARG(QVariant, "A rather long tag for the shelf"));
            QMetaObject::invokeMethod(books, "setFilter", Q_ARG(QString, "language"), Q_ARG(QVariant, "en"));
            QMetaObject::invokeMethod(books, "setFilter", Q_ARG(QString, "author"),
                                      Q_ARG(QVariant, "Maximiliana Oluwaseun Featherstonehaugh-Abernathy"));
            QMetaObject::invokeMethod(books, "setFilter", Q_ARG(QString, "held"), Q_ARG(QVariant, "digital"));
            look(desk, "filtered");
            books->setProperty("query", "nothing is called this");
            look(desk, "no-match");
            books->setProperty("query", "");
            QMetaObject::invokeMethod(books, "clearFilters");

            desk.backend->say(desk.language == "de"
                                  ? QString::fromUtf8("„Die ungekürzte und gründlich kommentierte Chronik der Laternenwärter“ wurde der Bibliothek hinzugefügt.")
                                  : QString("“The Unabridged and Thoroughly Annotated Chronicle of the Lantern Keepers” was added to the library."));
            look(desk, "toast");
            desk.backend->dismissMessage();
        }
        Q_UNUSED(ids)
    }

    // The open book, alone and with each of its panes, and two at once.
    void pages(Desk &desk, qint64 book) {
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("readerView"));
        QObject *view = desk.find("readerView");
        const auto show = [&](const char *pane, bool yes) {
            view->setProperty(pane, yes);
            desk.settle();
        };
        for (const QSize &size : sizes()) {
            QVERIFY(desk.resize(size));
            look(desk, "reader");
            // One, two and three pages asked for: what shows fits the
            // window, each page of a spread keeps the minimum measure, and
            // no page stands over another or past the reading area.
            for (const int asked : {1, 2, 3}) {
                desk.settings->setPages(asked);
                desk.settle();
                look(desk, QString("reader-pages-%1").arg(asked));
                const PageFormat *format = desk.reader->format();
                const QString where = QString("%1x%2 %3 pages").arg(size.width()).arg(size.height()).arg(asked);
                if (format->columns() > asked || (format->pagesHonoured() && format->columns() != asked))
                    found << where + QString(" — %1 show").arg(format->columns());
                QQuickItem *area = desk.item("contentLoader");
                const QRectF room = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()));
                QRectF before;
                for (int index = 0; index < format->columns() && format->columns() > 1; ++index) {
                    QQuickItem *page = desk.item(QString("page%1").arg(index));
                    if (!page || !page->isVisible()) {
                        found << where + QString(" — page %1 is not there").arg(index);
                        continue;
                    }
                    const QRectF at = page->mapRectToScene(QRectF(0, 0, page->width(), page->height()));
                    if (!room.adjusted(-0.5, -0.5, 0.5, 0.5).contains(at))
                        found << where + QString(" — page %1 is outside the reading area").arg(index);
                    if (at.width() < PageLayout::minimumMeasure * format->characterWidth() - 0.5)
                        found << where + QString(" — page %1 is %2 wide, under the minimum measure").arg(index).arg(at.width());
                    if (at.width() > format->lineWidth() + 0.5)
                        found << where + QString(" — page %1 is wider than the measure").arg(index);
                    if (index > 0 && before.intersects(at))
                        found << where + QString(" — page %1 lies over the one before").arg(index);
                    before = at;
                }
            }
            desk.settings->setPages(0);
            desk.settle();
            show("showBar", true);
            look(desk, "reader-bar");
            show("showBar", false);
            show("showIndex", true);
            look(desk, "reader-contents");
            show("showAssistant", true);
            look(desk, "reader-contents-assistant");
            show("showIndex", false);
            look(desk, "reader-assistant");
            show("showGraph", true);
            look(desk, "reader-graph-assistant");
            show("showAssistant", false);
            look(desk, "reader-graph");
            show("showGraph", false);
            // The "Aa" panel: beside the page, or along its foot in a
            // compact window; and with the contents open too.
            show("showFormat", true);
            look(desk, "reader-format");
            show("showIndex", true);
            look(desk, "reader-contents-format");
            show("showIndex", false);
            show("showFormat", false);
        }
        for (const QSize &size : dialogSizes()) {
            QVERIFY(desk.resize(size));
            dialog(desk, "find", [&] { desk.run("reader.find"); });
            dialog(desk, "menu-reader", [&] { desk.menu("reader"); });
            // The menu beside selected words, under them and near the foot
            // of the page, and the hint while the keyboard selects.
            QObject *menu = desk.find("readingMenu");
            QVERIFY(menu);
            for (const double down : {0.1, 0.9}) {
                const QVariantMap chosen{{"text", "a few words of the chapter, selected to ask about"},
                                         {"paragraph", ""},
                                         {"rect", QRectF(size.width() * 0.3, size.height() * down, 120, 20)}};
                QMetaObject::invokeMethod(menu, "openMenu", Q_ARG(QVariant, chosen));
                desk.settle();
                look(desk, "reader-menu");
                QMetaObject::invokeMethod(menu, "close");
                desk.settle();
            }
            show("selecting", true);
            look(desk, "reader-selecting");
            show("selecting", false);
        }
        // Every pane dragged as narrow and as wide as it goes: the page
        // keeps its floor, and nothing in a pane is cut at its narrowest.
        for (const int dragged : {1, 4000}) {
            for (const char *pane : {"contents", "assistant", "graph", "details"})
                desk.settings->setPaneWidth(pane, dragged);
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                show("showIndex", true);
                show("showAssistant", true);
                look(desk, dragged == 1 ? "panes-narrowest" : "panes-widest");
                QQuickItem *page = desk.item("contentLoader");
                QVERIFY(page);
                QQuickItem *contents = desk.item("contentsSide"), *asked = desk.item("assistantSide");
                if (!contents->property("over").toBool()) {
                    // Beside the page: the page has what the floor promises.
                    if (page->width() < desk.window->property("contentFloor").toReal() - 0.5)
                        found << QString("%1x%2 panes %3 — the page is squeezed to %4 beside its panes")
                                     .arg(size.width()).arg(size.height()).arg(dragged).arg(page->width());
                } else if (contents->isVisible() && asked->isVisible()) {
                    found << QString("%1x%2 panes %3 — two panes lie over the page at once").arg(size.width()).arg(size.height()).arg(dragged);
                }
                show("showIndex", false);
                show("showAssistant", false);
            }
        }
        for (const char *pane : {"contents", "assistant", "graph", "details"})
            desk.settings->setPaneWidth(pane, 0);
        desk.reader->close();
        QTRY_VERIFY(!desk.reader->isOpen());
        // A book that can't be opened says why, in place of the page.
        // The book's file is gone from under it, as when a disk is not there.
        QVERIFY(QFile::remove(desk.backend->library()->formatPath(book)));
        desk.reader->openBook(book);
        QTRY_VERIFY(!desk.reader->error().isEmpty());
        for (const QSize &size : dialogSizes()) {
            QVERIFY(desk.resize(size));
            look(desk, "reader-error");
        }
        desk.reader->close();
        QTRY_VERIFY(!desk.reader->isOpen());
    }

    // Every dialog and popup the window has.
    void dialogs(Desk &desk, qint64 book) {
        for (const QSize &size : dialogSizes()) {
            QVERIFY(desk.resize(size));
            dialog(desk, "help", [&] { desk.call("showHelp"); });
            dialog(desk, "settings", [&] { desk.call("showSettings"); });
            dialog(desk, "themes", [&] { desk.call("showThemes"); });
            dialog(desk, "libraries", [&] { desk.call("showLibraries"); });
            dialog(desk, "plugins", [&] { desk.call("showPlugins"); });
            // The sheets the plugin manager opens over itself.
            dialog(desk, "plugin-new", [&] {
                desk.call("showPlugins");
                desk.waitForPopup();
                QTest::keyClick(desk.window, Qt::Key_N);
                QTest::qWait(50);
            });
            dialog(desk, "plugin-install", [&] {
                desk.call("showPlugins");
                desk.waitForPopup();
                QTest::keyClick(desk.window, Qt::Key_I);
                QTest::qWait(50);
            });
            // A plugin that came from a GitHub repository, with an update that
            // waits: its details say where it is from and what can be done,
            // and the same sheet shows the update for a yes or a no. The
            // source is written as Omabook would have; nothing asks the network.
            const QString storyteller = desk.dir.filePath("plugins/storyteller");
            if (!QFileInfo::exists(storyteller)) {
                QDir().mkpath(storyteller);
                Fixtures::write(storyteller + "/plugin.json",
                                "{\"name\": \"Storyteller\", \"version\": \"1.0\", \"author\": \"Someone\", \"description\": \"Writes an "
                                "original book for you, chapter by chapter, as you read it.\", \"script\": \"main.js\", \"permissions\": [\"files\"]}");
                Fixtures::write(storyteller + "/main.js", "omabook.on(\"book.closed\", function (event) {});\n");
                Fixtures::write(desk.dir.filePath("plugins/sources.json"),
                                "{\"sources\": {\"storyteller\": {\"repository\": \"someone/omabook-storyteller\", \"subfolder\": \"\", "
                                "\"channel\": {\"kind\": \"branch\", \"name\": \"main\"}, \"checked_at\": 1800000000, "
                                "\"installed\": {\"version\": \"1.0\", \"title\": \"Storyteller\", \"ref\": \"main\", \"commit\": "
                                "\"ee850f7d23defc4490b813c3168d63527e8f6476\"}, \"latest\": {\"ref\": \"main\", \"commit\": "
                                "\"aa850f7d23defc4490b813c3168d63527e8f6476\"}, \"previous\": {\"version\": \"0.9\"}, \"pending\": {\"version\": "
                                "\"2.0\", \"title\": \"Storyteller\", \"reason\": \"permissions\", \"permissions\": [\"http\", \"run\"]}}}}");
                QDir().mkpath(desk.dir.filePath("plugins/.previous/storyteller"));
                Fixtures::write(desk.dir.filePath("plugins/.previous/storyteller/plugin.json"), "{}");
                desk.backend->pluginManager()->reload();
            }
            dialog(desk, "plugin-source", [&] {
                desk.call("showPlugins");
                desk.waitForPopup();
                QMetaObject::invokeMethod(desk.find("plugins"), "show", Q_ARG(QVariant, QString("folder:storyteller")));
                QTest::qWait(50);
            });
            dialog(desk, "plugin-install-offer", [&] {
                desk.call("showPlugins");
                desk.waitForPopup();
                // What is on offer, as fetching the update would have put it there.
                desk.backend->pluginManager()->consider(storyteller);
                QTest::qWait(50);
                QMetaObject::invokeMethod(desk.find("pluginInstall"), "review");
                QTest::qWait(50);
            });
            dialog(desk, "tidy", [&] {
                desk.call("showTidy");
                // With what it found: the same book twice, an author spelled two ways.
                QVERIFY(QTest::qWaitFor([&] { return !desk.librarian->busy() && desk.find("tidyList") && desk.find("tidyList")->property("count").toInt() > 0; }, 3000));
            });
            dialog(desk, "sites", [&] { desk.call("showReadingSites"); });
            dialog(desk, "graph-build", [&] { desk.call("showGraphBuild", book); });
            dialog(desk, "ask", [&] { desk.call("showAskLibrary"); });
            dialog(desk, "change", [&] { desk.run("global.change"); });
            dialog(desk, "edit", [&] { desk.call("editBook", book); });
            dialog(desk, "new", [&] { desk.call("newBook"); });
            dialog(desk, "lookup", [&] {
                desk.call("lookUpBook", book);
                QVERIFY(QTest::qWaitFor([&] { return desk.find("lookupList") && desk.find("lookupList")->property("count").toInt() > 0; }, 3000));
            });
            dialog(desk, "remove", [&] { desk.call("removeBook", book); });
            // What a plugin asks, drawn by Omabook; and its entry on the shelf, taken off.
            dialog(desk, "plugin-form", [&] { desk.backend->scripts()->run("former:form"); });
            dialog(desk, "remove-launcher", [&] { desk.call("removeBook", desk.backend->books()->idAt(0)); });
            dialog(desk, "filter", [&] { desk.run("library.filter"); });
            // What the ⋯ of the shelf lists, and what a book's does.
            dialog(desk, "menu-library", [&] { desk.menu("library"); });
            dialog(desk, "menu-book", [&] { desk.menu("book"); });
            // The whole library as a graph, which takes the shelf's place.
            desk.call("showGraph", 0);
            QTRY_VERIFY(desk.find("libraryGraph"));
            look(desk, "graph");
            // A thing selected, which brings its details beside the picture
            // or under it; then the same as a list.
            QObject *graph = desk.find("libraryGraph");
            QObject *browser = graph->property("browser").value<QObject *>();
            const QVariantList rows = browser ? browser->property("rows").toList() : QVariantList();
            if (!rows.isEmpty()) {
                QMetaObject::invokeMethod(browser, "select", Q_ARG(qint64, rows.first().toMap().value("id").toLongLong()));
                graph->setProperty("peek", true);
                desk.settle();
                look(desk, "graph-details");
                graph->setProperty("listing", true);
                desk.settle();
                look(desk, "graph-list");
                graph->setProperty("listing", false);
                graph->setProperty("peek", false);
                QMetaObject::invokeMethod(browser, "select", Q_ARG(qint64, 0));
            } else {
                found << "graph — the library's graph has nothing in it to select";
            }
            // A thing of a book being read, with what is known of it later
            // kept closed; then asked about; then open.
            {
                Library *books = desk.backend->library();
                BookMeta letters;
                letters.title = "The Lighthouse Letters";
                letters.authors = {"Idris Vance"};
                // Made once: the looks run again at every size.
                qint64 reading = 0;
                for (const Book &held : books->books())
                    if (held.meta.title == letters.title)
                        reading = held.id;
                if (reading == 0)
                    reading = books->addEntry(letters, true).id;
                books->setReading(reading, "reading", 0, {});
                books->setProgress(reading, {0, 0.2, 10, 0});
                Graph facts(*books);
                const qint64 thing = facts.upsert({}, "character", "Wilhelmina Overbeck-Saltonstall",
                                                  "Keeps the long lighthouse at the end of the world, and writes to nobody.", {},
                                                  GraphPlace{reading, 0, 0.1}, "Wilhelmina");
                facts.learn(thing, "summary", "The keeper's daughter, who has been writing the letters all along, and who knows "
                                              "where the ship went down on the night of the great storm.",
                            GraphMoment{{reading, 3, 0.5}, "who has been writing the letters"});
                facts.learn(thing, "alias", "The Letter Writer of the Northern Lighthouse", GraphMoment{{reading, 3, 0.6}, "The Letter Writer"});
                QMetaObject::invokeMethod(browser, "refresh");
                desk.settle();
                QMetaObject::invokeMethod(browser, "jumpTo", Q_ARG(qint64, thing));
                graph->setProperty("peek", true);
                desk.settle();
                look(desk, "graph-spoilers-closed");
                if (QObject *details = desk.find("graphDetails"))
                    details->setProperty("askingFor", thing);
                desk.settle();
                look(desk, "graph-spoilers-asking");
                QMetaObject::invokeMethod(browser, "showAhead", Q_ARG(qint64, thing), Q_ARG(bool, true));
                desk.settle();
                look(desk, "graph-spoilers-open");
                QMetaObject::invokeMethod(browser, "showAhead", Q_ARG(qint64, thing), Q_ARG(bool, false));
                graph->setProperty("peek", false);
            }
            QMetaObject::invokeMethod(desk.find("libraryGraph"), "done");
            desk.settle();
        }
    }

    void survey(const QByteArray &language, const QByteArray &colors, bool touch) {
        Desk desk(language, colors, touch);
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
            desk.window->setMinimumSize(QSize(420, 360));
        shelves(desk, ids);
        if (QTest::currentTestFailed()) return;
        pages(desk, ids.first());
        if (QTest::currentTestFailed()) return;
        // A plugin with an entry on the shelf and a form of every kind of
        // field, which the dialogs below put up.
        QDir().mkpath(desk.dir.filePath("plugins/former"));
        for (const QString file : {"plugin.json", "main.js"})
            QVERIFY(QFile::copy(QFINDTESTDATA("data/former-plugin/" + file), desk.dir.filePath("plugins/former/" + file)));
        desk.backend->scripts()->reload();
        QTRY_VERIFY(desk.backend->books()->idAt(0) < 0);
        dialogs(desk, ids.first());
        if (QTest::currentTestFailed()) return;
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Fails with what was found since the last call, each problem once with
    // the sizes it was found at.
    void verdict() {
        const QString report = qEnvironmentVariable("OMABOOK_LAYOUT_REPORT");
        if (!report.isEmpty()) {
            QFile file(report);
            if (file.open(QIODevice::Append))
                file.write((found.join('\n') + '\n').toUtf8());
        }
        const QStringList problems = found;
        found.clear();
        if (problems.isEmpty())
            return;
        const QString shown = problems.mid(0, 40).join('\n');
        QFAIL(qPrintable(QString("%1 layout problems in %2 looks:\n%3%4").arg(problems.size()).arg(looked).arg(shown,
                         problems.size() > 40 ? "\n… and more; set OMABOOK_LAYOUT_REPORT to a file for all of them" : "")));
    }

    // A click or a tap in the middle of something that is on screen.
    static QPoint middle(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }
    static void click(Desk &desk, const QString &name) {
        QQuickItem *item = desk.item(name);
        QVERIFY2(item && item->isVisible(), qPrintable(name));
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(item));
    }
    // The handle of a pane dragged by `by` pixels across, with the mouse.
    static void drag(Desk &desk, const QString &handle, int by) {
        QQuickItem *item = desk.item(handle);
        QVERIFY2(item && item->isVisible(), qPrintable(handle));
        const QPoint from = middle(item);
        QTest::mousePress(desk.window, Qt::LeftButton, Qt::NoModifier, from);
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(desk.window, from + QPoint(by * step / 8, 0), 5);
        QTest::mouseRelease(desk.window, Qt::LeftButton, Qt::NoModifier, from + QPoint(by, 0), 5);
    }
    static QJsonObject savedWidths(const Desk &desk) {
        QFile file(desk.dir.filePath("config.json"));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().value("pane_widths").toObject()
                                              : QJsonObject();
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The window says how small it goes, and that size is one the survey
    // looks at; the size classes change where their comment says they do.
    void theSizeClassesAreWhatTheySay() {
        Desk desk("en", {}, false);
        QVERIFY(!stock(desk).isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QCOMPARE(desk.window->minimumSize(), QSize(420, 360));
        QVERIFY(sizes().contains(desk.window->minimumSize()) || qEnvironmentVariableIsSet("OMABOOK_LAYOUT_SIZES"));
        struct Class {
            QSize size;
            const char *name;
            bool low;
        };
        for (const Class &expected : {Class{{420, 360}, "compact", true}, Class{{699, 519}, "compact", true},
                                      Class{{700, 520}, "medium", false}, Class{{899, 800}, "medium", false},
                                      Class{{900, 480}, "expanded", true}, Class{{2560, 1200}, "expanded", false}}) {
            QVERIFY(desk.resize(expected.size));
            QCOMPARE(desk.window->property("sizeClass").toString(), QString(expected.name));
            QCOMPARE(desk.window->property("compact").toBool(), QString(expected.name) == "compact");
            QCOMPARE(desk.window->property("medium").toBool(), QString(expected.name) == "medium");
            QCOMPARE(desk.window->property("expanded").toBool(), QString(expected.name) == "expanded");
            QCOMPARE(desk.window->property("low").toBool(), expected.low);
        }
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The header folds in the order it says, never loses anything on the
    // way, and what it folded still works: by pointer, by finger, by key.
    void theHeaderFoldsInOrderAndLosesNothing() {
        for (const QByteArray &language : {QByteArray("en"), QByteArray("de")}) {
            Desk desk(language, {}, false);
            QVERIFY(!stock(desk).isEmpty());
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QObject *header = desk.find("libraryHeader");
            QVERIFY(header);
            int before = 0;
            for (int width = 1600; width >= 420; width -= 20) {
                QVERIFY(desk.resize(QSize(width, 600)));
                const int step = header->property("step").toInt();
                const QByteArray at = language + " " + QByteArray::number(width);
                // Narrower never gives anything back.
                QVERIFY2(step >= before, at);
                before = step;
                QCOMPARE(desk.shows("bookCount"), step < 1);
                QCOMPARE(desk.find("filterLabel")->property("worded").toBool(), step < 3);
                QCOMPARE(desk.find("sort")->property("worded").toBool(), step < 3);
                QCOMPARE(desk.shows("viewSwitcher"), step < 4);
                QCOMPARE(desk.shows("viewChoice"), step >= 4);
                QCOMPARE(desk.shows("search"), step < 5);
                QCOMPARE(desk.shows("searchChoice"), step >= 5);
                // Whatever the step, each thing is there in one form or the other.
                QVERIFY2(desk.shows("filterLabel") && desk.shows("sort"), at);
                // The name is the last to give way, and never goes.
                QQuickItem *name = desk.item("libraryName");
                QVERIFY2(desk.shows("libraryName") && name->width() >= 40, at);
                if (step < 5)
                    QVERIFY2(!name->property("truncated").toBool(), at);
            }
            // The smallest window folds all but the search field, which has
            // room there until fingers bring their buttons; with a wider font
            // than the one this was written on, the search field folds too.
            QVERIFY2(before == 4 || before == 5, QByteArray::number(before));
            QVERIFY(desk.warnings.isEmpty());
            QObject *books = desk.backend->books();

            // The folded switcher lists the views, and takes the one picked.
            click(desk, "viewChoice");
            QVERIFY(desk.waitForPopup());
            QCOMPARE(desk.find("palette")->property("mode").toString(), QString("view"));
            QCOMPARE(desk.find("paletteList")->property("count").toInt(), 6);
            QTest::keyClick(desk.window, Qt::Key_Down);
            QTest::keyClick(desk.window, Qt::Key_Down);
            QTest::keyClick(desk.window, Qt::Key_Down);
            QTest::keyClick(desk.window, Qt::Key_Return);
            QTRY_COMPARE(desk.backend->view(), QString("list"));
            QTRY_VERIFY(desk.popups().isEmpty());
            // And the keys never depended on the header.
            QTest::keyClick(desk.window, '1');
            QTRY_COMPARE(desk.backend->view(), QString("covers"));
            click(desk, "filterLabel");
            QVERIFY(desk.waitForPopup());
            QCOMPARE(desk.find("palette")->property("mode").toString(), QString("filter"));
            QVERIFY(desk.closePopups());
            click(desk, "sort");
            QVERIFY(desk.waitForPopup());
            QCOMPARE(desk.find("palette")->property("mode").toString(), QString("sort"));
            QVERIFY(desk.closePopups());

            // Five filters in a narrow, low window: one row of chips, the
            // rest counted, and the count opens the list they are in.
            QVERIFY(desk.resize(QSize(420, 360)));
            for (const auto &filter : QList<QPair<QString, QVariant>>{{"status", "read"}, {"tag", "Borrowed"}, {"language", "en"},
                                                                      {"held", "digital"}, {"rating", 3}})
                QMetaObject::invokeMethod(books, "setFilter", Q_ARG(QString, filter.first), Q_ARG(QVariant, filter.second));
            desk.settle();
            QQuickItem *chips = desk.item("chips");
            const int fitting = chips->property("fitting").toInt();
            QVERIFY2(fitting >= 1 && fitting < 5, QByteArray::number(fitting));
            QVERIFY(desk.shows("chipsMore"));
            QVERIFY(chips->height() < 2 * 28 + 8);
            click(desk, "chipsMore");
            QVERIFY(desk.waitForPopup());
            QCOMPARE(desk.find("palette")->property("mode").toString(), QString("filter"));
            QVERIFY(desk.closePopups());
            // A taller window gives the chips their second row.
            QVERIFY(desk.resize(QSize(420, 800)));
            QVERIFY(chips->property("fitting").toInt() > fitting);
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }

        // Under fingers the row of buttons folds away, and the ⋯ that is
        // always there lists what they did with everything else.
        Desk desk("de", {}, true);
        QVERIFY(!stock(desk).isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(2560, 800)));
        QVERIFY(desk.shows("touchBar") && desk.shows("libraryMore"));
        QVERIFY(desk.resize(QSize(480, 800)));
        QVERIFY(!desk.shows("touchBar") && desk.shows("libraryMore"));
        QPointingDevice *finger = QTest::createTouchDevice();
        const QPoint at = middle(desk.item("libraryMore"));
        QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
        QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
        QVERIFY(desk.waitForPopup());
        QCOMPARE(desk.find("commandMenu")->property("kind").toString(), QString("library"));
        for (const char *button : {"library.add", "library.filter", "library.suggest", "library.libraries", "global.settings"})
            QVERIFY2(desk.find(QString("menuEntry-") + button), button);
        // "Filtern" among them opens the filters, once the list has gone.
        const QPoint filter = middle(desk.item("menuEntry-library.filter"));
        QTest::touchEvent(desk.window, finger).press(0, filter, desk.window);
        QTest::touchEvent(desk.window, finger).release(0, filter, desk.window);
        QTRY_VERIFY(desk.find("palette")->property("opened").toBool());
        QCOMPARE(desk.find("palette")->property("mode").toString(), QString("filter"));
        QVERIFY(desk.waitForPopup());
        QVERIFY(desk.closePopups());

        // At the smallest size the search folds too.
        QVERIFY(desk.resize(QSize(420, 360)));
        QCOMPARE(desk.find("libraryHeader")->property("step").toInt(), 5);
        QObject *books = desk.backend->books();
        // The folded search opens over the header, by a click and by its key.
        click(desk, "searchChoice");
        QTRY_VERIFY(desk.shows("search"));
        QTRY_VERIFY(desk.find("search")->property("activeFocus").toBool());
        QVERIFY(!desk.shows("libraryName") && !desk.shows("searchChoice"));
        QTRY_VERIFY(desk.item("search")->width() > 300);
        for (const char c : QByteArray("moss"))
            QTest::keyClick(desk.window, c);
        QCOMPARE(books->property("query").toString(), QString("moss"));
        QCOMPARE(books->property("count").toInt(), 1);
        // Enter goes back to the shelf: the field folds, and the search
        // stays, shown as a chip that takes it off again.
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_VERIFY(!desk.shows("search"));
        QVERIFY(desk.shows("searchChoice") && desk.shows("libraryName"));
        QVERIFY(desk.find("searchChoice")->property("lit").toBool());
        QTRY_VERIFY(desk.shows("chip-search"));
        desk.settle();
        click(desk, "chip-search");
        QTRY_COMPARE(books->property("query").toString(), QString());
        QTest::keyClick(desk.window, '/');
        QTRY_VERIFY(desk.shows("search") && desk.find("search")->property("activeFocus").toBool());
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.shows("search"));

        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A pane is as wide as it is dragged, by a mouse, a finger or the keys,
    // within what the window has; a double click puts it back; the width
    // is kept in the settings and followed when the file changes.
    void aPaneIsAsWideAsItIsDragged() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("readerView"));
        QObject *view = desk.find("readerView");
        view->setProperty("showAssistant", true);
        desk.settle();
        QQuickItem *pane = desk.item("assistantSide");
        QQuickItem *page = desk.item("contentLoader");
        QVERIFY(pane && page && pane->isVisible());
        const qreal floor = desk.window->property("contentFloor").toReal();
        QCOMPARE(pane->width(), 400.0);
        QVERIFY(!pane->property("over").toBool());
        QCOMPARE(page->width(), 1180.0 - 400);

        // The handle is on the pane's inner edge; dragging it away from the
        // window's edge widens the pane as it goes.
        QQuickItem *handle = desk.item("assistantSideHandle");
        QVERIFY(handle && handle->isVisible());
        QCOMPARE(middle(handle).x(), int(pane->mapToScene(QPointF(0, 0)).x()));
        drag(desk, "assistantSideHandle", -100);
        QTRY_COMPARE(pane->width(), 500.0);
        QCOMPARE(page->width(), 680.0);
        QCOMPARE(desk.settings->paneWidth("assistant"), 500);
        QCOMPARE(savedWidths(desk).value("assistant").toInt(), 500);
        // Neither further than the page's floor, nor narrower than is of use.
        drag(desk, "assistantSideHandle", -600);
        QTRY_COMPARE(pane->width(), 720.0);
        QVERIFY(page->width() >= floor);
        drag(desk, "assistantSideHandle", 900);
        QTRY_COMPARE(pane->width(), 280.0);
        QCOMPARE(desk.settings->paneWidth("assistant"), 280);
        // A double click puts it back, and forgets the choice.
        QTest::mouseDClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(handle));
        QTRY_COMPARE(pane->width(), 400.0);
        QVERIFY(!savedWidths(desk).contains("assistant"));

        // The keys, which are commands like any other.
        QCOMPARE(desk.custom->keyText().value("global.pane_wider").toString(), QString(">"));
        QTest::keyClick(desk.window, Qt::Key_Escape); // out of the question's field
        QTRY_VERIFY(!pane->isVisible());
        view->setProperty("showAssistant", true);
        desk.item("contentLoader")->forceActiveFocus();
        QTest::keyClick(desk.window, '>');
        QTRY_COMPARE(pane->width(), 440.0);
        QTest::keyClick(desk.window, '<');
        QTest::keyClick(desk.window, '<');
        QTRY_COMPARE(pane->width(), 360.0);
        QCOMPARE(desk.settings->paneWidth("assistant"), 360);

        // A finger drags it too, and has a handle wide enough to catch.
        QVERIFY(desk.run("global.pane_wider"));
        QTRY_COMPARE(pane->width(), 400.0);
        QPointingDevice *finger = QTest::createTouchDevice();
        QPoint at = middle(handle);
        QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
        for (int step = 1; step <= 6; ++step) {
            QTest::touchEvent(desk.window, finger).move(0, at - QPoint(20 * step, 0), desk.window);
            QTest::qWait(5);
        }
        QTest::touchEvent(desk.window, finger).release(0, at - QPoint(120, 0), desk.window);
        QTRY_COMPARE(pane->width(), 520.0);
        QTRY_VERIFY(desk.window->property("touch").toBool());
        QTRY_VERIFY(handle->width() >= Survey::fingertip);
        // A double tap puts it back.
        at = middle(handle);
        for (int tap = 0; tap < 2; ++tap) {
            QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
            QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
        }
        QTRY_COMPARE(pane->width(), 400.0);

        // The window shrinks: the pane keeps what was chosen for as long
        // as the page keeps its floor, then narrows, then lies over the
        // page; and it has the chosen width back when the window grows.
        desk.settings->setPaneWidth("assistant", 600);
        QTRY_COMPARE(pane->width(), 600.0);
        QVERIFY(desk.resize(QSize(900, 780)));
        QCOMPARE(pane->width(), 900.0 - floor);
        QVERIFY(!pane->property("over").toBool());
        QVERIFY(desk.resize(QSize(720, 780)));
        QCOMPARE(pane->width(), 280.0);
        QVERIFY(!pane->property("over").toBool());
        QCOMPARE(page->width(), floor);
        QVERIFY(desk.resize(QSize(700, 780)));
        QVERIFY(pane->property("over").toBool());
        QCOMPARE(page->width(), 700.0);
        QCOMPARE(pane->width(), 600.0);
        // Over the page the handle still drags, and the page is not moved.
        drag(desk, "assistantSideHandle", 150);
        QTRY_COMPARE(pane->width(), 450.0);
        QCOMPARE(page->width(), 700.0);
        QVERIFY(desk.shows("paneScrim"));
        QVERIFY(desk.resize(QSize(420, 780)));
        QCOMPARE(pane->width(), 420.0 - 48);
        QCOMPARE(desk.settings->paneWidth("assistant"), 450);
        QVERIFY(desk.resize(QSize(1180, 780)));
        QCOMPARE(pane->width(), 450.0);
        QVERIFY(!pane->property("over").toBool());

        // The file edited by hand is followed while the app runs.
        QFile file(desk.dir.filePath("config.json"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject config = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        config.insert("pane_widths", QJsonObject{{"assistant", 333}, {"contents", 260}});
        QVERIFY(Fixtures::write(desk.dir.filePath("config.json"), QJsonDocument(config).toJson()));
        QTRY_COMPARE(pane->width(), 333.0);

        // The contents are on the other side, so their handle is on their
        // other edge and drags the other way.
        view->setProperty("showAssistant", false);
        view->setProperty("showIndex", true);
        desk.settle();
        QQuickItem *contents = desk.item("contentsSide");
        QCOMPARE(contents->width(), 260.0);
        QCOMPARE(contents->mapToScene(QPointF(0, 0)).x(), 0.0);
        QCOMPARE(middle(desk.item("contentsSideHandle")).x(), 260);
        drag(desk, "contentsSideHandle", 60);
        QTRY_COMPARE(contents->width(), 320.0);
        // With the contents in the keyboard's hands, the keys are theirs.
        view->setProperty("showAssistant", true);
        QMetaObject::invokeMethod(desk.find("contents"), "takeFocus");
        QTest::keyClick(desk.window, '<');
        QTRY_COMPARE(contents->width(), 280.0);
        QCOMPARE(pane->width(), 333.0);
        desk.reader->close();
        QTRY_VERIFY(!desk.reader->isOpen());

        // The details beside the shelf are a pane like the others.
        QQuickItem *details = desk.item("detailsSide");
        QTRY_VERIFY(details->isVisible());
        QCOMPARE(details->width(), 320.0);
        drag(desk, "detailsSideHandle", -80);
        QTRY_COMPARE(details->width(), 400.0);
        QTest::keyClick(desk.window, '<');
        QTRY_COMPARE(details->width(), 360.0);
        QCOMPARE(desk.settings->paneWidth("details"), 360);
        // In a window with no room beside the shelf they are over it, and
        // only when asked for: I shows them, Escape puts them away.
        QVERIFY(desk.resize(QSize(600, 780)));
        QVERIFY(!details->isVisible());
        QVERIFY(desk.item("shelfLoader")->width() >= 590);
        QVERIFY(desk.run("library.details"));
        QTRY_VERIFY(details->isVisible());
        QVERIFY(details->property("over").toBool() && desk.shows("detailsScrim"));
        QVERIFY(desk.item("shelfLoader")->width() >= 590);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!details->isVisible());
        QVERIFY(desk.resize(QSize(1180, 780)));
        QTRY_VERIFY(details->isVisible() && !details->property("over").toBool());

        // And so is the plugin manager's list.
        desk.call("showPlugins");
        QVERIFY(desk.waitForPopup());
        QQuickItem *plugins = desk.item("pluginsSide");
        QVERIFY(plugins && plugins->isVisible());
        QCOMPARE(plugins->width(), 312.0);
        QTest::keyClick(desk.window, '>');
        QTRY_COMPARE(plugins->width(), 352.0);
        drag(desk, "pluginsSideHandle", -60);
        QTRY_COMPARE(plugins->width(), 292.0);
        QCOMPARE(desk.settings->paneWidth("plugins"), 292);
        QVERIFY(desk.closePopups());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The page keeps its place when its width changes: when a pane opens
    // beside it and two pages become one, when the pane is dragged and the
    // lines are cut anew, and when it closes again. It was at the top of
    // the chapter with the header still saying 50%.
    void thePageKeepsItsPlaceWhenItsWidthChanges() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        // The widths below are in characters of the reading font, so the test
        // asks the same of a machine whose font is wider (DejaVu, the only
        // one in a package build) or narrower: whether two pages fit is a
        // matter of characters, not pixels.
        const double wider = PageLayout::characterWidth(PageLayout::bodyFont(QString(), desk.settings->fontSize()))
                             / referenceCharacterWidth;
        const auto across = [&](int pixels) { return int(std::lround(pixels * wider)); };
        QVERIFY(desk.resize(QSize(across(1180), 780)));
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen());
        // Two pages side by side, turned to the middle of the chapter.
        QTRY_VERIFY(desk.find("spreadReader"));
        QTRY_VERIFY(desk.find("spreadReader")->property("pages").toInt() > 3);
        while (desk.reader->fraction() < 0.45)
            QTest::keyClick(desk.window, Qt::Key_Space);
        const double middle = desk.reader->fraction();
        QVERIFY(middle >= 0.45 && middle < 0.75);
        const auto through = [&]() -> double {
            QObject *flow = desk.find("flowReader");
            return flow && flow->property("range").toReal() > 0 ? flow->property("contentY").toReal() / flow->property("range").toReal() : -1;
        };

        // The AI's pane opens beside the page, which is now too narrow for
        // two: one page, at the same place, and the header agrees.
        QObject *view = desk.find("readerView");
        view->setProperty("showAssistant", true);
        QTRY_VERIFY(desk.find("flowReader"));
        QVERIFY(!desk.item("assistantSide")->property("over").toBool());
        QTRY_VERIFY2(qAbs(through() - middle) < 0.05, QByteArray::number(through()));
        QVERIFY(qAbs(desk.reader->fraction() - middle) < 0.05);
        QVERIFY(desk.find("flowReader")->property("contentY").toReal() > 500);

        // Scrolled by hand, then the pane dragged narrower and wider: the
        // lines are cut anew each time, and the page stays where it was.
        for (int line = 0; line < 3; ++line)
            QTest::keyClick(desk.window, Qt::Key_J);
        const double scrolled = desk.reader->fraction();
        QVERIFY(scrolled > middle - 0.05);
        // The one page's own text; the two pages' is called the same.
        const auto textWidth = [&] { return desk.find("flowReader")->findChild<QObject *>("flowText")->property("width").toReal(); };
        const qreal before = textWidth();
        desk.settings->setPaneWidth("assistant", across(600));
        QTRY_VERIFY(textWidth() < before - 50);
        desk.settle();
        QVERIFY2(qAbs(through() - scrolled) < 0.02, QByteArray::number(through()));
        QVERIFY(qAbs(desk.reader->fraction() - scrolled) < 0.02);
        const qreal narrow = textWidth();
        desk.settings->setPaneWidth("assistant", across(500));
        QTRY_VERIFY(textWidth() > narrow + 50);
        desk.settle();
        QVERIFY2(qAbs(through() - scrolled) < 0.02, QByteArray::number(through()));
        QVERIFY(qAbs(desk.reader->fraction() - scrolled) < 0.02);

        // The pane closed, it is two pages again, on the spread that holds that place.
        view->setProperty("showAssistant", false);
        QTRY_VERIFY(desk.find("spreadReader"));
        QObject *spread = desk.find("spreadReader");
        QTRY_VERIFY(spread->property("pages").toInt() > 3);
        QTRY_VERIFY(qAbs(double(spread->property("page").toInt()) / spread->property("lastLeft").toInt() - scrolled) < 0.15);
        QVERIFY(qAbs(desk.reader->fraction() - scrolled) < 0.15);

        // And the same with the map beside the page, which is how it was found.
        view->setProperty("showGraph", true);
        QTRY_VERIFY(desk.find("flowReader"));
        QTRY_VERIFY2(qAbs(through() - scrolled) < 0.15, QByteArray::number(through()));
        QVERIFY(qAbs(desk.reader->fraction() - scrolled) < 0.15);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Mirrored, a pane is on the other side, and so is its handle; dragging
    // away from the window's edge still widens it.
    void aPaneIsDraggedTheOtherWayRightToLeft() {
        Desk desk("ar", lightTheme, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("readerView"));
        desk.find("readerView")->setProperty("showAssistant", true);
        desk.settle();
        QQuickItem *pane = desk.item("assistantSide");
        QTRY_VERIFY(pane->isVisible());
        QCOMPARE(pane->mapToScene(QPointF(0, 0)).x(), 0.0);
        QCOMPARE(middle(desk.item("assistantSideHandle")).x(), 400);
        drag(desk, "assistantSideHandle", 100);
        QTRY_COMPARE(pane->width(), 500.0);
        QCOMPARE(desk.item("contentLoader")->mapToScene(QPointF(0, 0)).x(), 500.0);
        drag(desk, "assistantSideHandle", -150);
        QTRY_COMPARE(pane->width(), 350.0);
        // Narrow, it lies over the page from the same side.
        QVERIFY(desk.resize(QSize(480, 780)));
        QVERIFY(pane->property("over").toBool());
        QCOMPARE(pane->mapToScene(QPointF(0, 0)).x(), 0.0);
        Survey survey{desk.window, false, {}};
        survey.walk();
        QVERIFY2(survey.problems.isEmpty(), qPrintable(survey.problems.join('\n')));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void inEnglish() {
        survey("en", {}, false);
        verdict();
    }
    void inGermanOnALightTheme() {
        survey("de", lightTheme, false);
        verdict();
    }
    void inGermanUnderFingers() {
        survey("de", {}, true);
        verdict();
    }

    // The first start: an empty shelf that says how to fill it, the offer
    // of a tour over it, and the tour itself, step by step.
    void anEmptyLibraryAndTheTour() {
        // A Calibre library found on the computer is offered there too, in
        // a temp folder that stands in for Calibre's own.
        QTemporaryDir calibre;
        QVERIFY(Fixtures::calibreShelf(calibre.filePath("Eine ziemlich lange Calibre-Bibliothek")));
        QVERIFY(Fixtures::calibreConfig(calibre.filePath("config"), {calibre.filePath("Eine ziemlich lange Calibre-Bibliothek")}));
        qputenv("CALIBRE_CONFIG_DIRECTORY", calibre.filePath("config").toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("CALIBRE_CONFIG_DIRECTORY"); });
        for (const bool touch : {false, true}) {
            Desk desk("de", touch ? QByteArray() : lightTheme, touch);
            desk.settings->setTour("");
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QTRY_VERIFY(desk.shows("calibreOffer"));
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "empty-offered");
            }
            // The offer answered, the tour's own is what is left.
            QMetaObject::invokeMethod(desk.find("calibreOfferLater"), "clicked");
            QTRY_VERIFY(!desk.shows("calibreOffer"));
            desk.call("startTour", "tour");
            QTRY_VERIFY(desk.window->property("touring").toBool());
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                QTest::qWait(280); // the card moves to its step
                look(desk, "tour");
                QTest::keyClick(desk.window, Qt::Key_Right);
            }
            QTest::keyClick(desk.window, Qt::Key_Escape);
            QTRY_VERIFY(!desk.window->property("touring").toBool());
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "empty");
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }
    // Collections: a row of tabs at the top and one of the collections
    // inside the one opened, with long names in a long language, at every
    // size; the tabs scroll rather than squeeze or spill. And the dialog and
    // the list that make and choose them.
    void collectionsAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            const QList<qint64> ids = stock(desk);
            QVERIFY(!ids.isEmpty());
            Library *library = desk.backend->library();
            for (const QString path : {"Belletristik", "Fachbücher/Regelungstechnik und Kalman-Filter", "Fachbücher/Hydraulik",
                                       "Fachbücher/Bodenmechanik und Baggerei", "Fachbücher/Werkstoffe", "Zum Verleihen an Freunde",
                                       "Sommerlektüre am See"})
                QVERIFY(library->addCollection(path, path == "Belletristik" ? "genre:fiction" : "tag:" + path.section('/', -1).section(' ', 0, 0)));
            // One filled by a space someone shared, marked as theirs.
            const QString shared = QFINDTESTDATA("data/space-1.0.omabook-space");
            QVERIFY(Subscriptions::subscribe(*library, *Subscriptions::fetch(shared), {}).has_value());
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.backend->reload();
            desk.backend->setCollection(library->collectionNamed("Fachbücher")->id);
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "collections");
            }
            // Inside one of them: both rows stay, the inner one marking it.
            desk.backend->setCollection(library->collectionNamed("Fachbücher/Werkstoffe")->id);
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "collections-inside");
                dialog(desk, "collection-dialog", [&] {
                    QMetaObject::invokeMethod(desk.window, "editCollection", Q_ARG(QVariant, desk.backend->collection()));
                });
                dialog(desk, "collection-list", [&] { QTest::keyClick(desk.window, Qt::Key_C); });
                // Publishing the collection shown, its counts in; subscribing to a file dropped, read.
                dialog(desk, "publish", [&] {
                    QVERIFY(desk.run("library.publish"));
                    QTRY_VERIFY(desk.backend->spaces()->publishing().contains("path"));
                });
                dialog(desk, "subscribe", [&] {
                    QVERIFY(desk.backend->spaces()->takeSpaces({QUrl::fromLocalFile(shared)}).isEmpty());
                    QTRY_VERIFY(desk.backend->spaces()->subscribing().contains("space"));
                });
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }

    // The search finding a book by what is known of it (the details say
    // which thing, with a way to the graph and the moment), the hint a
    // question in the search field gets, and the AI's answer, in a long
    // language, at every size, and under fingers. The answer is one kept
    // from before, with long reasons, so no model is asked.
    void searchAndTheAiAtEverySize() {
        for (const bool touch : {false, true}) {
            Desk desk("de", touch ? QByteArray() : lightTheme, touch);
            const QList<qint64> ids = stock(desk);
            QVERIFY(!ids.isEmpty());
            Library *library = desk.backend->library();
            library->setReading(ids.first(), "read", 4, {});
            Graph(*library).upsert({}, "character", "Wilhelmina von Hohenstaufen-Overbeck", {}, {}, GraphPlace{ids.first(), 0, 0.1});
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.backend->reload();
            const QString question = "die Bücher, in denen eine Mentorin stirbt";
            const QString why = "Eine Leuchtturmwärterin verliert ihre Lehrmeisterin im zweiten Winter am Hafen";
            AiSearch::remember(AiSearch::cacheKey(*library, question, 0),
                               {{ids.first(), why, 90}, {ids.value(1), why + " und erzählt davon", 70}});
            BookListModel *books = desk.backend->books();
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                books->setQuery("hohenstaufen");
                QTRY_COMPARE(books->count(), 1);
                look(desk, "search-known");
                books->setQuery("?" + question);
                QTRY_COMPARE(books->count(), books->total());
                look(desk, "search-question");
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                dialog(desk, "ai-search", [&] {
                    QMetaObject::invokeMethod(desk.window, "showAiSearch", Q_ARG(QVariant, "?" + question));
                    QTRY_VERIFY(desk.librarian->findCached());
                });
            }
            books->setQuery({});
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }

    // A book read as pages and as one column that runs on, and a PDF the
    // same, at every size, in a long language and right to left: nothing
    // spills or stands over anything, at a chapter's start and at its end,
    // where the column says what comes next.
    void theLayoutsAtEverySizeInGermanAndRightToLeft() {
        for (const QByteArray language : {QByteArray("de"), QByteArray("ar")}) {
            Desk desk(language, language == "ar" ? lightTheme : QByteArray(), false);
            QString chapter;
            for (int i = 1; i <= 30; ++i)
                chapter += QString("<p>Absatz %1: Die Laternenwärterin ging am Abend die Hafenmauer entlang und zählte die "
                                   "Glocken vom Ufer, bis keine mehr zu zählen waren.</p>").arg(i);
            const QString epub = desk.dir.filePath("kapitel.epub"), pdf = desk.dir.filePath("seiten.pdf");
            QVERIFY(Fixtures::chapteredEpub(epub, "Die Hafenchronik",
                                            {{"Erstes Kapitel: Eine ungewöhnlich lange Überschrift über den Hafen", "<h1>Erstes Kapitel</h1>" + chapter},
                                             {"Zweites Kapitel", "<h1>Zweites Kapitel</h1><p>Kurz.</p>"},
                                             {"Drittes Kapitel", "<h1>Drittes Kapitel</h1>" + chapter}}));
            QVERIFY(Fixtures::outlinedPdf(pdf, "Seiten", 6, {{"Eins", 0}, {"Zwei", 3}}));
            const qint64 book = desk.backend->library()->add(epub).id, paper = desk.backend->library()->add(pdf).id;
            QVERIFY(book > 0 && paper > 0);
            desk.backend->reload();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            for (const qint64 id : {book, paper}) {
                desk.reader->openBook(id);
                QTRY_VERIFY(desk.reader->isOpen());
                QTRY_VERIFY(desk.find("readerView"));
                for (const QString layout : {"pages", "scroll"}) {
                    desk.settings->setOption("layout", layout);
                    for (const QSize &size : sizes()) {
                        QVERIFY(desk.resize(size));
                        desk.reader->goTo(0);
                        look(desk, "layout-" + layout + (id == paper ? "-pdf" : ""));
                        if (id == book) {
                            // The end of the chapter, and the start of the next.
                            desk.reader->goTo(0, {}, true);
                            look(desk, "layout-" + layout + "-end");
                            desk.reader->goTo(1);
                            look(desk, "layout-" + layout + "-next");
                        }
                    }
                }
                desk.settings->setOption("layout", "auto");
                desk.reader->close();
                QTRY_VERIFY(!desk.reader->isOpen());
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        verdict();
    }

    // Omabook is the keyboard's first, and nothing is the keyboard's
    // alone: every command in the commands table, and every command a
    // plugin adds, has a control a mouse can click and a finger can tap,
    // somewhere on the shelf, a book, the page or the graph, or in the list
    // a ⋯ there opens. A control says which commands it is the way to with
    // `runs` (see AppButton.qml); an entry of a list says it of the one it
    // runs. Under fingers each is a fingertip on its smaller side.
    //
    // A new command fails here until it has one. There is no command that
    // is the keyboard's by nature; one that ever is goes in `keyboardOnly`
    // with the reason.
    void everyCommandIsThereForAMouseAndAFinger() {
        static const QHash<QString, QString> keyboardOnly{};
        for (const bool touch : {false, true}) {
            Desk desk("en", {}, touch);
            const QList<qint64> ids = stock(desk);
            QVERIFY(!ids.isEmpty());
            QVERIFY(desk.backend->library()->addCollection("Harbour books", "tag:Fantasy"));
            // A plugin with a command for the shelf and one for the page.
            QDir().mkpath(desk.dir.filePath("plugins/shelver"));
            Fixtures::write(desk.dir.filePath("plugins/shelver/plugin.json"), "{\"name\": \"Shelver\", \"script\": \"main.js\"}");
            Fixtures::write(desk.dir.filePath("plugins/shelver/main.js"),
                            "omabook.commands.register({id: \"shelve\", title: \"Shelve it\", place: \"library\", key: \"Ctrl+Shift+E\", "
                            "run: function () {}});\n"
                            "omabook.commands.register({id: \"note\", title: \"Note the page\", place: \"reader\", run: function () {}});\n");
            desk.backend->scripts()->reload();
            // A thing known of the first book, for its graph.
            Graph(*desk.backend->library()).upsert({}, "character", "Wilhelmina Overbeck", "Keeps the long lighthouse.", {},
                                                    GraphPlace{ids.first(), 0, 0.1}, "Wilhelmina");
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.backend->reload();
            QVERIFY(desk.resize(QSize(1180, 780)));
            desk.quicken();
            const QString mode = touch ? "touch" : "mouse";

            // The largest of the smaller sides of the controls for each command.
            QHash<QString, qreal> reached;
            const std::function<void(QQuickItem *)> collect = [&](QQuickItem *item) {
                if (!item->isVisible() || item->opacity() <= 0.01)
                    return;
                for (const QString &id : item->property("runs").toString().split(' ', Qt::SkipEmptyParts))
                    reached[id] = std::max(reached.value(id), std::min(item->width(), item->height()));
                for (QQuickItem *child : item->childItems())
                    collect(child);
            };
            const auto gather = [&](const QString &state) {
                desk.settle();
                collect(desk.window->contentItem());
                desk.shot("commands-" + mode + "-" + state);
            };
            QPointingDevice *finger = QTest::createTouchDevice();
            const auto press = [&](const QPoint &at, Qt::MouseButton button = Qt::LeftButton) {
                if (touch && button == Qt::LeftButton) {
                    QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
                    QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
                } else {
                    QTest::mouseClick(desk.window, button, Qt::NoModifier, at);
                }
            };
            const auto pressItem = [&](const QString &name) {
                QQuickItem *item = desk.item(name);
                QVERIFY2(item && desk.shows(name), qPrintable(mode + ": " + name + " is not on screen"));
                press(middle(item));
            };
            const auto inMenu = [&](const QString &opener, const QString &state) {
                pressItem(opener);
                QVERIFY2(desk.waitForPopup(), qPrintable(mode + ": " + opener + " opened nothing"));
                QVERIFY(desk.find("commandMenu")->property("opened").toBool());
                gather(state);
                QVERIFY(desk.closePopups());
            };

            // The shelf, a book selected and its details beside it.
            QTRY_VERIFY(desk.shows("details"));
            gather("shelf");
            inMenu("libraryMore", "library-menu");
            if (QTest::currentTestFailed()) return;
            // What can be done with a book: a right click on it (a long
            // press, for a finger: touch_tests), and the details' "More".
            if (!touch) {
                QQuickItem *grid = desk.item("grid");
                QVERIFY(grid);
                press(grid->mapToScene(QPointF(grid->property("cellWidth").toReal() / 2, 120)).toPoint(), Qt::RightButton);
                QVERIFY2(desk.waitForPopup(), "a right click on a book opened nothing");
                QCOMPARE(desk.find("commandMenu")->property("kind").toString(), QString("book"));
                gather("book-menu");
                QVERIFY(desk.closePopups());
            }
            inMenu("bookMore", "book-more");
            if (QTest::currentTestFailed()) return;
            // A book the search found by what is known of it: the details
            // say which thing, with its way to the graph and to the moment.
            BookListModel *books = desk.backend->books();
            books->setQuery("overbeck");
            QTRY_COMPARE(books->count(), 1);
            QTRY_VERIFY(desk.shows("detailsFoundBy"));
            gather("found");
            books->setQuery({});
            desk.backend->setView("stacks");
            QTRY_VERIFY(desk.find("shelfLoader")->property("status").toInt() == 1);
            gather("stacks");
            desk.backend->setView("covers");
            QTRY_VERIFY(desk.find("shelfLoader")->property("status").toInt() == 1);

            // The graph of a book, a thing in it selected.
            desk.call("showGraph", ids.first());
            QTRY_VERIFY(desk.shows("libraryGraph"));
            QObject *browser = desk.find("libraryGraph")->property("browser").value<QObject *>();
            QVERIFY(browser);
            QTRY_VERIFY(!browser->property("rows").toList().isEmpty());
            const QVariantList rows = browser->property("rows").toList();
            QMetaObject::invokeMethod(browser, "select", Q_ARG(qint64, rows.first().toMap().value("id").toLongLong()));
            gather("graph");
            inMenu("graphMore", "graph-menu");
            if (QTest::currentTestFailed()) return;
            QMetaObject::invokeMethod(desk.find("libraryGraph"), "done");

            // The open book: the bar a finger brings up, and the list
            // behind its "More" or the ⋯ in the corner.
            desk.reader->openBook(ids.first());
            QTRY_VERIFY(desk.reader->isOpen());
            QTRY_VERIFY(desk.shows("touchArea"));
            gather("page");
            if (touch) {
                pressItem("menuButton");
                QTRY_VERIFY(desk.shows("readerBar"));
                gather("bar");
                inMenu("barMore", "reader-menu");
            } else {
                QVERIFY(!desk.shows("menuButton"));
                inMenu("readerMore", "reader-menu");
            }
            if (QTest::currentTestFailed()) return;
            desk.reader->close();

            QStringList missing, small;
            QStringList wanted;
            for (const Customization::Command &command : Customization::commands())
                wanted << command.id;
            for (const QVariant &command : desk.custom->pluginCommands())
                wanted << command.toMap().value("id").toString();
            QVERIFY(wanted.contains("library.shelver:shelve") && wanted.contains("reader.shelver:note"));
            for (const QString &id : std::as_const(wanted)) {
                if (keyboardOnly.contains(id))
                    continue;
                if (!reached.contains(id))
                    missing << id;
                else if (touch && reached.value(id) < Survey::fingertip - 0.5)
                    small << QString("%1 (%2 px)").arg(id).arg(reached.value(id));
            }
            QVERIFY2(missing.isEmpty(), qPrintable(mode + ": no control for " + missing.join(", ")));
            QVERIFY2(small.isEmpty(), qPrintable(mode + ": smaller than a fingertip: " + small.join(", ")));
            // And nothing is said to be the way to a command there isn't.
            QStringList unknown;
            for (auto it = reached.cbegin(); it != reached.cend(); ++it)
                if (!wanted.contains(it.key()))
                    unknown << it.key();
            QVERIFY2(unknown.isEmpty(), qPrintable("no such command: " + unknown.join(", ")));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }
};

QTEST_MAIN(LayoutTests)
#include "layout_tests.moc"
