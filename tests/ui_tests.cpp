#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPainter>
#include <QPointingDevice>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QVersionNumber>
#include <QtTest>
#include <atomic>
#include <cmath>
#include <optional>

#include "assistant.h"
#include "backend.h"
#include "customization.h"
#include "fakegithub.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcanvas.h"
#include "graphjobs.h"
#include "graphmodel.h"
#include "formats.h"
#include "interface.h"
#include "librarian.h"
#include "manual.h"
#include "pageformat.h"
#include "reader.h"
#include "readingsite.h"
#include "settings.h"
#include "sitedesk.h"
#include "secretstore.h"
#include "theme.h"
#include "themechoice.h"
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextDocument>
#include "translator.h"

namespace {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

int lines(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().count('\n') : 0;
}

// A hyprctl that answers with response.json beside it and counts its calls.
QString fakeHyprctl(const QString &directory) {
    const QString command = directory + "/hyprctl";
    write(command, "#!/bin/bash\necho >> '" + directory.toUtf8() + "/calls'\ncat '" + directory.toUtf8()
                       + "/response.json'\n");
    QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return command;
}

QJsonObject readCatalog(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The placeholders in a text, sorted: %1 to %99, and the count (%n, %Ln).
QStringList placeholders(const QString &text) {
    static const QRegularExpression placeholder("%L?(?:[1-9][0-9]?|n)");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// Every text the sources hand to qsTr() or tr(), and whether a call passes it
// a count. It walks the code a token at a time, so a string never hides a
// comment or the other way round, C++'s adjacent literals join, and QML's
// single quotes count. A text that isn't a literal can't be in a catalog.
QMap<QString, bool> translatables(const QString &folder, QStringList *problems) {
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QString code = QString::fromUtf8(file.readAll());
        qsizetype i = 0;
        auto literal = [&](QString *text) {
            const QChar quote = code[i++];
            for (; i < code.size() && code[i] != quote; ++i) {
                if (code[i] == '\\' && i + 1 < code.size()) {
                    const QChar escaped = code[++i];
                    const int digits = escaped == 'u' ? 4 : escaped == 'x' ? 2 : 0;
                    bool hex = false;
                    const ushort unit = code.mid(i + 1, digits).toUShort(&hex, 16);
                    if (digits && hex) {
                        *text += QChar(unit);
                        i += digits;
                    } else {
                        *text += escaped == 'n' ? QChar('\n') : escaped == 't' ? QChar('\t') : escaped;
                    }
                } else {
                    *text += code[i];
                }
            }
            ++i;
        };
        auto skipSpace = [&] {
            while (i < code.size() && code[i].isSpace())
                ++i;
        };
        while (i < code.size()) {
            const QChar c = code[i];
            if (code.mid(i, 2) == "//") {
                i = code.indexOf('\n', i);
                if (i < 0)
                    break;
            } else if (code.mid(i, 2) == "/*") {
                i = code.indexOf("*/", i + 2);
                if (i < 0)
                    break;
                i += 2;
            } else if (c == '"' || c == '\'') {
                QString skipped;
                literal(&skipped);
            } else if (c.isLetter() || c == '_') {
                const qsizetype start = i;
                while (i < code.size() && (code[i].isLetterOrNumber() || code[i] == '_'))
                    ++i;
                const QString word = code.mid(start, i - start);
                skipSpace();
                if ((word != "qsTr" && word != "tr") || i >= code.size() || code[i] != '(')
                    continue;
                ++i;
                skipSpace();
                if (i >= code.size() || (code[i] != '"' && code[i] != '\'')) {
                    *problems << info.fileName() + ": not a literal: " + code.mid(start, 40);
                    continue;
                }
                QString text;
                do {
                    literal(&text);
                    skipSpace();
                } while (i < code.size() && code[i] == '"');
                // The arguments after the text, to the call's closing parenthesis.
                int depth = 0, commas = 0;
                while (i < code.size()) {
                    const QChar d = code[i];
                    if (d == '"' || d == '\'') {
                        QString skipped;
                        literal(&skipped);
                        continue;
                    }
                    if (d == '(' || d == '[' || d == '{')
                        ++depth;
                    else if ((d == ')' || d == ']' || d == '}') && depth-- == 0)
                        break;
                    else if (d == ',' && depth == 0)
                        ++commas;
                    ++i;
                }
                found[text] = found.value(text) || commas >= 2;
            } else {
                ++i;
            }
        }
    }
    return found;
}


// A file dialog that answers at once with what the test set.
class FakePicker : public FilePicker {
public:
    QList<QUrl> next;
    QList<Purpose> asked;
    void pick(Purpose purpose, const QString &) override {
        asked << purpose;
        if (!next.isEmpty())
            emit picked(purpose, next);
    }
};

// A model that answers every question with the same words, in two pieces,
// and keeps what it was asked.
struct AiLog {
    QList<AiRequest> requests;
    QString answer = "It is a book made for tests.";
    bool available = true;
};

class FakeAi : public AiProvider {
public:
    explicit FakeAi(AiLog *log) : m_log(log) {}
    QString name() const override { return "Testbot"; }
    void ask(const AiRequest &request) override {
        m_log->requests << request;
        QTimer::singleShot(0, this, [this] {
            emit delta(m_log->answer.left(5));
            emit delta(m_log->answer.mid(5));
            emit finished();
        });
    }
    void cancel() override {}

private:
    AiLog *m_log;
};

// A catalog that knows one book.
class FakeCatalog : public MetadataSource {
public:
    using MetadataSource::MetadataSource;
    QString name() const override { return "Fake Catalog"; }
    QStringList fields() const override { return {"title", "authors"}; }
    void search(const BookMeta &) override {
        QTimer::singleShot(0, this, [this] {
            MetaCandidate candidate;
            candidate.source = name();
            candidate.meta.title = "Catalog Title";
            candidate.meta.authors = QStringList{"Cat Author"};
            emit found({candidate});
        });
    }
};

// The whole app on a temp folder: its own language, theme, settings and
// library, a fake file dialog, AI and catalog, and the real window.
struct Desk {
    QTemporaryDir dir;
    FakePicker picker;
    AiLog ai;
    std::optional<Translator> translator;
    std::optional<Theme> theme;
    std::optional<Settings> settings;
    std::optional<Backend> backend;
    std::optional<Reader> reader;
    std::optional<Assistant> assistant;
    std::optional<AiAgent> aiAgent;
    std::optional<Customization> custom;
    std::optional<Look> look;
    std::optional<Librarian> librarian;
    std::optional<Manual> manual;
    std::optional<QQmlEngine> engine;
    std::optional<Interface> interface;
    QStringList warnings;
    QQuickWindow *window = nullptr;

    explicit Desk(const QString &converter = {}, const QByteArray &language = "en", const QByteArray &colors = {}) {
        // Nothing the app caches lands in the real home.
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        write(dir.filePath("language"), language + "\n");
        if (!colors.isEmpty()) {
            QDir(dir.path()).mkpath("desktop/theme");
            write(dir.filePath("desktop/theme/colors.toml"), colors);
        }
        translator.emplace(dir.filePath("language"));
        QCoreApplication::installTranslator(&*translator);
        theme.emplace(dir.filePath("desktop"));
        custom.emplace(dir.filePath("custom"));
        look.emplace(&*theme, &*custom);
        settings.emplace(dir.filePath("config.json"));
        settings->setLibraryPath(dir.filePath("Library"));
        auto makeAi = [this](QString *why) -> std::unique_ptr<AiProvider> {
            if (!ai.available) {
                if (why)
                    *why = "No AI is set up for this test.";
                return nullptr;
            }
            return std::make_unique<FakeAi>(&ai);
        };
        backend.emplace(&*settings, &picker, nullptr, makeAi,
                        [](QObject *parent) { return QList<MetadataSource *>{new FakeCatalog(parent)}; },
                        dir.filePath("plugins"), "-");
        reader.emplace(&*backend, converter);
        assistant.emplace(&*reader, makeAi);
        // Omarchy's default agent is a file of the test's own, never the user's.
        aiAgent.emplace(&*settings, dir.filePath("omarchy/defaults/agent"));
        librarian.emplace(&*backend, &*settings, &*custom, [this] { return look->colors(); });
        // The tour was answered long ago, unless a test says this is a first start.
        settings->setTour("done");
        settings->setNewsSeen(OMABOOK_VERSION);
        manual.emplace(&*settings, &*custom);
        // What script plugins add joins the interface, as in main.cpp.
        const auto followPlugins = [this] { custom->setPlugins(backend->scripts()->commands(), backend->scripts()->panels()); };
        QObject::connect(backend->scripts(), &ScriptPlugins::changed, &*custom, followPlugins);
        followPlugins();
    }

    ~Desk() {
        interface.reset();
        engine.reset();
        QCoreApplication::removeTranslator(&*translator);
    }

    qint64 add(const QString &file) {
        const qint64 id = backend->library()->add(file).id;
        backend->reload();
        return id;
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
        engine->rootContext()->setContextProperty("touchscreen", false);
        interface.emplace(&*engine, &*custom);
        // A view of the user's that changes loads the window again.
        QObject::connect(&*interface, &Interface::loaded, &*engine, [this] { window = interface->window(); });
        if (!interface->load())
            return false;
        return activate();
    }

    bool activate() {
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

    QVariant value(const QString &name, const char *property) const {
        QObject *object = find(name);
        return object ? object->property(property) : QVariant();
    }

    // With OMABOOK_SHOTS set to a folder, the tests leave pictures of the
    // window there, to look a change over without a desktop.
    void shot(const QString &name) const {
        const QString folder = qEnvironmentVariable("OMABOOK_SHOTS");
        if (folder.isEmpty())
            return;
        QTest::qWait(350); // let popups finish their transitions
        window->grabWindow().save(folder + "/" + name + ".png");
    }

    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { QTest::keyClick(window, key, modifiers); }
    void type(const QString &text) {
        for (const QChar c : text)
            QTest::keyClick(window, c.toLatin1());
    }
};

// A cover for a made-up book: a color, a motif, and its title and author.
QImage madeUpCover(const QString &title, const QString &author, const QColor &color, int motif) {
    QImage cover(400, 600, QImage::Format_RGB32);
    cover.fill(color);
    QPainter painter(&cover);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool dark = color.lightnessF() < 0.55;
    const QColor ink = dark ? QColor(255, 255, 255, 235) : QColor(20, 20, 20, 225);
    painter.setPen(Qt::NoPen);
    painter.setBrush(dark ? color.lighter(135) : color.darker(118));
    if (motif % 3 == 0)
        painter.drawEllipse(QPointF(200, 400), 120, 120);
    else if (motif % 3 == 1)
        painter.drawRect(0, 330, 400, 120);
    else
        painter.drawPolygon(QPolygonF{QPointF(60, 520), QPointF(200, 300), QPointF(340, 520)});
    painter.setPen(ink);
    QFont font = painter.font();
    font.setPixelSize(40);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(36, 50, 328, 230), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, title);
    font.setPixelSize(22);
    font.setBold(false);
    painter.setFont(font);
    painter.drawText(QRect(36, 530, 328, 40), Qt::AlignLeft | Qt::AlignVCenter, author);
    return cover;
}

// A made-up library to look the views over with: books in two series and
// on their own, by a dozen authors, as files of every length, on paper and
// only wished for, read, half read and waiting, with and without covers.
// Nothing in it is a real book. Returns the ids, in the order added.
QList<qint64> stock(Desk &desk) {
    struct Made {
        const char *title, *author, *series;
        double index;
        const char *tag, *color;
    };
    static const QList<Made> books{
        {"Harbour of Glass", "Mira Okafor", "The Salt Roads", 1, "Fantasy", "#1f4e5f"},
        {"The Lantern Tide", "Mira Okafor", "The Salt Roads", 2, "Fantasy", "#27667b"},
        {"A Map of Low Water", "Mira Okafor", "The Salt Roads", 3, "Fantasy", "#3a8a9e"},
        {"The Brass Seedling", "Tobias Lindqvist", "Clockwork Orchard", 1, "Fantasy", "#8a5a1c"},
        {"Winter Gears", "Tobias Lindqvist", "Clockwork Orchard", 2, "Fantasy", "#a8742a"},
        {"Notes on Slow Rivers", "Edda Marlowe", "", 0, "Essays", "#e8dcc0"},
        {"The Quiet Cartographer", "Idris Vance", "", 0, "Mystery", "#2c2c3a"},
        {"Small Hours", "June Abernathy", "", 0, "Essays", "#b23a48"},
        {"Field Guide to Imaginary Birds", "P. L. Okonkwo", "", 0, "Nature", "#5b8c5a"},
        {"The Last Lighthouse Keeper", "Soren Hale", "", 0, "Mystery", "#f2b134"},
        {"Bread, Salt and Thunder", "Anneke de Vries", "", 0, "History", "#7a3b2e"},
        {"An Atlas of Forgotten Stations", "Idris Vance", "", 0, "History", "#404e7c"},
        {"Thirteen Ways to Fold a Letter", "Hana Sato", "", 0, "Essays", "#d98c5f"},
        {"The Beekeeper's Algebra", "Lucia Ferrand", "", 0, "Nature", "#e3b505"},
        {"Moss", "Edda Marlowe", "", 0, "Nature", "#3d5a3c"},
        {"How to Read a Tide Table", "Mira Okafor", "", 0, "Nature", "#9fc5d6"},
        {"The Understory", "Kofi Brandt", "", 0, "Nature", "#2f4b26"},
        {"Paper Moons", "June Abernathy", "", 0, "Fantasy", "#51355a"},
        {"Granite", "Soren Hale", "", 0, "Mystery", "#6b6f73"},
        {"The Night Ferry", "Hana Sato", "", 0, "Mystery", "#14213d"},
        {"Seven Kinds of Rain", "Lucia Ferrand", "", 0, "Essays", "#5c80bc"},
        {"Letters from the Signal Box", "Kofi Brandt", "", 0, "History", "#933a16"},
        {"The Glassblower's Apprentice", "Anneke de Vries", "", 0, "History", "#c8553d"},
        {"Ember and Ash", "Tobias Lindqvist", "", 0, "Fantasy", "#a4243b"},
        {"A Short History of Fog", "Edda Marlowe", "", 0, "History", "#c9cdd2"},
        {"The Orchard at the End of the Line", "Idris Vance", "", 0, "Fantasy", "#6a994e"},
        {"Tin Stars", "P. L. Okonkwo", "", 0, "Mystery", "#22333b"},
        {"What the Heron Knew", "Kofi Brandt", "", 0, "Nature", "#577590"},
    };
    Library *library = desk.backend->library();
    QRandomGenerator sizes(7);
    QList<qint64> ids;
    for (int i = 0; i < books.size(); ++i) {
        const Made &made = books.at(i);
        BookMeta meta;
        meta.title = made.title;
        meta.authors = QStringList{made.author};
        meta.series = made.series;
        meta.seriesIndex = made.index;
        meta.tags = QStringList{made.tag};
        meta.language = i % 6 == 4 ? "nl" : "en";
        meta.published = QString::number(1950 + (i * 37) % 70);
        // One in seven is on paper only, one in eleven is only wished for,
        // and the rest are files, a few of those on paper as well.
        const bool wish = i % 11 == 5, paper = i % 7 == 3 || i % 13 == 6;
        qint64 id = 0;
        if (wish || i % 7 == 3) {
            id = library->addEntry(meta, !wish).id;
        } else {
            // A file as long as a book is: a pamphlet to a doorstop.
            QByteArray pages(int(60000 * std::pow(40.0, sizes.generateDouble())), Qt::Uninitialized);
            for (char &byte : pages)
                byte = char(sizes.generate());
            Fixtures::Entries entries = Fixtures::epubEntries(made.title);
            entries.append({"OEBPS/pages.bin", pages});
            const QString file = desk.dir.filePath(QString("made-%1.epub").arg(i));
            if (!Fixtures::zip(file, entries))
                return {};
            id = library->add(file, meta).id;
            QFile::remove(file);
            if (paper)
                library->setPaper(id, true);
        }
        if (id == 0)
            return {};
        // One in nine has no cover, so the plain tile is looked over too.
        if (i % 9 != 8)
            library->setCover(id, madeUpCover(made.title, made.author, QColor(made.color), i));
        if (i % 4 == 0)
            library->setReading(id, "read", 3 + i % 3, QString("2026-%1-12").arg(1 + i % 9, 2, 10, QChar('0')));
        else if (i % 4 == 1 && !wish && i % 7 != 3)
            library->setProgress(id, {0, 0.5, double(12 + (i * 17) % 70), 1700000000 + i * 86400});
        else if (i % 5 == 2 || wish)
            library->setReading(id, "to-read", 0, {});
        ids << id;
    }
    desk.backend->reload();
    return ids;
}

} // namespace

// The window itself: the real Main.qml on the real backend, driven with the
// keyboard, and failing on any warning QML reports. Also the pieces every
// Omarchy app shares: the theme, the translator and the catalogs.
class UiTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void themeFallsBack() {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
        QCOMPARE(theme.radius(), 0);
    }

    void themeReadsThePalette() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("theme"));
        QVERIFY(write(dir.path() + "/theme/colors.toml", "mode = \"light\"\n"
                                                         "theme_type = \"dark\"\n"
                                                         "accent = \"#112233\"\n"
                                                         "bg = #fafafa # paper\n"
                                                         "foreground = '#202020'\n"
                                                         "color1 = \"#aa0000\"\n"));

        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#fafafa"));
        QCOMPARE(theme.foreground(), QColor("#202020"));
        QCOMPARE(theme.colors().value("red").value<QColor>(), QColor("#aa0000"));
        QVERIFY(theme.light());
    }

    void themeAsksHyprlandAgainOnlyWhenItReloads() {
        // The fake Hyprland lives outside the theme directory, whose changes
        // the Theme also follows.
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));

        // Once on start, and once more when the event socket connects.
        Theme theme(dir.path(), nullptr, command, events.fullServerName());
        QTRY_VERIFY(events.hasPendingConnections());
        QLocalSocket *hyprland = events.nextPendingConnection();
        QTRY_COMPARE(lines(hypr.filePath("calls")), 2);
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":10}"));
        QTest::qWait(300);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        // Events arrive in pieces; only a whole configreloaded line counts.
        hyprland->write("activewindow>>kitty,~\nconfigrel");
        hyprland->flush();
        QTest::qWait(100);
        QCOMPARE(lines(hypr.filePath("calls")), 2);
        hyprland->write("oaded>>\n");
        QTRY_COMPARE(theme.radius(), 10);
        QCOMPARE(lines(hypr.filePath("calls")), 3);
    }

    void themeReachesHyprlandOnceItListens() {
        QTemporaryDir dir, hypr;
        const QString command = fakeHyprctl(hypr.path());
        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":4}"));
        Theme theme(dir.path(), nullptr, command, hypr.filePath("events.sock"));
        QTRY_COMPARE(theme.radius(), 4);

        QVERIFY(write(hypr.filePath("response.json"), "{\"int\":8}"));
        QLocalServer events;
        QVERIFY(events.listen(hypr.filePath("events.sock")));
        QTRY_VERIFY_WITH_TIMEOUT(events.hasPendingConnections(), 5000);
        QTRY_COMPARE(theme.radius(), 8);
    }

    void translatorFallsBackToEnglish() {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Save").isNull());
    }

    void translatorFollowsTheLanguageFile() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "pt-BR\n"));
        QVERIFY(write(dir.path() + "/pt.json", R"({"Save": "Guardar", "Open": "Abrir"})"));
        QVERIFY(write(dir.path() + "/pt-BR.json", R"({"Save": "Salvar"})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("pt-BR"));
        QCOMPARE(translator.translate("Main", "Save"), QString("Salvar"));
        QCOMPARE(translator.translate("Main", "Open"), QString("Abrir"));
        QVERIFY(translator.translate("Main", "Quit").isNull());
    }

    void translatorSeesAFirstPick() {
        // Before the first pick there is no ~/.config/omarchy to watch.
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/da.json", R"({"Save": "Gem"})"));
        Translator translator(dir.path() + "/omarchy/language", dir.path());
        QVERIFY(QDir(dir.path()).mkpath("omarchy"));
        QVERIFY(write(dir.path() + "/omarchy/language", "da\n"));
        QTRY_COMPARE(translator.translate("Main", "Save"), QString("Gem"));
    }

    void translatorPicksPluralForms() {
        QTemporaryDir dir;
        QVERIFY(write(dir.path() + "/language", "ru\n"));
        QVERIFY(write(dir.path() + "/en.json", R"({"%n clips": {"one": "%n clip", "other": "%n clips"},
                                                   "%n files": {"one": "%n file", "other": "%n files"}})"));
        QVERIFY(write(dir.path() + "/ru.json", R"({"%n clips": {"one": "%n клип", "few": "%n клипа", "many": "%n клипов", "other": "%n клипа"}})"));

        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 21), QString("%n клип"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 22), QString("%n клипа"));
        QCOMPARE(translator.translate("Main", "%n clips", nullptr, 25), QString("%n клипов"));
        // An untranslated counted string falls back to English, with English rules.
        QCOMPARE(translator.translate("Main", "%n files", nullptr, 21), QString("%n files"));
    }

    void englishCatalogListsEveryString() {
        // en.json is what translators work from: every text the sources
        // translate is in it, as itself or, when counted, as its English
        // plural forms, and nothing else is.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QStringList problems;
        const QMap<QString, bool> used = translatables(sources, &problems);
        const QJsonObject english = readCatalog(sources + "/i18n/en.json");
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne() {
        // A catalog may lack a string (it shows in English meanwhile), but what
        // it has translates something in en.json, with the same placeholders,
        // and a counted string has every form its language uses.
        const QString folder = QFileInfo(QFINDTESTDATA("../src/i18n/en.json")).absolutePath();
        const QJsonObject english = readCatalog(folder + "/en.json");
        QStringList problems;
        for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
            const QString code = file.completeBaseName();
            const QJsonObject catalog = readCatalog(file.filePath());
            if (catalog.isEmpty())
                problems << code + ": not a JSON object, or empty";
            // Some languages reach a form only at a million ("many" in French).
            QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
            for (long long n = 0; n <= 1000; ++n)
                categories << Translator::pluralCategory(code, n);
            for (auto it = catalog.begin(); it != catalog.end(); ++it) {
                const QString where = code + ": " + it.key() + ": ";
                const QJsonValue source = english.value(it.key());
                if (source.isUndefined()) {
                    problems << where + "not in en.json";
                } else if (source.isString()) {
                    if (!it.value().isString() || it.value().toString().isEmpty())
                        problems << where + "needs a translation";
                    else if (placeholders(it.value().toString()) != placeholders(it.key()))
                        problems << where + "has other placeholders than the English";
                } else {
                    const QJsonObject forms = it.value().toObject();
                    const QStringList wanted = placeholders(source.toObject().value("other").toString());
                    for (const QString &category : categories)
                        if (forms.value(category).toString().isEmpty())
                            problems << where + "needs its " + category + " form";
                    // A form other than "other" may say its number in words.
                    for (auto form = forms.begin(); form != forms.end(); ++form) {
                        const QStringList has = placeholders(form.value().toString());
                        QStringList needed = wanted;
                        if (form.key() != "other")
                            needed.removeIf([&](const QString &p) { return p.endsWith('n') && !has.contains(p); });
                        if (has != needed)
                            problems << where + form.key() + " has other placeholders than the English";
                    }
                }
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }


    void theShelfShowsBooksAndAnswersTheKeyboard() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        QVERIFY(Fixtures::epub(desk.dir.filePath("two.epub"), "Second Story"));
        QVERIFY(desk.show());
        QVERIFY(desk.value("emptyState", "visible").toBool());

        // A adds books through the file dialog, off the GUI thread.
        desk.picker.next = {QUrl::fromLocalFile(desk.dir.filePath("one.epub")), QUrl::fromLocalFile(desk.dir.filePath("two.epub"))};
        desk.key(Qt::Key_A);
        QCOMPARE(desk.picker.asked, QList<FilePicker::Purpose>{FilePicker::AddBooks});
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 2);
        QCOMPARE(desk.value("bookCount", "text").toString(), QString("2 books"));
        QCOMPARE(desk.value("toastText", "text").toString(), QString("2 books added"));
        QVERIFY(!desk.value("emptyState", "visible").toBool());

        // The newest is first and selected; H and L walk the shelf.
        QTRY_COMPARE(desk.value("detailsTitle", "text").toString(), QString("Second Story"));
        desk.key(Qt::Key_L);
        QCOMPARE(desk.value("detailsTitle", "text").toString(), QString("The Test Book"));
        QCOMPARE(desk.value("detailsAuthors", "text").toString(), QString("Ann Author & Bo Writer"));
        desk.shot("shelf");
        desk.key(Qt::Key_H);
        QCOMPARE(desk.value("detailsTitle", "text").toString(), QString("Second Story"));

        // Adding them again adds nothing, and says so.
        desk.key(Qt::Key_A);
        QTRY_COMPARE(desk.value("toastText", "text").toString(), QString("2 were already in the library"));

        // "/" searches; while typing, letters are letters, not shortcuts.
        desk.key(Qt::Key_Slash);
        QTRY_VERIFY(desk.value("search", "activeFocus").toBool());
        desk.type("second q");
        QCOMPARE(desk.value("grid", "count").toInt(), 0);
        desk.key(Qt::Key_Backspace);
        desk.key(Qt::Key_Backspace);
        QCOMPARE(desk.value("grid", "count").toInt(), 1);
        QCOMPARE(desk.value("bookCount", "text").toString(), QString("1 of 2"));
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.value("grid", "count").toInt(), 2);
        QVERIFY(desk.value("grid", "activeFocus").toBool());

        desk.key(Qt::Key_S);
        QCOMPARE(desk.value("sortName", "text").toString(), QString("Title"));
        QCOMPARE(desk.settings->sort(), QString("title"));

        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        desk.shot("help");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("help", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aBookAddedElsewhereShowsUp() {
        // The command line, or an agent, adds to the library the window has open.
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        QVERIFY(desk.show());
        Library elsewhere(desk.dir.filePath("Library"));
        QVERIFY(elsewhere.open());
        QVERIFY(elsewhere.add(desk.dir.filePath("one.epub")).error.isEmpty());
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 1);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void detailsAreEditedByHand() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        desk.key(Qt::Key_E);
        QTRY_VERIFY(desk.value("edit", "opened").toBool());
        QTRY_VERIFY(desk.value("editTitle", "activeFocus").toBool());
        QCOMPARE(desk.value("editTitle", "text").toString(), QString("The Test Book"));
        desk.shot("edit");
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.type("Renamed");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("edit", "opened").toBool());
        QTRY_COMPARE(desk.value("detailsTitle", "text").toString(), QString("Renamed"));
        QCOMPARE(desk.backend->library()->book(id)->meta.title, QString("Renamed"));
        // What wasn't touched is as it was.
        QCOMPARE(desk.backend->library()->book(id)->meta.series, QString("The Fixtures"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aBookIsRemovedOnlyAfterAsking() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        desk.key(Qt::Key_D);
        QTRY_VERIFY(desk.value("removal", "opened").toBool());
        // Dismissing the question keeps the book.
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("removal", "visible").toBool());
        QCOMPARE(desk.value("grid", "count").toInt(), 1);

        desk.key(Qt::Key_D);
        QTRY_VERIFY(desk.value("removal", "opened").toBool());
        QTRY_VERIFY(desk.value("confirmRemove", "activeFocus").toBool());
        desk.shot("remove");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 0);
        QVERIFY(desk.backend->library()->books().isEmpty());
        QVERIFY(QFile::exists(desk.dir.filePath("one.epub")));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aFlowingBookIsRead() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QTRY_VERIFY(desk.value("grid", "activeFocus").toBool());
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->kind(), QString("flow"));
        QTRY_VERIFY(desk.find("flowText"));
        // The page shows the chapter the reader holds, laid out as rich text.
        QVERIFY(desk.reader->html().contains("Chapter One"));
        QVERIFY(desk.value("flowText", "text").toString().contains("Centered words."));
        QCOMPARE(desk.value("readerTitle", "text").toString(), QString("The Test Book · One"));
        desk.shot("reader");

        // Paging down reaches the chapter's end and then turns to the next
        // one; a page up from its top comes back.
        for (int press = 0; press < 4 && desk.reader->section() == 0; ++press)
            desk.key(Qt::Key_Space);
        QCOMPARE(desk.reader->section(), 1);
        QCOMPARE(desk.reader->fraction(), 0.0);
        QVERIFY(desk.value("flowText", "text").toString().contains("The end."));
        desk.key(Qt::Key_Space, Qt::ShiftModifier);
        QTRY_COMPARE(desk.reader->section(), 0);
        // It comes back to the end of the chapter, not its start.
        QTRY_COMPARE(desk.reader->fraction(), 1.0);
        // A link to the other chapter's footnote.
        QVERIFY(desk.reader->follow("omabook:1#note"));
        QCOMPARE(desk.reader->section(), 1);
        QVERIFY(!desk.reader->follow("https://example.com/"));
        desk.key(Qt::Key_H);
        QCOMPARE(desk.reader->section(), 0);

        // The index opens beside the page, is walked with the arrow keys,
        // and stays open after going somewhere.
        desk.key(Qt::Key_T);
        QTRY_VERIFY(desk.value("contents", "visible").toBool());
        QCOMPARE(desk.value("contentsList", "count").toInt(), 3);
        desk.shot("contents");
        QTRY_VERIFY(desk.value("contentsList", "activeFocus").toBool());
        desk.key(Qt::Key_Down);
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.reader->section(), 1);
        QVERIFY(desk.value("contents", "visible").toBool());
        // A chapter with parts folds: "One" holds "A note".
        desk.key(Qt::Key_Up);
        desk.key(Qt::Key_Left);
        QCOMPARE(desk.value("contentsList", "count").toInt(), 2);
        desk.key(Qt::Key_Right);
        QCOMPARE(desk.value("contentsList", "count").toInt(), 3);
        desk.key(Qt::Key_T);
        QTRY_VERIFY(!desk.value("contents", "visible").toBool());

        // The book and nothing else, and back.
        desk.key(Qt::Key_F);
        QVERIFY(desk.find("readerView")->property("immersive").toBool());
        QVERIFY(!desk.value("readerTitle", "visible").toBool());
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.find("readerView")->property("immersive").toBool());
        QVERIFY(desk.reader->isOpen());

        // Finding words anywhere in the book.
        desk.key(Qt::Key_Slash);
        QTRY_VERIFY(desk.value("finder", "opened").toBool());
        QTRY_VERIFY(desk.value("findWords", "activeFocus").toBool());
        desk.type("note");
        desk.key(Qt::Key_Return);
        QCOMPARE(desk.value("findCount", "text").toString(), QString("2 places"));
        desk.shot("find");
        QTRY_VERIFY(desk.value("findList", "activeFocus").toBool());
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("finder", "visible").toBool());
        QCOMPARE(desk.reader->section(), 0);

        desk.key(Qt::Key_B);
        QCOMPARE(desk.reader->bookmarks().size(), 1);
        QCOMPARE(desk.value("toastText", "text").toString(), QString("Bookmarked"));
        desk.key(Qt::Key_Plus);
        QCOMPARE(desk.settings->fontSize(), 20);

        // Escape goes back to the shelf, and the place is remembered.
        desk.key(Qt::Key_L);
        QCOMPARE(desk.reader->section(), 1);
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.reader->isOpen());
        QCOMPARE(desk.backend->library()->progress(id).section, 1);
        QTRY_VERIFY(desk.value("grid", "activeFocus").toBool());
        desk.key(Qt::Key_Space);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->section(), 1);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aPagedBookIsRead() {
        Desk desk;
        QVERIFY(Fixtures::pdf(desk.dir.filePath("paper.pdf"), "A Paper", {"First page", "Second page", "Third page"}));
        const qint64 id = desk.add(desk.dir.filePath("paper.pdf"));
        QVERIFY(desk.show());
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->kind(), QString("pages"));
        // A wide window shows the pages two by two.
        QTRY_COMPARE(desk.value("pageReader", "pageCount").toInt(), 3);
        QVERIFY(desk.value("pageReader", "twoUp").toBool());
        QCOMPARE(desk.value("pageReader", "count").toInt(), 2);
        QCOMPARE(desk.value("readerPlace", "text").toString(), QString("1 of 3"));
        desk.shot("pages");

        desk.key(Qt::Key_End);
        QTRY_COMPARE(desk.reader->section(), 2);
        QCOMPARE(desk.value("readerPlace", "text").toString(), QString("3 of 3"));
        QCOMPARE(desk.reader->percent(), 100.0);
        desk.key(Qt::Key_Home);
        QTRY_COMPARE(desk.reader->section(), 0);
        desk.key(Qt::Key_Plus);
        QVERIFY(desk.value("pageReader", "zoomFactor").toDouble() > 1);
        // The page itself is drawn by the image provider.
        QSize size;
        const QImage page = desk.reader->imageProvider()->requestImage(
            desk.reader->imagePrefix().section('/', 3, 3) + "/page/0/400", &size, {});
        QCOMPARE(page.width(), 400);
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.reader->isOpen());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theCompanionAnswersFromWhatHasBeenRead() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());

        desk.key(Qt::Key_A);
        QTRY_VERIFY(desk.value("assistantPane", "visible").toBool());
        QTRY_VERIFY(desk.value("question", "activeFocus").toBool());
        // A question with a Q in it: typing never quits.
        desk.type("Who is quiet?");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.ai.requests.size(), 1);
        const AiRequest first = desk.ai.requests.first();
        QVERIFY(first.system.contains("\"The Test Book\" by Ann Author & Bo Writer"));
        QCOMPARE(first.messages.size(), 1);
        // The chapter in front of the reader goes along, and no later one.
        QVERIFY(first.messages.first().text.contains("<book_excerpt>"));
        QVERIFY(first.messages.first().text.contains("Chapter One"));
        QVERIFY(!first.messages.first().text.contains("Chapter Two"));
        QVERIFY(first.messages.first().text.endsWith("Who is quiet?"));
        QTRY_COMPARE(desk.value("conversation", "count").toInt(), 2);
        QCOMPARE(desk.assistant->messages().last().toMap().value("text").toString(), desk.ai.answer);
        QVERIFY(!desk.assistant->busy());

        // "Catch me up" in the second chapter: the first time, the book so
        // far is read, and the answer is kept as the model's notes.
        desk.reader->goTo(1);
        QMetaObject::invokeMethod(desk.find("catchUp"), "clicked");
        QTRY_COMPARE(desk.ai.requests.size(), 2);
        const AiRequest second = desk.ai.requests.last();
        QCOMPARE(second.messages.size(), 1);
        QVERIFY(second.messages.first().text.contains("Chapter One"));
        QVERIFY(second.messages.first().text.contains("has read so far"));
        QVERIFY(!second.messages.first().text.contains("<notes>"));
        QTRY_COMPARE(desk.value("conversation", "count").toInt(), 4);
        QTRY_VERIFY(!desk.assistant->busy());
        QCOMPARE(desk.backend->library()->recap(id).text, desk.ai.answer);
        QCOMPARE(desk.backend->library()->recap(id).section, 1);

        // Asked again with nothing read since, it costs nothing: the notes
        // are shown as they are.
        QMetaObject::invokeMethod(desk.find("catchUp"), "clicked");
        QCOMPARE(desk.ai.requests.size(), 2);
        QCOMPARE(desk.value("conversation", "count").toInt(), 6);
        QVERIFY(desk.value("assistantNote", "text").toString().contains("nothing was sent"));

        // "Who is who" is kept the same way, as notes of its own.
        QMetaObject::invokeMethod(desk.find("whoIsWho"), "clicked");
        QTRY_COMPARE(desk.ai.requests.size(), 3);
        QVERIFY(desk.ai.requests.last().messages.first().text.contains("List who is who so far"));
        QTRY_VERIFY(!desk.assistant->busy());
        QCOMPARE(desk.backend->library()->notes(id, "cast").text, desk.ai.answer);
        QCOMPARE(desk.backend->library()->notes(id, "recap").text, desk.ai.answer);

        // An earlier book of the same series: what was noted there goes
        // along the first time, and not again once this book has its notes.
        QVERIFY(Fixtures::epub(desk.dir.filePath("earlier.epub"), "The First Fixture"));
        const qint64 earlier = desk.add(desk.dir.filePath("earlier.epub"));
        BookMeta meta = desk.backend->library()->book(earlier)->meta;
        meta.series = "the fixtures";
        meta.seriesIndex = 1;
        QVERIFY(desk.backend->library()->setMeta(earlier, meta));
        desk.backend->library()->setRecap(earlier, {0, 1, "Ann built the first fixture.", 0});
        desk.backend->library()->setNotes(id, "cast", {});
        QMetaObject::invokeMethod(desk.find("whoIsWho"), "clicked");
        QTRY_COMPARE(desk.ai.requests.size(), 4);
        QVERIFY(desk.ai.requests.last().messages.first().text.contains(
            "<book title=\"The First Fixture\">\nAnn built the first fixture.\n</book>"));
        QVERIFY(desk.value("assistantNote", "text").toString().contains("The First Fixture"));
        QTRY_VERIFY(!desk.assistant->busy());
        QCOMPARE(desk.backend->library()->seriesNotes(earlier, "cast").size(), 0);

        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("assistantPane", "visible").toBool());
        QVERIFY(desk.reader->isOpen());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void withoutAnAiThePaneSaysWhatIsMissing() {
        Desk desk;
        desk.ai.available = false;
        desk.assistant->refresh();
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.key(Qt::Key_A);
        QTRY_VERIFY(desk.value("assistantPane", "visible").toBool());
        QVERIFY(desk.value("assistantProblem", "visible").toBool());
        QCOMPARE(desk.value("assistantProblem", "text").toString(), QString("No AI is set up for this test."));
        desk.shot("assistant-none");
        QVERIFY(!desk.value("question", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void detailsAreLookedUpAndTaken() {
        Desk desk;
        desk.ai.answer = "{\"title\": \"AI Title\", \"authors\": [\"Ai Author\"], \"tags\": [\"tested\"], "
                         "\"why\": \"The opening pages match.\"}";
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        desk.key(Qt::Key_M);
        QTRY_VERIFY(desk.value("lookup", "opened").toBool());
        // The catalog answers first; then the AI is asked, with the opening
        // pages and what the catalog found, and its judgement leads the list.
        QTRY_COMPARE(desk.value("lookupList", "count").toInt(), 2);
        QCOMPARE(desk.ai.requests.size(), 1);
        const QString asked = desk.ai.requests.first().messages.first().text;
        QVERIFY(asked.contains("Chapter One"));
        QVERIFY(asked.contains("The Test Book - Ann Author.epub"));
        QVERIFY2(asked.contains("<catalog_results>") && asked.contains("Catalog Title"), qPrintable(asked.right(400)));
        QVERIFY(!desk.backend->searching());

        // Every source says how it answered, and every result what it would do.
        const QVariantList sources = desk.backend->lookUpSources();
        QCOMPARE(sources.size(), 2);
        QCOMPARE(sources.at(0).toMap().value("name").toString(), QString("Fake Catalog"));
        QCOMPARE(sources.at(0).toMap().value("found").toInt(), 1);
        QVERIFY(!sources.at(1).toMap().value("asking").toBool());
        const QVariantMap recommended = desk.backend->candidates().at(0).toMap();
        QCOMPARE(recommended.value("source").toString(), QString("Testbot"));
        QCOMPARE(recommended.value("why").toString(), QString("The opening pages match."));
        // It changes the title and authors and tags, and adds nothing new.
        QStringList changed;
        for (const QVariant &change : recommended.value("changes").toList())
            changed << change.toMap().value("field").toString();
        QCOMPARE(changed, (QStringList{"title", "authors", "tags"}));
        QCOMPARE(recommended.value("changes").toList().first().toMap().value("from").toString(), QString("The Test Book"));
        QVERIFY(recommended.value("adds").toStringList().isEmpty());
        QCOMPARE(desk.backend->candidates().at(1).toMap().value("source").toString(), QString("Fake Catalog"));
        desk.shot("lookup");

        QTRY_VERIFY(desk.value("lookupList", "activeFocus").toBool());
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("lookup", "opened").toBool());
        const auto book = desk.backend->library()->book(id);
        QCOMPARE(book->meta.title, QString("AI Title"));
        QCOMPARE(book->meta.authors, QStringList{"Ai Author"});
        QCOMPARE(book->meta.tags, QStringList{"tested"});
        // What the candidate didn't say is kept.
        QCOMPARE(book->meta.publisher, QString("Fixture Press"));
        QTRY_COMPARE(desk.value("detailsTitle", "text").toString(), QString("AI Title"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aKindleBookIsConvertedOnceThenRead() {
        QTemporaryDir tools;
        QVERIFY(Fixtures::epub(tools.filePath("converted.epub")));
        write(tools.filePath("convert"), "#!/bin/bash\ncp '" + tools.filePath("converted.epub").toUtf8() + "' \"$2\"\n");
        QFile::setPermissions(tools.filePath("convert"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

        Desk desk(tools.filePath("convert"));
        QVERIFY(Fixtures::write(desk.dir.filePath("book.azw3"), Fixtures::mobi()));
        const qint64 id = desk.add(desk.dir.filePath("book.azw3"));
        QVERIFY(desk.show());
        QCOMPARE(desk.value("detailsTitle", "text").toString(), QString("Kindle Fixture"));
        desk.reader->openBook(id);
        QVERIFY(desk.reader->loading());
        QTRY_VERIFY_WITH_TIMEOUT(desk.reader->isOpen(), 10000);
        QCOMPARE(desk.reader->kind(), QString("flow"));
        // The EPUB joined the book; the Kindle file is still there, untouched.
        QCOMPARE(desk.backend->library()->book(id)->formats, (QStringList{"epub", "azw3"}));
        QVERIFY(QFile::exists(desk.backend->library()->formatPath(id, "azw3")));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aBookThatCannotBeOpenedSaysWhy() {
        Desk desk; // with no converter installed
        QVERIFY(Fixtures::write(desk.dir.filePath("book.azw3"), Fixtures::mobi()));
        const qint64 id = desk.add(desk.dir.filePath("book.azw3"));
        QVERIFY(desk.show());
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.find("readerError"));
        QTRY_VERIFY2(desk.value("readerError", "text").toString().contains("ebook-convert"),
                     qPrintable(desk.value("readerError", "text").toString()));
        desk.shot("error");
        QVERIFY(!desk.reader->isOpen());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(desk.value("grid", "visible").toBool());
        QCOMPARE(desk.reader->error(), QString());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void fingersBrowseAndRead() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QPointingDevice *finger = QTest::createTouchDevice();
        auto tap = [&](const QPointF &at) {
            QTest::touchEvent(desk.window, finger).press(0, at.toPoint(), desk.window);
            QTest::touchEvent(desk.window, finger).release(0, at.toPoint(), desk.window);
        };
        auto swipe = [&](const QPointF &from, const QPointF &to) {
            QTest::touchEvent(desk.window, finger).press(0, from.toPoint(), desk.window);
            // A finger moves over several frames; moves sent all at once are
            // merged into one.
            for (int step = 1; step <= 6; ++step) {
                QTest::touchEvent(desk.window, finger).move(0, (from + (to - from) * step / 6.0).toPoint(), desk.window);
                QTest::qWait(20);
            }
            QTest::touchEvent(desk.window, finger).release(0, to.toPoint(), desk.window);
        };

        // With no touchscreen the keys have no buttons, until a finger lands.
        QVERIFY(!desk.value("touchBar", "visible").toBool());
        auto *grid = qobject_cast<QQuickItem *>(desk.find("grid"));
        QVERIFY(grid);
        // One tap on a cover opens the book.
        tap(grid->mapToScene(QPointF(grid->property("cellWidth").toReal() / 2, 120)));
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->bookId(), id);
        QTRY_VERIFY(desk.find("flowText"));

        auto *page = qobject_cast<QQuickItem *>(desk.find("touchArea"));
        QVERIFY(page);
        const QPointF right = page->mapToScene(QPointF(page->width() * 0.9, page->height() / 2));
        const QPointF left = page->mapToScene(QPointF(page->width() * 0.1, page->height() / 2));
        const QPointF middle = page->mapToScene(QPointF(page->width() * 0.5, page->height() / 2));

        // A page turn slides the page away and the next one in.
        auto turned = [&] {
            QTRY_VERIFY(!desk.find("readerView")->property("turningPage").toBool());
            QCOMPARE(desk.find("readerView")->property("slide").toReal(), 0.0);
        };
        // Taps on the right edge turn pages, on into the next chapter; one on
        // the left edge turns back.
        for (int press = 0; press < 4 && desk.reader->section() == 0; ++press) {
            tap(right);
            QTest::qWait(60);
            turned();
        }
        QCOMPARE(desk.reader->section(), 1);
        tap(left);
        QTest::qWait(60);
        turned();
        QCOMPARE(desk.reader->section(), 0);
        // It turned back to the end of the first chapter, as a page would.
        QCOMPARE(desk.reader->fraction(), 1.0);

        // A swipe leftwards turns the page: the page follows the finger, and
        // from the chapter's last page that is the next chapter.
        swipe(right, left);
        turned();
        QCOMPARE(desk.reader->section(), 1);
        QCOMPARE(desk.reader->fraction(), 0.0);
        // A swipe rightwards turns it back.
        swipe(left, right);
        turned();
        QCOMPARE(desk.reader->section(), 0);
        // A short drag is not a turn: the page settles where it was.
        swipe(middle, middle + QPointF(-40, 0));
        turned();
        QCOMPARE(desk.reader->section(), 0);

        // For fingers there is one button that is always there; it shows
        // everything else, the contents among it.
        QVERIFY(desk.value("menuButton", "visible").toBool());
        QMetaObject::invokeMethod(desk.find("menuButton"), "clicked");
        QTRY_VERIFY(desk.value("readerBar", "visible").toBool());
        QMetaObject::invokeMethod(desk.find("menuButton"), "clicked");
        QTRY_VERIFY(!desk.value("readerBar", "visible").toBool());

        // A tap in the middle shows it too.
        QVERIFY(!desk.value("readerBar", "visible").toBool());
        tap(middle);
        QTRY_VERIFY(desk.value("readerBar", "visible").toBool());
        desk.shot("touch-reader");
        QMetaObject::invokeMethod(desk.find("barLarger"), "clicked");
        QCOMPARE(desk.settings->fontSize(), 20);
        tap(middle);
        QTRY_VERIFY(!desk.value("readerBar", "visible").toBool());
        tap(middle);
        QMetaObject::invokeMethod(desk.find("barBack"), "clicked");
        QTRY_VERIFY(!desk.reader->isOpen());

        // Now that it has been touched, the shelf has its buttons too.
        QVERIFY(desk.value("touchBar", "visible").toBool());
        desk.shot("touch-shelf");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void anotherLibraryIsOneKeyAway() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        QVERIFY(Fixtures::epub(desk.dir.filePath("two.epub"), "Second Story"));
        desk.add(desk.dir.filePath("one.epub"));
        desk.backend->useLibrary(desk.dir.filePath("Other"));
        desk.add(desk.dir.filePath("two.epub"));
        desk.backend->useLibrary(desk.dir.filePath("Library"));
        QVERIFY(desk.show());
        QCOMPARE(desk.value("libraryName", "text").toString(), QString("Library"));
        QTRY_COMPARE(desk.value("detailsTitle", "text").toString(), QString("The Test Book"));

        desk.key(Qt::Key_O);
        QTRY_VERIFY(desk.value("libraries", "opened").toBool());
        QTRY_VERIFY(desk.value("librariesList", "activeFocus").toBool());
        // This library, the other, and the one Omabook starts with.
        QCOMPARE(desk.value("librariesList", "count").toInt(), 3);
        desk.shot("libraries");
        desk.key(Qt::Key_Down);
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("libraries", "visible").toBool());
        QCOMPARE(desk.value("libraryName", "text").toString(), QString("Other"));
        QTRY_COMPARE(desk.value("detailsTitle", "text").toString(), QString("Second Story"));
        QCOMPARE(desk.settings->libraryPath(), desk.dir.filePath("Other"));

        // Syncing says what it needs when no computer is named.
        desk.key(Qt::Key_Y);
        QCOMPARE(desk.value("toastText", "text").toString(), QString("Name the computer to sync with in the settings first."));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aCalibreLibraryIsAddedAsAFolder() {
        Desk desk;
        QVERIFY(Fixtures::calibreLibrary(desk.dir.filePath("Calibre Library")));
        QVERIFY(desk.show());
        desk.picker.next = {QUrl::fromLocalFile(desk.dir.filePath("Calibre Library"))};
        desk.key(Qt::Key_A, Qt::ShiftModifier);
        QCOMPARE(desk.picker.asked, QList<FilePicker::Purpose>{FilePicker::AddFolder});
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 1);
        QTRY_COMPARE(desk.value("detailsTitle", "text").toString(), QString("Calibre's Title"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void thePluginsAreListedAndSwitched() {
        Desk desk;
        // One of the user's own, in the plugin folder.
        QVERIFY(QDir(desk.dir.path()).mkpath("plugins/shelfmark"));
        write(desk.dir.filePath("plugins/shelfmark/plugin.json"),
              "{\"name\": \"Shelfmark\", \"version\": \"0.3\", \"author\": \"Me\", \"type\": \"metadata\", "
              "\"description\": \"My own catalog.\", \"fields\": [\"title\", \"series\"], \"command\": [\"./lookup\"]}");
        QVERIFY(desk.show());
        desk.key(Qt::Key_P);
        QTRY_VERIFY(desk.value("plugins", "opened").toBool());
        desk.shot("plugins");
        // What Omabook is made of, its four catalogs, and the one added.
        QStringList ids;
        QVariantMap mine;
        for (const QVariant &plugin : desk.backend->plugins()) {
            ids << plugin.toMap().value("id").toString();
            if (ids.last() == "folder:shelfmark")
                mine = plugin.toMap();
        }
        for (const char *expected : {"format:EPUB", "conversion:Calibre conversion", "catalog:BibTeX", "ai:Antigravity",
                                     "builtin:Open Library", "builtin:Apple Books", "builtin:Crossref", "folder:shelfmark"})
            QVERIFY2(ids.contains(expected), expected);
        QCOMPARE(desk.value("pluginsList", "count").toInt(), int(ids.size()));
        QCOMPARE(mine.value("name").toString(), QString("Shelfmark"));
        QCOMPARE(mine.value("fields").toStringList(), (QStringList{"title", "series"}));
        QVERIFY(mine.value("enabled").toBool());

        // A source of details can be switched off, and stays off.
        desk.backend->setPluginEnabled("folder:shelfmark", false);
        QVERIFY(!desk.settings->pluginEnabled("folder:shelfmark", true));
        // What Omabook is made of can't.
        desk.backend->setPluginEnabled("format:EPUB", false);
        QVERIFY(desk.settings->pluginEnabled("format:EPUB", true));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("plugins", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aScriptPluginJoinsTheWindow() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        // Written while the window is up, as an agent would: a command with
        // a key of its own, and a panel beside the shelf fed by the script.
        QVERIFY(QDir(desk.dir.path()).mkpath("plugins/shelver"));
        write(desk.dir.filePath("plugins/shelver/Panel.qml"),
              "import QtQuick\nimport QtQuick.Controls\n"
              "Label { objectName: \"shelverPanel\"; text: (backend.scripts.data[\"shelver\"] || {}).note || \"\" }\n");
        write(desk.dir.filePath("plugins/shelver/main.js"),
              "omabook.commands.register({id: \"shelve\", title: \"Shelve it\", place: \"library\", key: \"Ctrl+R\",\n"
              "    run: function (context) {\n"
              "        omabook.ui.set(\"note\", \"shelved \" + context.book);\n"
              "        omabook.say(\"Shelved \" + omabook.library.get(context.book).title);\n"
              "    }});\n"
              "omabook.ui.panel({place: \"library.side\", title: \"Shelver\", width: 200, source: \"Panel.qml\"});\n"
              "omabook.ui.set(\"note\", \"nothing yet\");\n");
        write(desk.dir.filePath("plugins/shelver/plugin.json"),
              "{\"name\": \"Shelver\", \"version\": \"1.0\", \"script\": \"main.js\", \"permissions\": [\"http\"]}");
        QTRY_VERIFY(desk.find("shelverPanel"));
        QCOMPARE(desk.value("shelverPanel", "text").toString(), QString("nothing yet"));

        // Its key runs it for the selected book; what it says is said, and
        // what it set shows in its panel.
        desk.key(Qt::Key_R, Qt::ControlModifier);
        QTRY_COMPARE(desk.value("toastText", "text").toString(), QString("Shelved The Test Book"));
        QTRY_COMPARE(desk.value("shelverPanel", "text").toString(), QString("shelved %1").arg(id));

        // It is among the shortcuts, and among the plugins with what it may do.
        QCOMPARE(desk.custom->keyText().value("library.shelver:shelve").toString(), QString("Ctrl+R"));
        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        desk.shot("help-plugins");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("help", "visible").toBool());
        desk.key(Qt::Key_P);
        QTRY_VERIFY(desk.value("plugins", "opened").toBool());
        desk.shot("plugins-script");
        QVariantMap mine;
        for (const QVariant &plugin : desk.backend->plugins())
            if (plugin.toMap().value("id").toString() == "folder:shelver")
                mine = plugin.toMap();
        QCOMPARE(mine.value("kind").toString(), QString("script"));
        QCOMPARE(mine.value("permissions").toStringList(), QStringList{"http"});
        // Switched off, its key and its panel are gone.
        desk.backend->setPluginEnabled("folder:shelver", false);
        QTRY_VERIFY(!desk.find("shelverPanel"));
        QVERIFY(desk.custom->pluginCommands().isEmpty());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("plugins", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aScriptsFormatIsReadInTheWindow() {
        Desk desk;
        QVERIFY(desk.show());
        // A format written while the window is up: a book is its lines,
        // a chapter each.
        QVERIFY(QDir(desk.dir.path()).mkpath("plugins/lines"));
        write(desk.dir.filePath("plugins/lines/main.js"),
              "omabook.format.register({name: \"Lines\", extensions: [\"lines\"], open: function (source) {\n"
              "    var lines = source.text(source.entries()[0]).split(\"\\n\").filter(function (line) { return line !== \"\"; });\n"
              "    return {title: lines[0], authors: [\"Lin Liner\"],\n"
              "            chapters: lines.slice(1).map(function (line, i) { return {title: \"Line \" + (i + 1), text: line}; })};\n"
              "}});\n");
        write(desk.dir.filePath("plugins/lines/plugin.json"), "{\"name\": \"Lines\", \"script\": \"main.js\"}");
        write(desk.dir.filePath("book.lines"), "A Book of Lines\nThe first line.\nThe second line.\n");
        QTRY_VERIFY(Formats::isBook(desk.dir.filePath("book.lines")));

        // It joins the library with what the script said of it, and the
        // reader opens it beside the window like any book.
        const qint64 id = desk.add(desk.dir.filePath("book.lines"));
        QVERIFY(id > 0);
        QCOMPARE(desk.backend->book(id).value("title").toString(), QString("A Book of Lines"));
        QCOMPARE(desk.backend->book(id).value("authors").toString(), QString("Lin Liner"));
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->count(), 2);
        QCOMPARE(desk.reader->kind(), QString("flow"));
        QTRY_VERIFY(desk.reader->html().contains("The first line."));
        desk.key(Qt::Key_L);
        QTRY_VERIFY(desk.reader->html().contains("The second line."));
        desk.reader->close();

        // Switched off, the book stays on the shelf and says why it can't be read.
        desk.backend->setPluginEnabled("folder:lines", false);
        QTRY_VERIFY(!Formats::isBook(desk.dir.filePath("book.lines")));
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->error() != "");
        desk.reader->close();
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aBookThatGrowsIsFollowedAndAPluginAsksForWords() {
        Desk desk;
        QVERIFY(desk.show());
        // A plugin that writes a story as a text book, adds chapters to it,
        // and asks the reader where it should go.
        QVERIFY(QDir(desk.dir.path()).mkpath("plugins/teller"));
        write(desk.dir.filePath("plugins/teller/main.js"),
              "function story(chapters) {\n"
              "    var text = \"\";\n"
              "    for (var i = 1; i <= chapters; ++i)\n"
              "        text += \"CHAPTER \" + i + \"\\n\\nThe opening of chapter \" + i + \".\\n\\n\"\n"
              "                + new Array(60).join(\"And then a good deal more of it, at some length, as stories go.\\n\\n\");\n"
              "    return text;\n"
              "}\n"
              "function make() {\n"
              "    omabook.files.write(\"data/story.txt\", story(3));\n"
              "    return omabook.library.create({title: \"A Tale Being Told\", authors: [\"Tess Teller\"]}, \"data/story.txt\").id;\n"
              "}\n"
              "function grow(id) { omabook.files.write(\"data/story.txt\", story(5)); omabook.library.replaceFile(id, \"data/story.txt\"); }\n"
              "omabook.commands.register({id: \"steer\", title: \"Steer the story\", place: \"global\", key: \"Ctrl+R\", run: function () {\n"
              "    return omabook.ui.ask({title: \"What happens next?\", placeholder: \"A wish\", text: \"They go north.\"})\n"
              "        .then(function (words) { omabook.say(words === null ? \"Left to the teller.\" : \"Heard: \" + words); });\n"
              "}});\n");
        write(desk.dir.filePath("plugins/teller/plugin.json"), "{\"name\": \"Teller\", \"script\": \"main.js\", \"permissions\": [\"files\"]}");
        QTRY_VERIFY(desk.backend->scripts()->plugin("teller"));
        const qint64 id = desk.backend->scripts()->call("teller", "make").toLongLong();
        QVERIFY(id > 0);
        QTRY_COMPARE(desk.backend->book(id).value("title").toString(), QString("A Tale Being Told"));

        // The reader is partway down the second chapter, the contents open.
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->count(), 3);
        desk.key(Qt::Key_T);
        QTRY_VERIFY(desk.find("contents"));
        desk.reader->goToPlace(1, 0.5);
        QTRY_VERIFY(desk.reader->html().contains("The opening of chapter 2."));
        QTest::qWait(300);
        const double fraction = desk.reader->fraction();
        QVERIFY(fraction > 0.2 && fraction < 0.8);

        // The story grows under them. The window stays in the reader, on
        // the same chapter at the same place, with more ahead and the new
        // chapters in the contents.
        desk.backend->scripts()->call("teller", "grow", {id});
        QTRY_COMPARE(desk.reader->count(), 5);
        QVERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->section(), 1);
        QCOMPARE(desk.reader->toc().size(), 5);
        QTest::qWait(300);
        QVERIFY2(qAbs(desk.reader->fraction() - fraction) < 0.02, qPrintable(QString("%1 became %2").arg(fraction).arg(desk.reader->fraction())));
        QVERIFY(desk.reader->html().contains("The opening of chapter 2."));
        QVERIFY(desk.value("readerView", "visible").toBool());
        desk.shot("grown");
        // And reading on leads into what was added.
        desk.reader->goTo(4);
        QTRY_VERIFY(desk.reader->html().contains("The opening of chapter 5."));

        // The plugin's key asks for words, here in the reader: what is
        // typed is what the script hears.
        desk.key(Qt::Key_R, Qt::ControlModifier);
        QTRY_VERIFY(desk.value("pluginAsk", "opened").toBool());
        QCOMPARE(desk.value("pluginAskTitle", "text").toString(), QString("What happens next?"));
        QTRY_COMPARE(desk.value("pluginAskWords", "text").toString(), QString("They go north."));
        QTRY_VERIFY(desk.value("pluginAskWords", "activeFocus").toBool());
        desk.shot("plugin-ask");
        // The text is selected, so typing replaces it.
        desk.type("south");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.value("toastText", "text").toString(), QString("Heard: south"));
        QTRY_VERIFY(!desk.value("pluginAsk", "visible").toBool());
        // Escape leaves it unanswered, which is not an empty answer.
        desk.key(Qt::Key_R, Qt::ControlModifier);
        QTRY_VERIFY(desk.value("pluginAsk", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_COMPARE(desk.value("toastText", "text").toString(), QString("Left to the teller."));
        QTRY_VERIFY(!desk.value("pluginAsk", "visible").toBool());
        // The book is still open behind it.
        QVERIFY(desk.reader->isOpen());
        desk.reader->close();
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aPluginsPanelAndKeysAreThereForItsOwnBooksAndItShowsTheGraph() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 human = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QVERIFY(QDir(desk.dir.path()).mkpath("plugins/teller"));
        // The panel asks its script something that takes a while, and
        // shows the answer when it comes.
        write(desk.dir.filePath("plugins/teller/Story.qml"),
              "import QtQuick\nimport QtQuick.Controls\n"
              "Label {\n"
              "    objectName: \"storyPanel\"\n"
              "    property int wanted: -1\n"
              "    text: \"waiting\"\n"
              "    Component.onCompleted: wanted = backend.scripts.request(\"teller\", \"later\", [\"the plot\"])\n"
              "    Connections {\n"
              "        target: backend.scripts\n"
              "        function onAnswered(id, result, error) { if (id === wanted) text = error !== \"\" ? error : result }\n"
              "    }\n"
              "}\n");
        write(desk.dir.filePath("plugins/teller/main.js"),
              "function make() {\n"
              "    omabook.files.write(\"data/story.txt\", \"A story, told.\\n\");\n"
              "    return omabook.library.create({title: \"A Tale Being Told\"}, \"data/story.txt\").id;\n"
              "}\n"
              "function later(words) { return new Promise(function (resolve) { omabook.after(50, function () { resolve(\"later: \" + words); }); }); }\n"
              "omabook.ui.panel({place: \"reader.side\", title: \"Story\", width: 220, source: \"Story.qml\", when: \"generated\"});\n"
              "omabook.commands.register({id: \"steer\", title: \"Steer the story\", place: \"reader\", key: \"Ctrl+R\", when: \"generated\",\n"
              "    run: function (context) { omabook.say(\"Steering \" + context.book); }});\n"
              "omabook.commands.register({id: \"world\", title: \"Show the world\", place: \"global\", key: \"Ctrl+J\",\n"
              "    run: function () { omabook.ui.showGraph({scope: \"world:teller\"}); }});\n");
        write(desk.dir.filePath("plugins/teller/plugin.json"), "{\"name\": \"Teller\", \"script\": \"main.js\", \"permissions\": [\"files\"]}");
        QTRY_VERIFY(desk.backend->scripts()->plugin("teller"));
        const qint64 own = desk.backend->scripts()->call("teller", "make").toLongLong();
        QVERIFY(own > 0);
        auto offered = [&] {
            QStringList ids;
            for (const QVariant &command : desk.custom->pluginCommands())
                ids << command.toMap().value("id").toString();
            return ids;
        };

        // A person's book: no panel beside the page, taking no room, and
        // the plugin's key for its own books does nothing.
        desk.reader->openBook(human);
        QTRY_VERIFY(desk.reader->isOpen());
        QTest::qWait(250);
        QVERIFY(!desk.find("storyPanel"));
        QCOMPARE(offered(), QStringList{"global.teller:world"});
        desk.key(Qt::Key_R, Qt::ControlModifier);
        QTest::qWait(150);
        QCOMPARE(desk.backend->message(), QString());
        desk.reader->close();

        // The plugin's own: the panel is there, its question answered when
        // the script got round to it, and the key runs.
        desk.reader->openBook(own);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("storyPanel"));
        QTRY_COMPARE(desk.value("storyPanel", "text").toString(), QString("later: the plot"));
        QVERIFY(offered().contains("reader.teller:steer"));
        desk.key(Qt::Key_R, Qt::ControlModifier);
        QTRY_COMPARE(desk.value("toastText", "text").toString(), QString("Steering %1").arg(own));
        desk.shot("plugin-panel");

        // It asks for the graph of its world: beside the page while a book
        // is open, and staying on that world as pages turn.
        desk.key(Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
        QTRY_VERIFY(desk.value("readerGraph", "held").toBool());
        desk.reader->close();
        QTRY_VERIFY(!desk.find("storyPanel"));
        // And in place of the shelf when none is.
        desk.key(Qt::Key_J, Qt::ControlModifier);
        QTRY_VERIFY(desk.value("libraryGraph", "visible").toBool());
        desk.key(Qt::Key_Escape);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aWideWindowIsAnOpenBook() {
        Desk desk;
        // A chapter long enough for several pages, then a short one.
        QByteArray text = "CHAPTER 1\n\n";
        for (int i = 1; i <= 60; ++i)
            text += "Paragraph " + QByteArray::number(i) + " of a long chapter, with enough words in it to run over more than "
                    "one line of the page so the pages have something to cut between.\n\n";
        text += "CHAPTER 2\n\nThe short last chapter.\n";
        QVERIFY(Fixtures::write(desk.dir.filePath("Long - Ann Author.txt"), text));
        const qint64 id = desk.add(desk.dir.filePath("Long - Ann Author.txt"));
        QVERIFY(desk.show());
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());

        // Wider than tall: two pages side by side.
        QTRY_VERIFY(desk.find("spreadReader"));
        QObject *spread = desk.find("spreadReader");
        QTRY_VERIFY(spread->property("pages").toInt() > 3);
        const int pages = spread->property("pages").toInt();
        QCOMPARE(spread->property("page").toInt(), 0);
        // Pages are cut between lines: each starts where the one before ends,
        // and none is taller than the page.
        const QVariantList breaks = spread->property("breaks").toList();
        QCOMPARE(breaks.size(), pages + 1);
        for (int i = 1; i < breaks.size(); ++i) {
            QVERIFY(breaks.at(i).toDouble() > breaks.at(i - 1).toDouble());
            QVERIFY(breaks.at(i).toDouble() - breaks.at(i - 1).toDouble() <= spread->property("pageHeight").toDouble() + 0.5);
        }
        // The right page carries on where the left one stops.
        auto *left = qobject_cast<QQuickItem *>(desk.find("page0"));
        auto *right = qobject_cast<QQuickItem *>(desk.find("page1"));
        QVERIFY(left && right);
        QVERIFY(right->x() > left->x() + left->width());
        QCOMPARE(right->property("index").toInt(), 1);
        desk.shot("spread");

        // A turn moves on by two pages; the last spread turns to the next chapter.
        desk.key(Qt::Key_Space);
        QCOMPARE(spread->property("page").toInt(), 2);
        QVERIFY(desk.reader->fraction() > 0);
        desk.key(Qt::Key_Space, Qt::ShiftModifier);
        QCOMPARE(spread->property("page").toInt(), 0);
        desk.key(Qt::Key_End);
        QCOMPARE(spread->property("page").toInt(), spread->property("lastLeft").toInt());
        QCOMPARE(desk.reader->fraction(), 1.0);
        desk.key(Qt::Key_Space);
        QTRY_COMPARE(desk.reader->section(), 1);
        QTRY_COMPARE(desk.find("spreadReader")->property("page").toInt(), 0);
        // And back to the last spread of the chapter before.
        desk.key(Qt::Key_Space, Qt::ShiftModifier);
        QTRY_COMPARE(desk.reader->section(), 0);
        QTRY_COMPARE(desk.find("spreadReader")->property("page").toInt(), desk.find("spreadReader")->property("lastLeft").toInt());

        // Taller than wide: one page that scrolls, at the same place.
        desk.window->resize(760, 1100);
        QTRY_VERIFY(desk.find("flowReader"));
        QVERIFY(!desk.find("spreadReader"));
        QCOMPARE(desk.reader->section(), 0);
        QTRY_VERIFY(desk.reader->fraction() > 0.8);
        desk.shot("single");
        // And wide again.
        desk.window->resize(1180, 780);
        QTRY_VERIFY(desk.find("spreadReader"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void keysAreWhateverTheFileSays() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        // As shipped, Z does nothing and Space reads.
        desk.key(Qt::Key_Z);
        QVERIFY(!desk.reader->isOpen());

        // Give Z to "read" and take Space away, while the app runs.
        QVERIFY(QDir().mkpath(desk.dir.filePath("custom")));
        write(desk.dir.filePath("custom/keys.json"),
              "{\"library\": {\"Z\": \"read\", \"Space\": \"\"}, \"reader\": {\"Space\": \"next\", \"Ctrl+B\": \"back\"}}");
        QTRY_COMPARE(desk.custom->plainKeys().value("library.read").toStringList(), QStringList{"Z"});
        QCOMPARE(desk.custom->keyText().value("reader.next").toString(), QString("L  Right  N  Space"));
        // Space left the command that had it in the reader.
        QCOMPARE(desk.custom->plainKeys().value("reader.page_down").toStringList(), QStringList{"PgDown"});
        // A key with Ctrl is one that works while typing.
        QCOMPARE(desk.custom->chordKeys().value("reader.back").toStringList(), QStringList{"Ctrl+B"});
        desk.key(Qt::Key_Space);
        QVERIFY(!desk.reader->isOpen());
        desk.key(Qt::Key_Z);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->bookId(), id);
        QTRY_VERIFY(desk.find("flowText"));
        // Space is now "next chapter", at once.
        desk.key(Qt::Key_Space);
        QCOMPARE(desk.reader->section(), 1);
        desk.key(Qt::Key_B, Qt::ControlModifier);
        QTRY_VERIFY(!desk.reader->isOpen());

        // A file with a mistake changes nothing, and says what is wrong.
        write(desk.dir.filePath("custom/keys.json"), "{\"library\": {\"R\": \"fly\"}}");
        QTRY_VERIFY(QFile::exists(desk.dir.filePath("custom/keys.error")));
        QFile error(desk.dir.filePath("custom/keys.error"));
        QVERIFY(error.open(QIODevice::ReadOnly));
        QVERIFY(error.readAll().contains("no command \"library.fly\""));
        QCOMPARE(desk.custom->plainKeys().value("library.read").toStringList(), QStringList{"Z"});
        // Fixed, the note goes; removed, the defaults are back.
        QVERIFY(QFile::remove(desk.dir.filePath("custom/keys.json")));
        QTRY_COMPARE(desk.custom->plainKeys().value("library.read").toStringList(), QStringList{"Space"});
        QVERIFY(!QFile::exists(desk.dir.filePath("custom/keys.error")));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theLookAndTheSettingsAreFiles() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QCOMPARE(desk.window->color(), QColor("#0e0e10"));

        // style.json lies over the desktop's theme, live.
        QVERIFY(QDir().mkpath(desk.dir.filePath("custom")));
        write(desk.dir.filePath("custom/style.json"),
              "{\"colors\": {\"background\": \"#f4ecd8\", \"foreground\": \"#3b2f20\", \"accent\": \"#8a4b2a\"}, \"radius\": 9}");
        QTRY_COMPARE(desk.window->color(), QColor("#f4ecd8"));
        QCOMPARE(desk.look->accent(), QColor("#8a4b2a"));
        QCOMPARE(desk.look->accentForeground(), QColor("white"));
        // A pale background of your own makes it a light look.
        QVERIFY(desk.look->light());
        QCOMPARE(desk.look->radius(), 9);
        // What the file doesn't name is still the theme's.
        QCOMPARE(desk.look->colors().value("red").value<QColor>(), desk.theme->colors().value("red").value<QColor>());
        desk.shot("styled");

        write(desk.dir.filePath("custom/style.json"), "{\"colors\": {\"accent\": \"brownish\"}}");
        QTRY_VERIFY(QFile::exists(desk.dir.filePath("custom/style.error")));
        QCOMPARE(desk.look->accent(), QColor("#8a4b2a"));
        QVERIFY(QFile::remove(desk.dir.filePath("custom/style.json")));
        QTRY_COMPARE(desk.window->color(), QColor("#0e0e10"));

        // config.json edited by hand, or by an agent, is followed too.
        const int columns = desk.value("grid", "columns").toInt();
        write(desk.dir.filePath("config.json"),
              "{\"library\": \"" + desk.dir.filePath("Library").toUtf8() + "\", \"cover_size\": 320, \"justify\": true}");
        QTRY_COMPARE(desk.settings->coverSize(), 320);
        QVERIFY(desk.settings->justify());
        QTRY_VERIFY(desk.value("grid", "columns").toInt() < columns);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aViewAndAPanelOfYourOwn() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QVERIFY(!desk.find("myDetails"));

        // A panel beside the shelf: a mod, which adds without replacing.
        QVERIFY(QDir().mkpath(desk.dir.filePath("custom/mods/count")));
        write(desk.dir.filePath("custom/mods/count/mod.json"), "{\"place\": \"library.side\", \"title\": \"Count\", \"width\": 200}");
        write(desk.dir.filePath("custom/mods/count/Main.qml"),
              "import QtQuick\nimport QtQuick.Controls\nLabel { objectName: \"myCount\"; text: backend.books.total + \" on the shelf\"; color: theme.accent }\n");
        QTRY_VERIFY(desk.find("myCount"));
        QCOMPARE(desk.value("myCount", "text").toString(), QString("1 on the shelf"));

        // A view of your own in place of the built-in one: the window is
        // loaded again with it, and the library is as it was.
        QString problem;
        const QString details = desk.custom->exportView("DetailsPane", &problem);
        QVERIFY2(!details.isEmpty(), qPrintable(problem));
        QFile view(details);
        QVERIFY(view.open(QIODevice::ReadOnly));
        QByteArray source = view.readAll();
        view.close();
        QVERIFY(source.contains("objectName: \"detailsTitle\""));
        source.replace("objectName: \"detailsTitle\"", "objectName: \"myDetails\"");
        write(details, source);
        QSignalSpy loaded(&*desk.interface, &Interface::loaded);
        QVERIFY(loaded.wait(5000));
        QVERIFY(desk.activate());
        QTRY_VERIFY(desk.find("myDetails"));
        QTRY_COMPARE(desk.value("myDetails", "text").toString(), QString("The Test Book"));
        QVERIFY(!desk.find("detailsTitle"));
        QVERIFY(!QFile::exists(desk.dir.filePath("custom/ui.error")));
        QTRY_VERIFY(desk.find("myCount"));

        // A view with a mistake in it: the built-in window comes up instead,
        // and ui.error says where the mistake is.
        write(details, "import QtQuick\nItem { this is not QML }\n");
        QVERIFY(loaded.wait(5000));
        QVERIFY(desk.activate());
        QTRY_VERIFY(desk.find("detailsTitle"));
        QFile error(desk.dir.filePath("custom/ui.error"));
        QVERIFY(error.open(QIODevice::ReadOnly));
        QVERIFY2(error.readAll().contains("DetailsPane.qml"), "ui.error should name the file");

        // Taken away again, everything is as shipped.
        QVERIFY(QFile::remove(details));
        QVERIFY(loaded.wait(5000));
        QVERIFY(desk.activate());
        QTRY_VERIFY(desk.find("detailsTitle"));
        QVERIFY(!QFile::exists(desk.dir.filePath("custom/ui.error")));
    }

    void theLibraryIsTidiedAsOneBatch() {
        Desk desk;
        // Two copies of one book under two spellings of its author, and a third book.
        auto add = [&](const QString &name, const QString &title, const QString &author, const QByteArray &words) {
            Fixtures::write(desk.dir.filePath(name), words);
            BookMeta meta;
            meta.title = title;
            meta.authors = QStringList{author};
            return desk.backend->library()->add(desk.dir.filePath(name), meta).id;
        };
        const qint64 first = add("a.txt", "Dragon Wing", "Margaret Weis", "One.\n");
        const qint64 second = add("b.md", "Dragon Wing", "Weis, Margaret", "# Two\n");
        const qint64 third = add("c.txt", "Elven Star", "Margaret Weis", "Three.\n");
        desk.backend->reload();
        // The model's part: which books are a series.
        desk.ai.answer = "[{\"id\": " + QString::number(third) + ", \"series\": \"The Death Gate Cycle\", \"series_index\": 2}]";
        QVERIFY(desk.show());

        desk.key(Qt::Key_T, Qt::ShiftModifier);
        QTRY_VERIFY(desk.value("tidy", "opened").toBool());
        // The duplicate and the spelling are there at once; the series when the model answers.
        QTRY_COMPARE(desk.value("tidyList", "count").toInt(), 3);
        QTRY_VERIFY(!desk.librarian->busy());
        QStringList kinds;
        for (const QVariant &proposal : desk.librarian->proposals())
            kinds << proposal.toMap().value("kind").toString();
        QCOMPARE(kinds, (QStringList{"duplicate", "author", "series"}));
        QCOMPARE(desk.librarian->proposals().at(1).toMap().value("changes").toList().first().toMap().value("to").toString(),
                 QString("Margaret Weis"));
        QVERIFY(desk.ai.requests.first().messages.first().text.contains("Elven Star"));
        desk.shot("tidy");

        // Unticked, a change is left alone: here, the series.
        QTRY_VERIFY(desk.value("tidyList", "activeFocus").toBool());
        desk.key(Qt::Key_Down);
        desk.key(Qt::Key_Down);
        desk.key(Qt::Key_Space);
        QCOMPARE(desk.librarian->chosen(), 2);
        QMetaObject::invokeMethod(desk.find("tidyApply"), "clicked");
        QTRY_VERIFY(!desk.value("tidy", "visible").toBool());
        QCOMPARE(desk.value("toastText", "text").toString(), QString("2 changes made"));
        // One Dragon Wing with both formats, under one spelling; no series.
        Library *library = desk.backend->library();
        QCOMPARE(library->books().size(), 2);
        QCOMPARE(library->book(first)->formats, (QStringList{"md", "txt"}));
        QVERIFY(!library->book(second));
        QCOMPARE(library->book(third)->meta.series, QString());
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 2);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theLibraryAnswersQuestionsAndOmabookChangesWhenAsked() {
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        desk.backend->library()->setRecap(id, {1, 0, "The test has begun.", 0});
        QVERIFY(desk.show());

        // W asks about the library: what it holds, and the notes on what was read.
        desk.ai.answer = "You have one book, The Test Book.";
        desk.key(Qt::Key_W);
        QTRY_VERIFY(desk.value("askLibrary", "opened").toBool());
        QTRY_VERIFY(desk.value("askLibraryWords", "activeFocus").toBool());
        desk.type("what do i have?");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.ai.requests.size(), 1);
        const QString asked = desk.ai.requests.first().messages.first().text;
        QVERIFY(asked.contains("The Test Book") && asked.contains("The test has begun.") && asked.endsWith("what do i have?"));
        QTRY_COMPARE(desk.librarian->answer(), desk.ai.answer);
        QTRY_VERIFY(!desk.librarian->answering());
        desk.shot("ask-library");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("askLibrary", "visible").toBool());

        // N asks what to read next that isn't on the shelf, from what was read.
        desk.ai.answer = "Try Moby-Dick.";
        desk.key(Qt::Key_N);
        QTRY_VERIFY(desk.value("askLibrary", "opened").toBool());
        QTRY_COMPARE(desk.ai.requests.size(), 2);
        QVERIFY(desk.ai.requests.last().system.contains("Never suggest a book that is already in the list"));
        QVERIFY(desk.ai.requests.last().messages.first().text.contains("Suggest what I should read next"));
        QVERIFY(desk.ai.requests.last().messages.first().text.contains("The test has begun."));
        QTRY_COMPARE(desk.librarian->answer(), QString("Try Moby-Dick."));
        QTRY_VERIFY(!desk.librarian->answering());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("askLibrary", "visible").toBool());

        // Ctrl+K asks for a change to Omabook itself; the answer rewrites the
        // interface files, and the window follows.
        desk.ai.answer = "{\"style\": {\"colors\": {\"background\": \"#221a12\", \"accent\": \"#d9953f\"}}, \"keys\": "
                         "{\"library\": {\"R\": \"read\"}}, \"settings\": {\"cover_size\": 240}, \"say\": \"Warmer, with bigger covers.\"}";
        desk.key(Qt::Key_K, Qt::ControlModifier);
        QTRY_VERIFY(desk.value("changeIt", "opened").toBool());
        QTRY_VERIFY(desk.value("changeItWords", "activeFocus").toBool());
        desk.type("make it warmer");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.librarian->changeNote(), QString("Warmer, with bigger covers."));
        QVERIFY(desk.ai.requests.last().messages.first().text.contains("<commands>"));
        QTRY_COMPARE(desk.window->color(), QColor("#221a12"));
        QCOMPARE(desk.settings->coverSize(), 240);
        QCOMPARE(desk.custom->plainKeys().value("library.read").toStringList(), (QStringList{"Space", "R"}));
        QVERIFY(desk.librarian->canUndo());
        desk.shot("change");

        // And one click puts it all back.
        QMetaObject::invokeMethod(desk.find("changeItUndo"), "clicked");
        QTRY_COMPARE(desk.window->color(), QColor("#0e0e10"));
        QCOMPARE(desk.settings->coverSize(), 176);
        QCOMPARE(desk.custom->plainKeys().value("library.read").toStringList(), QStringList{"Space"});
        QVERIFY(!desk.librarian->canUndo());

        // An answer that can't be used changes nothing, and says so.
        desk.ai.answer = "{\"style\": {\"colors\": {\"accent\": \"not a color\"}}}";
        desk.librarian->change("break it");
        QTRY_VERIFY(desk.librarian->changeNote().startsWith("Nothing was changed"));
        QCOMPARE(desk.window->color(), QColor("#0e0e10"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void everyViewShowsTheShelfAndAnswersKeysAndTaps() {
        Desk desk;
        QVERIFY(desk.show());
        const QList<qint64> ids = stock(desk);
        QCOMPARE(ids.size(), 28);
        BookListModel *books = desk.backend->books();
        // By title, so the rows are known: "A Map of Low Water" leads.
        books->setSort("title");
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 28);
        auto selected = [&] { return desk.value("detailsTitle", "text").toString(); };
        auto titleAt = [&](int row) { return books->get(row).value("title").toString(); };
        auto click = [&](const QPointF &at) { QTest::mouseClick(desk.window, Qt::LeftButton, {}, at.toPoint()); };
        auto doubleClick = [&](const QPointF &at) { QTest::mouseDClick(desk.window, Qt::LeftButton, {}, at.toPoint()); };
        auto backToTheShelf = [&] {
            if (!QTest::qWaitFor([&] { return desk.reader->isOpen(); }))
                return false;
            desk.reader->close();
            return QTest::qWaitFor([&] { return !desk.reader->isOpen(); });
        };
        // Each view, looked over once before anything is pressed.
        for (const QString &view : {QString("spines"), QString("carousel"), QString("list"), QString("stacks"), QString("covers")}) {
            desk.backend->setView(view);
            QTRY_VERIFY(desk.find(view == "covers" ? QString("grid") : view));
            QMetaObject::invokeMethod(desk.find("shelfLoader")->property("item").value<QObject *>(), "show", Q_ARG(QVariant, 9));
            desk.shot("view-" + view);
        }
        QMetaObject::invokeMethod(desk.find("shelfLoader")->property("item").value<QObject *>(), "show", Q_ARG(QVariant, 0));
        // The first book that is a file, which Space can open.
        int file = 0;
        while (books->get(file).value("held").toString() != "digital")
            ++file;

        // Covers: the grid, now with what is on paper or wished for marked,
        // the finished checked, and stars where a book was rated.
        QCOMPARE(desk.backend->view(), QString("covers"));
        QTRY_COMPARE(selected(), titleAt(0));
        QVERIFY(desk.find("heldBadge"));
        desk.key(Qt::Key_L);
        QCOMPARE(selected(), titleAt(1));

        // 2 is the bookshelf. The book that was selected still is, and is
        // pulled out to show its cover.
        desk.key(Qt::Key_2);
        QTRY_VERIFY(desk.find("spines"));
        QCOMPARE(desk.backend->view(), QString("spines"));
        // The view before is taken down a moment after the new one is up.
        QTRY_VERIFY(!desk.find("grid"));
        QTRY_COMPARE(selected(), titleAt(1));
        QTRY_VERIFY(desk.find("pulledOut"));
        QVERIFY(desk.find("paperLabel"));
        // L and H walk along the shelf; J and K go to the shelf below and back.
        desk.key(Qt::Key_L);
        QCOMPARE(selected(), titleAt(2));
        desk.key(Qt::Key_H);
        desk.key(Qt::Key_H);
        QCOMPARE(selected(), titleAt(0));
        auto *spines = qobject_cast<QQuickItem *>(desk.find("spines"));
        const qreal tall = std::round(desk.settings->coverSize() * 1.25);
        const QVariantList shelves = books->shelve(spines->width() - 64, tall);
        QVERIFY(shelves.size() >= 2);
        auto showRow = [&](int row) {
            QMetaObject::invokeMethod(desk.find("shelfLoader")->property("item").value<QObject *>(), "show", Q_ARG(QVariant, row));
        };
        // From a standing book, down is the book nearest below it on the
        // next shelf, and up comes back to the first.
        int standing = 0;
        while (shelves.at(0).toList().at(standing).toMap().value("flat").toBool())
            ++standing;
        showRow(standing);
        QTRY_COMPARE(selected(), titleAt(standing));
        desk.key(Qt::Key_J);
        const QString under = selected();
        bool onSecond = false;
        for (const QVariant &slot : shelves.at(1).toList())
            onSecond = onSecond || titleAt(slot.toMap().value("row").toInt()) == under;
        QVERIFY(onSecond);
        desk.key(Qt::Key_K);
        QVERIFY(selected() != under);
        // A click on a spine selects it, a double click opens the book.
        auto spineAt = [&](int row) {
            for (int shelf = 0; shelf < shelves.size(); ++shelf)
                for (const QVariant &place : shelves.at(shelf).toList()) {
                    const QVariantMap slot = place.toMap();
                    if (slot.value("row").toInt() == row)
                        return spines->mapToScene(QPointF(32 + slot.value("x").toReal() + slot.value("w").toReal() / 2,
                                                          shelf * (tall + 52) + tall + 30 - slot.value("y").toReal()
                                                              - slot.value("h").toReal() / 2));
                }
            return QPointF();
        };
        const int last = shelves.at(0).toList().last().toMap().value("row").toInt();
        click(spineAt(last));
        QTRY_COMPARE(selected(), titleAt(last));
        showRow(file);
        QTRY_COMPARE(selected(), titleAt(file));
        desk.key(Qt::Key_Space);
        QVERIFY(backToTheShelf());

        // 3 is the carousel: the selected book in the middle, what it is
        // underneath, and no pane beside it.
        desk.key(Qt::Key_3);
        QTRY_VERIFY(desk.find("carousel"));
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file));
        QVERIFY(!desk.value("details", "visible").toBool());
        desk.key(Qt::Key_L);
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file + 1));
        desk.key(Qt::Key_Left);
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file));
        auto *carousel = qobject_cast<QQuickItem *>(desk.find("carousel"));
        const QPointF middle = carousel->mapToScene(QPointF(carousel->width() / 2, carousel->height() / 2));
        const QPointF beside = middle + QPointF(carousel->property("coverWidth").toReal() * 0.9, 0);
        // A click on the book beside it brings that one to the middle.
        click(beside);
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file + 1));
        QVERIFY(!desk.reader->isOpen());
        desk.key(Qt::Key_H);
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file));
        QTest::qWait(350);
        // A finger does the same, and a tap on the middle one opens it.
        QPointingDevice *finger = QTest::createTouchDevice();
        auto tap = [&](const QPointF &at) {
            QTest::touchEvent(desk.window, finger).press(0, at.toPoint(), desk.window);
            QTest::touchEvent(desk.window, finger).release(0, at.toPoint(), desk.window);
        };
        tap(beside);
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file + 1));
        QVERIFY(!desk.reader->isOpen());
        desk.key(Qt::Key_H);
        QTRY_COMPARE(desk.value("carouselTitle", "text").toString(), titleAt(file));
        QTest::qWait(350);
        tap(middle);
        QVERIFY(backToTheShelf());
        // A swipe slides the row along, and the book it stops on is selected.
        QTest::touchEvent(desk.window, finger).press(0, beside.toPoint(), desk.window);
        for (int step = 1; step <= 6; ++step) {
            QTest::touchEvent(desk.window, finger).move(0, (beside - QPointF(60.0 * step, 0)).toPoint(), desk.window);
            QTest::qWait(20);
        }
        QTest::touchEvent(desk.window, finger).release(0, (beside - QPointF(360, 0)).toPoint(), desk.window);
        QTRY_VERIFY(desk.value("carousel", "currentIndex").toInt() > file);
        QTRY_VERIFY(!desk.value("carousel", "moving").toBool());
        QCOMPARE(desk.value("carouselTitle", "text").toString(), titleAt(desk.value("carousel", "currentIndex").toInt()));
        showRow(file);
        QVERIFY(desk.value("viewSwitcher", "visible").toBool());

        // 4 is the list. J and K walk it; H and L walk the order along the
        // headings, and a click on a heading sorts by it, a second turns it.
        desk.key(Qt::Key_4);
        QTRY_VERIFY(desk.find("list"));
        QTRY_COMPARE(selected(), titleAt(file));
        desk.key(Qt::Key_J);
        QCOMPARE(selected(), titleAt(file + 1));
        desk.key(Qt::Key_K);
        QCOMPARE(selected(), titleAt(file));
        const QString kept = selected();
        desk.key(Qt::Key_L);
        QCOMPARE(books->sort(), QString("author"));
        QCOMPARE(desk.value("sortName", "text").toString(), QString("Author"));
        // The selection stays on the book, wherever the new order puts it.
        QTRY_COMPARE(selected(), kept);
        auto *heading = qobject_cast<QQuickItem *>(desk.find("heading-title"));
        QVERIFY(heading);
        click(heading->mapToScene(QPointF(30, heading->height() / 2)));
        QCOMPARE(books->sort(), QString("title"));
        QVERIFY(!books->descending());
        click(heading->mapToScene(QPointF(30, heading->height() / 2)));
        QVERIFY(books->descending());
        QCOMPARE(desk.settings->sortReversed(), true);
        QCOMPARE(titleAt(0), QString("Winter Gears"));
        click(heading->mapToScene(QPointF(30, heading->height() / 2)));
        QVERIFY(!books->descending());
        auto *list = qobject_cast<QQuickItem *>(desk.find("list"));
        // A line is taller once the window has been touched, as it was above.
        const qreal line = desk.window->property("touch").toBool() ? 44 : 32;
        auto lineAt = [&](int row) { return list->mapToScene(QPointF(200, line * (row + 0.5) - list->property("contentY").toReal())); };
        showRow(0);
        click(lineAt(5));
        QTRY_COMPARE(selected(), titleAt(5));
        doubleClick(lineAt(file));
        QVERIFY(backToTheShelf());

        // 5 is the stacks: a pile per series, the books in none last. Space
        // opens a pile into its books, which is filtering by it; Escape
        // closes it again.
        desk.key(Qt::Key_5);
        QTRY_VERIFY(desk.find("stacks"));
        QTRY_COMPARE(desk.value("stacks", "count").toInt(), 3);
        QCOMPARE(desk.value("stacksHeading", "text").toString(), QString("By series"));
        QVERIFY(!desk.value("details", "visible").toBool());
        desk.key(Qt::Key_L);
        desk.key(Qt::Key_Space);
        QTRY_COMPARE(books->filters(), (QVariantMap{{"series", "The Salt Roads"}}));
        QCOMPARE(books->count(), 3);
        QCOMPARE(desk.value("stacksHeading", "text").toString(), QString("The Salt Roads"));
        QVERIFY(desk.find("chip-series"));
        QTRY_COMPARE(selected(), QString("A Map of Low Water"));
        desk.key(Qt::Key_L);
        QCOMPARE(selected(), QString("Harbour of Glass"));
        desk.shot("view-stacks-open");
        desk.key(Qt::Key_Space);
        QVERIFY(backToTheShelf());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(books->filters().isEmpty());
        QCOMPARE(books->count(), 28);
        // It comes back to the pile that was open.
        QTRY_COMPARE(desk.value("stacks", "currentIndex").toInt(), 1);
        // A click selects a pile and a double click opens it.
        auto *stacks = qobject_cast<QQuickItem *>(desk.find("stacks"));
        const QPointF first = stacks->mapToScene(QPointF(stacks->property("cellWidth").toReal() / 2, 150));
        click(first);
        QTRY_COMPARE(desk.value("stacks", "currentIndex").toInt(), 0);
        doubleClick(first);
        QTRY_COMPARE(books->filters(), (QVariantMap{{"series", "Clockwork Orchard"}}));
        desk.key(Qt::Key_Backspace);
        QTRY_VERIFY(books->filters().isEmpty());
        // G piles by something else: authors, tags, reading status.
        desk.key(Qt::Key_G);
        QCOMPARE(desk.backend->group(), QString("author"));
        QTRY_COMPARE(desk.value("stacks", "count").toInt(), 11);
        desk.key(Qt::Key_G);
        desk.key(Qt::Key_G);
        QCOMPARE(desk.backend->group(), QString("status"));
        QTRY_COMPARE(desk.value("stacks", "count").toInt(), 4);
        desk.shot("view-stacks-status");

        // V goes round the views, and the drawings in the corner switch too.
        desk.key(Qt::Key_V);
        QCOMPARE(desk.backend->view(), QString("covers"));
        QTRY_VERIFY(desk.find("grid"));
        auto *pick = qobject_cast<QQuickItem *>(desk.find("view-list"));
        QVERIFY(pick);
        click(pick->mapToScene(QPointF(pick->width() / 2, pick->height() / 2)));
        QCOMPARE(desk.backend->view(), QString("list"));
        QTRY_VERIFY(desk.find("list"));

        // The view and the grouping are remembered with the library.
        const QJsonObject kept2 = desk.settings->shelf(desk.settings->libraryPath());
        QCOMPARE(kept2.value("view").toString(), QString("list"));
        QCOMPARE(kept2.value("group").toString(), QString("status"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theShelfIsFilteredAndOrderedWithoutAToolbar() {
        Desk desk;
        QVERIFY(desk.show());
        QCOMPARE(stock(desk).size(), 28);
        BookListModel *books = desk.backend->books();
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 28);
        auto fitting = [&](const QString &words) { return int(desk.backend->library()->books(words).size()); };

        // F asks what to filter by: a few letters and Enter.
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("palette", "opened").toBool());
        QTRY_VERIFY(desk.value("paletteField", "activeFocus").toBool());
        QVERIFY(desk.value("paletteList", "count").toInt() > 10);
        desk.shot("filter");
        desk.type("paper");
        QCOMPARE(desk.value("paletteList", "count").toInt(), 1);
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("palette", "visible").toBool());
        QCOMPARE(books->filters(), (QVariantMap{{"held", "paper"}}));
        QCOMPARE(books->count(), fitting("is:paper"));
        QVERIFY(books->count() > 0 && books->count() < 28);
        QCOMPARE(desk.value("bookCount", "text").toString(), QString("%1 of 28").arg(books->count()));
        QVERIFY(desk.find("chip-held"));
        // The shelf has the keyboard again.
        QTRY_VERIFY(desk.value("grid", "activeFocus").toBool());

        // A second filter narrows further; "read" means Read, not Reading.
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("paletteField", "activeFocus").toBool());
        desk.type("read");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("palette", "visible").toBool());
        QCOMPARE(books->filters(), (QVariantMap{{"held", "paper"}, {"status", "read"}}));
        QCOMPARE(books->count(), fitting("is:paper status:read"));
        QVERIFY(desk.find("chip-status"));
        // And the search box narrows what the filters left.
        desk.key(Qt::Key_Slash);
        QTRY_VERIFY(desk.value("search", "activeFocus").toBool());
        desk.type("fog");
        QCOMPARE(books->count(), 1);
        QCOMPARE(books->count(), fitting("is:paper status:read fog"));
        desk.key(Qt::Key_Escape);
        desk.shot("filtered");

        // A chip is taken off with a click, and Shift+F takes them all off.
        auto *chip = qobject_cast<QQuickItem *>(desk.find("chip-held"));
        QVERIFY(chip);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
        QTRY_COMPARE(books->filters(), (QVariantMap{{"status", "read"}}));
        QVERIFY(!desk.find("chip-held"));
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("paletteField", "activeFocus").toBool());
        desk.type("fantasy");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(books->filters(), (QVariantMap{{"status", "read"}, {"tag", "Fantasy"}}));
        QTRY_VERIFY(!desk.value("palette", "visible").toBool());
        QCOMPARE(books->count(), fitting("status:read tag:Fantasy"));
        // Each other kind of filter, set the way the box sets it.
        for (const auto &[kind, value, words] : QList<std::tuple<QString, QVariant, QString>>{
                 {"format", "epub", "format:epub"}, {"language", "nl", "lang:nl"}, {"author", "Mira Okafor", "author:Okafor"},
                 {"series", "The Salt Roads", "series:Salt"}, {"rating", 4, "rating:4"}, {"held", "wishlist", "is:wishlist"},
                 {"status", "to-read", "status:to-read"}, {"status", "unread", "status:unread"}}) {
            books->clearFilters();
            books->setFilter(kind, value);
            QVERIFY2(books->count() > 0 && books->count() < 28, qPrintable(kind));
            QCOMPARE(books->count(), fitting(words));
            QVERIFY(desk.find("chip-" + kind));
        }
        books->setFilter("tag", "Nature");
        desk.key(Qt::Key_F, Qt::ShiftModifier);
        QVERIFY(books->filters().isEmpty());
        QCOMPARE(books->count(), 28);
        QVERIFY(!desk.find("chip-tag"));

        // A filter nothing passes says so, with the way out.
        books->setFilters({{"held", "wishlist"}, {"rating", 5}});
        QCOMPARE(books->count(), 0);
        QVERIFY(desk.value("emptyState", "visible").toBool());
        QVERIFY(desk.value("clearFilters", "visible").toBool());
        desk.shot("filtered-empty");
        QMetaObject::invokeMethod(desk.find("clearFilters"), "clicked");
        QCOMPARE(books->count(), 28);

        // S goes through the orders, each the way it is most wanted;
        // Shift+S turns the order round.
        QCOMPARE(books->sort(), QString("recent"));
        QStringList seen;
        for (int press = 0; press < 9; ++press) {
            desk.key(Qt::Key_S);
            seen << books->sort();
        }
        QCOMPARE(seen, (QStringList{"title", "author", "series", "added", "progress", "rating", "published", "finished", "recent"}));
        desk.key(Qt::Key_S);
        QCOMPARE(desk.value("sortName", "text").toString(), QString("Title"));
        QCOMPARE(desk.value("sortWay", "name").toString(), QString("up"));
        const QString first = books->get(0).value("title").toString();
        desk.key(Qt::Key_S, Qt::ShiftModifier);
        QVERIFY(books->reversed());
        QCOMPARE(desk.value("sortWay", "name").toString(), QString("down"));
        QCOMPARE(books->get(27).value("title").toString(), first);
        // A click on the order lists them all, to pick one by name.
        auto *order = qobject_cast<QQuickItem *>(desk.find("sortName"));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, order->mapToScene(QPointF(10, order->height() / 2)).toPoint());
        QTRY_VERIFY(desk.value("paletteField", "activeFocus").toBool());
        QCOMPARE(desk.value("paletteList", "count").toInt(), 10);
        desk.shot("order");
        desk.type("rat");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("palette", "visible").toBool());
        QCOMPARE(books->sort(), QString("rating"));
        QVERIFY(!books->reversed());
        QCOMPARE(books->get(0).value("rating").toInt(), 5);

        // All of it is kept: the order for every library, the filters and
        // the view for this one.
        books->setFilter("tag", "Nature");
        desk.key(Qt::Key_2);
        QTRY_VERIFY(desk.find("spines"));
        QCOMPARE(desk.settings->sort(), QString("rating"));
        {
            Backend again(&*desk.settings, &desk.picker, nullptr, {}, {}, desk.dir.filePath("plugins"), "-");
            QCOMPARE(again.view(), QString("spines"));
            QCOMPARE(again.books()->sort(), QString("rating"));
            QCOMPARE(again.books()->filters(), (QVariantMap{{"tag", "Nature"}}));
            QCOMPARE(again.books()->count(), books->count());
        }
        // Another library starts as covers with no filters, and coming back
        // finds this one as it was left.
        const QString here = desk.settings->libraryPath();
        desk.backend->useLibrary(desk.dir.filePath("Other"));
        QCOMPARE(desk.backend->view(), QString("covers"));
        QVERIFY(books->filters().isEmpty());
        QTRY_VERIFY(desk.find("grid"));
        desk.backend->useLibrary(here);
        QCOMPARE(desk.backend->view(), QString("spines"));
        QCOMPARE(books->filters(), (QVariantMap{{"tag", "Nature"}}));
        QTRY_VERIFY(desk.find("spines"));
        // config.json is followed while the app runs, the shelf's part too.
        QJsonObject shelf = desk.settings->shelf(here);
        shelf.insert("view", "carousel");
        shelf.insert("filters", QJsonObject{{"rating", 4}});
        desk.settings->setShelf(here, shelf);
        QCOMPARE(desk.backend->view(), QString("carousel"));
        QCOMPARE(books->filters(), (QVariantMap{{"rating", 4}}));
        QTRY_VERIFY(desk.find("carousel"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void everyViewInALightThemeAndRightToLeft() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                 "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";
        const QStringList views{"covers", "spines", "carousel", "list", "stacks"};
        const QStringList shows{"grid", "spines", "carousel", "list", "stacks"};
        {
            // A light theme, left to right: what the README shows beside the dark one.
            Desk desk({}, "en", light);
            QVERIFY(desk.show());
            QCOMPARE(stock(desk).size(), 28);
            desk.backend->books()->setSort("author");
            QCOMPARE(desk.window->color(), QColor("#fafafa"));
            for (int i = 0; i < views.size(); ++i) {
                desk.backend->setView(views.at(i));
                QTRY_VERIFY(desk.find(shows.at(i)));
                desk.key(Qt::Key_L);
                desk.key(Qt::Key_L);
                QTest::qWait(300);
                desk.shot("view-" + views.at(i) + "-light");
            }
            desk.key(Qt::Key_F);
            QTRY_VERIFY(desk.value("palette", "opened").toBool());
            desk.shot("filter-light");
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("palette", "visible").toBool());
            QTest::keyClick(desk.window, '?');
            QTRY_VERIFY(desk.value("help", "opened").toBool());
            desk.shot("help-light");
            desk.key(Qt::Key_Escape);
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        // The same in Arabic: everything mirrors, and the keys that go left
        // and right still go the way they point.
        Desk desk({}, "ar", light);
        QVERIFY(desk.show());
        QCOMPARE(stock(desk).size(), 28);
        BookListModel *books = desk.backend->books();
        books->setSort("title");
        books->setFilter("status", "unread");
        QVERIFY(desk.window->property("rightToLeft").toBool());
        for (int i = 0; i < views.size(); ++i) {
            desk.backend->setView(views.at(i));
            QTRY_VERIFY(desk.find(shows.at(i)));
            QTest::qWait(200);
            desk.shot("rtl-" + views.at(i));
        }
        books->clearFilters();
        // In the carousel and on the shelf, the next book is to the left.
        desk.backend->setView("carousel");
        QTRY_VERIFY(desk.find("carousel"));
        QTRY_COMPARE(desk.value("carousel", "currentIndex").toInt(), 0);
        desk.key(Qt::Key_H);
        QTRY_COMPARE(desk.value("carousel", "currentIndex").toInt(), 1);
        desk.key(Qt::Key_Left);
        QTRY_COMPARE(desk.value("carousel", "currentIndex").toInt(), 2);
        desk.key(Qt::Key_L);
        QTRY_COMPARE(desk.value("carousel", "currentIndex").toInt(), 1);
        desk.backend->setView("spines");
        QTRY_VERIFY(desk.find("spines"));
        QTRY_COMPARE(desk.find("shelfLoader")->property("item").value<QObject *>()->property("currentRow").toInt(), 1);
        desk.key(Qt::Key_H);
        QCOMPARE(desk.find("shelfLoader")->property("item").value<QObject *>()->property("currentRow").toInt(), 2);
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("palette", "opened").toBool());
        desk.shot("rtl-filter");
        desk.key(Qt::Key_Escape);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aThousandBooksInEveryView() {
        Desk desk;
        Library *library = desk.backend->library();
        // One transaction's worth of plain entries: the views are what is
        // being timed, not the adding.
        for (int i = 0; i < 1200; ++i) {
            BookMeta meta;
            meta.title = QString("Volume %1").arg(i + 1);
            meta.authors = QStringList{QString("Author %1").arg(i % 90)};
            meta.series = i % 3 == 0 ? QString("Series %1").arg(i % 40) : QString();
            meta.tags = QStringList{QString("Tag %1").arg(i % 12)};
            QVERIFY(library->addEntry(meta, i % 2 == 0).id > 0);
        }
        desk.backend->reload();
        QVERIFY(desk.show());
        BookListModel *books = desk.backend->books();
        QCOMPARE(books->count(), 1200);
        const QStringList views{"covers", "spines", "carousel", "list", "stacks"};
        const QStringList shows{"grid", "spines", "carousel", "list", "stacks"};
        QElapsedTimer timer;
        for (int i = 0; i < views.size(); ++i) {
            // Switching to a view, jumping to the far end of the library and
            // sorting it again each take well under a second, because a view
            // only builds the books that are in sight.
            timer.start();
            desk.backend->setView(views.at(i));
            QTRY_VERIFY(desk.find(shows.at(i)));
            QObject *shelf = desk.find("shelfLoader")->property("item").value<QObject *>();
            QMetaObject::invokeMethod(shelf, "show", Q_ARG(QVariant, 1199));
            books->setSort(i % 2 == 0 ? "title" : "author");
            desk.key(Qt::Key_L);
            desk.key(Qt::Key_J);
            QCoreApplication::processEvents();
            QVERIFY2(timer.elapsed() < 1500, qPrintable(views.at(i) + " took " + QString::number(timer.elapsed()) + " ms"));
        }
        QVERIFY(desk.find("stacks")->property("count").toInt() == 41);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theSettingsSayWhichAgentAnswersAndWhy() {
        // Automatic is Omarchy's default agent, followed while the window is open.
        Desk desk;
        QVERIFY(QDir().mkpath(desk.dir.filePath("bin")) && QDir().mkpath(desk.dir.filePath("omarchy/defaults")));
        for (const char *harness : {"codex", "claude"}) {
            const QString program = desk.dir.filePath(QString("bin/") + harness);
            QVERIFY(Fixtures::write(program, "#!/bin/bash\ncat\n"));
            QFile::setPermissions(program, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        const QByteArray path = qgetenv("PATH");
        const auto restore = qScopeGuard([&] { qputenv("PATH", path); });
        qputenv("PATH", desk.dir.filePath("bin").toUtf8());
        QVERIFY(Fixtures::write(desk.dir.filePath("omarchy/defaults/agent"), "codex\n"));
        QVERIFY(desk.show());
        desk.key(Qt::Key_Comma);
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        QTRY_COMPARE(desk.value("aiAgent", "text").toString(), QString("Omarchy's default: Codex"));
        // Switching the desktop's default switches Omabook's, with the dialog open.
        QVERIFY(Fixtures::write(desk.dir.filePath("omarchy/defaults/agent"), "crush\n"));
        QTRY_COMPARE(desk.value("aiAgent", "text").toString(),
                     QString("Omabook can't run Omarchy's default, Crush, with its tools off, so Claude Code answers."));
        desk.settings->setAiProvider("claude");
        QTRY_COMPARE(desk.value("aiAgent", "text").toString(), QString("Chosen here: Claude Code"));

        // The model picker: one model for what is read, a smaller one for
        // what Omabook reads, from the names the AI in use knows.
        QObject *quick = desk.find("quickModel"), *main = desk.find("mainModel");
        QVERIFY(quick && main);
        QCOMPARE(main->property("editText").toString(), QString("The default"));
        QCOMPARE(quick->property("editText").toString(), QString("The default (haiku)"));
        QCOMPARE(quick->property("model").toStringList(), QStringList({"The default (haiku)", "haiku", "sonnet", "opus", "fable"}));
        QVERIFY(desk.value("aiModelsNote", "text").toString().contains("much cheaper"));
        // Picked from the list.
        QVERIFY(QMetaObject::invokeMethod(quick, "activated", Q_ARG(int, 2)));
        QCOMPARE(desk.settings->aiModelFor("claude", "quick"), QString("sonnet"));
        QTRY_COMPARE(quick->property("editText").toString(), QString("sonnet"));
        // Or typed: any name the AI takes will do.
        QVERIFY(main->setProperty("editText", " claude-opus-5-5 "));
        QVERIFY(QMetaObject::invokeMethod(main, "accepted"));
        QCOMPARE(desk.settings->aiModelFor("claude", "main"), QString("claude-opus-5-5"));
        QTRY_COMPARE(main->property("editText").toString(), QString("claude-opus-5-5"));
        // Back to the default, by its first entry.
        QVERIFY(QMetaObject::invokeMethod(quick, "activated", Q_ARG(int, 0)));
        QCOMPARE(desk.settings->aiModelFor("claude", "quick"), QString());
        QTRY_COMPARE(quick->property("editText").toString(), QString("The default (haiku)"));
        // Another AI brings its own names, and nothing chosen for it yet.
        QVERIFY(Fixtures::write(desk.dir.filePath("omarchy/defaults/agent"), "codex\n"));
        desk.settings->setAiProvider("auto");
        QTRY_COMPARE(quick->property("editText").toString(), QString("The default (gpt-6-luna)"));
        QCOMPARE(main->property("editText").toString(), QString("The default"));
        desk.shot("settings-models");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aLightThemeAndARightToLeftLanguageLoadCleanly() {
        Desk desk({}, "ar", "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n");
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QCOMPARE(desk.window->color(), QColor("#fafafa"));
        QVERIFY(desk.window->property("rightToLeft").toBool());
        // Every dialog, opened once.
        desk.key(Qt::Key_Comma);
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        desk.shot("light-settings");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("settings", "visible").toBool());
        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("help", "visible").toBool());
        desk.key(Qt::Key_E);
        QTRY_VERIFY(desk.value("edit", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("edit", "visible").toBool());
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("flowText"));
        QCOMPARE(desk.find("flowText")->property("color").value<QColor>(), QColor("#202020"));
        desk.shot("light-reader");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aBookWithoutAFileIsAddedAndKept() {
        Desk desk;
        QVERIFY(desk.show());
        // B opens the dialog on an empty book; it is on a real shelf unless said otherwise.
        desk.key(Qt::Key_B);
        QTRY_VERIFY(desk.value("edit", "opened").toBool());
        QTRY_VERIFY(desk.value("editTitle", "activeFocus").toBool());
        QVERIFY(desk.value("editHeading", "visible").toBool());
        // Without a title nothing is added.
        QMetaObject::invokeMethod(desk.find("editSave"), "clicked");
        QVERIFY(desk.backend->library()->books().isEmpty());
        desk.type("A Paper Book");
        desk.find("editShelf")->setProperty("currentIndex", 3);
        desk.find("editStars")->setProperty("value", 4);
        QMetaObject::invokeMethod(desk.find("editSave"), "clicked");
        QTRY_COMPARE(desk.backend->library()->books().size(), 1);
        const Book book = desk.backend->library()->books().first();
        QCOMPARE(book.meta.title, QString("A Paper Book"));
        QCOMPARE(book.held(), QString("paper"));
        QCOMPARE(book.status(), QString("read"));
        QCOMPARE(book.rating, 4);

        // It has nothing to open: the window says so and stays on the shelf.
        desk.reader->openBook(book.id);
        QVERIFY(!desk.reader->isOpen());
        QVERIFY(desk.backend->property("message").toString().contains("on your shelf"));

        // Editing it shows what was said, and changes it.
        QMetaObject::invokeMethod(desk.window, "editBook", Q_ARG(QVariant, book.id));
        QTRY_VERIFY(desk.value("edit", "opened").toBool());
        QCOMPARE(desk.value("editShelf", "currentIndex").toInt(), 3);
        QCOMPARE(desk.value("editStars", "value").toInt(), 4);
        QVERIFY(desk.value("editPaper", "checked").toBool());
        desk.find("editPaper")->setProperty("checked", false);
        QMetaObject::invokeMethod(desk.find("editSave"), "clicked");
        QTRY_COMPARE(desk.backend->library()->book(book.id)->held(), QString("none"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aCalibreLibraryFoundOnTheComputerIsOffered() {
        // The desk has no Calibre; the Calibre plugin then takes Calibre's
        // settings folder from the environment, so this is a temp one too.
        QTemporaryDir calibre;
        const QString shelf = calibre.filePath("Shelf");
        QVERIFY(Fixtures::calibreShelf(shelf));
        QVERIFY(Fixtures::calibreConfig(calibre.filePath("config"), {shelf}));
        qputenv("CALIBRE_CONFIG_DIRECTORY", calibre.filePath("config").toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("CALIBRE_CONFIG_DIRECTORY"); });
        Desk desk;
        QVERIFY(desk.show());

        // A quiet line at the bottom names the library and its size; the
        // shelf behind it works as before.
        QTRY_VERIFY(desk.value("calibreOffer", "visible").toBool());
        const QString words = desk.value("calibreOfferText", "text").toString();
        QVERIFY2(words.contains("Shelf") && words.contains("6 books"), qPrintable(words));
        desk.shot("calibre-offer");
        QVERIFY(!desk.window->property("dialogOpen").toBool());

        // "Not now" takes it away, and is remembered.
        QMetaObject::invokeMethod(desk.find("calibreOfferLater"), "clicked");
        QTRY_VERIFY(!desk.value("calibreOffer", "visible").toBool());
        QCOMPARE(desk.settings->calibreLibraries().value(shelf).toObject().value("answer").toString(), QString("later"));

        // Going looking from the libraries popup offers it again, and
        // "Import" brings it in with what Calibre knew of the reading.
        QMetaObject::invokeMethod(desk.window, "showLibraries");
        QTRY_VERIFY(desk.value("libraries", "opened").toBool());
        QMetaObject::invokeMethod(desk.find("findCalibre"), "clicked");
        QTRY_VERIFY(desk.value("calibreOffer", "visible").toBool());
        QMetaObject::invokeMethod(desk.find("calibreOfferImport"), "clicked");
        QTRY_VERIFY(!desk.value("calibreOffer", "visible").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(int(desk.backend->library()->books().size()), 5, 20000);
        QTRY_VERIFY(desk.value("toastText", "text").toString().contains("2 are read"));
        QVERIFY(desk.value("toastText", "text").toString().contains("2 are being read"));
        QCOMPARE(desk.settings->calibreLibraries().value(shelf).toObject().value("answer").toString(), QString("imported"));
        QVERIFY(!desk.value("calibreOffer", "visible").toBool());

        // Switched off, the popup says so instead of listing Calibre's libraries.
        desk.backend->setPluginEnabled("calibre", false);
        QTRY_VERIFY(desk.backend->calibreSwitchedOff());
        QVERIFY(desk.backend->calibreLibraries().isEmpty());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void readingSitesComeInAndSuggestionsAreChecked() {
        // A reading site that needs no network: somebody with one shelf of
        // two books, a search that knows one title, and a memory of what it
        // was sent.
        struct StubSite : ReadingSite {
            StubSite(const QString &scheme, QList<ReadingSites::Outgoing> *sent, QObject *parent)
                : ReadingSite(parent), m_scheme(scheme), m_sent(sent) {}
            QString m_scheme, m_secret;
            QList<ReadingSites::Outgoing> *m_sent;
            QString name() const override { return m_scheme == "hardcover" ? "Hardcover" : m_scheme == "openlibrary" ? "Open Library" : "Goodreads"; }
            QString scheme() const override { return m_scheme; }
            QString account() const override { return "ann"; }
            QString identityFrom(const QString &pasted) const override { return pasted.contains(' ') ? QString() : pasted.trimmed().section('/', -1); }
            SecretUse secretUse() const override { return m_scheme == "hardcover" ? NeedsSecret : OptionalSecret; }
            void setSecret(const QString &secret) override { m_secret = secret; }
            QUrl findMePage() const override { return QUrl("https://example.org/" + m_scheme + "/me"); }
            bool writes() const override { return m_scheme != "goodreads" && !m_secret.isEmpty(); }
            void check() override {
                SiteProfile profile;
                profile.identity = "ann";
                profile.name = "Ann Reader";
                profile.books = 2;
                profile.shelves = 2;
                // Goodreads shows who somebody is, and not their shelves.
                profile.shelvesReadable = m_scheme != "goodreads";
                QMetaObject::invokeMethod(this, [this, profile] { emit checked(profile); }, Qt::QueuedConnection);
            }
            void fetchShelves() override {
                SiteEntry read, wanted;
                read.siteId = "1";
                read.title = "The Test Book";
                read.authors = QStringList{"Ann Author"};
                read.shelf = "read";
                read.rating = 4;
                wanted.siteId = "2";
                wanted.title = "One Day";
                wanted.authors = QStringList{"Dee Poet"};
                wanted.shelf = "to-read";
                QMetaObject::invokeMethod(this, [this, read, wanted] { emit fetched({read, wanted}); }, Qt::QueuedConnection);
            }
            void write(const QList<ReadingSites::Outgoing> &changes) override {
                QList<SiteWritten> results;
                for (const ReadingSites::Outgoing &change : changes) {
                    *m_sent << change;
                    results.append({change.bookId, change.siteId, {}});
                }
                QMetaObject::invokeMethod(this, [this, results] { emit wrote(results); }, Qt::QueuedConnection);
            }
            void find(const QString &title, const QString &) override {
                SiteBook book;
                book.site = "Stub";
                book.scheme = "stub";
                book.siteId = "9";
                book.title = "The Long Way Home";
                book.authors = QStringList{"Ann Author"};
                book.averageRating = 4.21;
                book.ratings = 1200;
                emit found(title == book.title ? QList<SiteBook>{book} : QList<SiteBook>{});
            }
        };

        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QList<ReadingSites::Outgoing> sent;
        SessionSecrets secrets;
        QList<QUrl> opened;
        SiteDesk *sites = desk.librarian->siteDesk();
        sites->setSecrets(&secrets);
        sites->setOpener([&](const QUrl &url) { opened << url; });
        QVERIFY(QDir(desk.dir.path()).mkpath("Downloads"));
        sites->setDownloads(desk.dir.filePath("Downloads"));
        sites->setMaker([&](const QString &scheme, const QString &, const QString &secret, QObject *parent) -> ReadingSite * {
            auto *site = new StubSite(scheme, &sent, parent);
            site->setSecret(secret);
            return site;
        });
        desk.librarian->setSites([&](QObject *parent) { return QList<ReadingSite *>{new StubSite("stub", &sent, parent)}; });
        QVERIFY(desk.show());
        auto text = [&](const QString &name) { return desk.value(name, "text").toString(); };
        auto click = [&](const QString &name) { return QMetaObject::invokeMethod(desk.find(name), "clicked"); };

        // The settings lead to the reading sites, and so does R. Every site
        // is a row that says how things stand; the first one that isn't
        // connected is open, with the keyboard on its row.
        desk.key(Qt::Key_Comma);
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        click("openReadingSites");
        QTRY_VERIFY(desk.value("readingSites", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("readingSites", "visible").toBool());
        desk.key(Qt::Key_R);
        QTRY_VERIFY(desk.value("readingSites", "opened").toBool());
        for (const char *row : {"goodreadsState", "openlibraryState", "hardcoverState"})
            QCOMPARE(text(row), QString("Not connected"));
        QCOMPARE(desk.value("readingSites", "current").toString(), QString("goodreads"));
        QTRY_VERIFY(desk.value("goodreadsRow", "activeFocus").toBool());
        QVERIFY(!desk.value("goodreadsSync", "visible").toBool());

        // Whatever somebody has of themselves is taken and looked at at
        // once. Goodreads shows who they are but not their shelves, which
        // the row says, with the guide to the file under it.
        desk.find("goodreadsIdentity")->setProperty("text", "https://www.goodreads.com/user/show/ann");
        click("goodreadsConnect");
        QTRY_COMPARE(text("goodreadsState"), QString("Needs attention · 2 books on 2 shelves"));
        QVERIFY(text("goodreadsWhy").contains("Ann Reader's profile (2 books)"));
        QCOMPARE(text("goodreadsIdentity"), QString("ann"));
        QVERIFY(desk.value("goodreadsGuide", "visible").toBool());
        // "Find me" and the export page open in the browser.
        click("goodreadsFindMe");
        click("goodreadsOpenExport");
        QCOMPARE(opened, (QList<QUrl>{QUrl("https://example.org/goodreads/me"), QUrl("https://www.goodreads.com/review/import")}));
        desk.shot("reading-sites");

        // The download is seen arriving in the Downloads folder and
        // offered; nothing is brought in until the click.
        QVERIFY(!desk.value("goodreadsOffer", "visible").toBool());
        QVERIFY(write(desk.dir.filePath("Downloads/goodreads_library_export.csv"),
                      "Book Id,Title,Author,Additional Authors,ISBN,ISBN13,My Rating,Binding,Date Read,Exclusive Shelf,Owned Copies\n"
                      "501,The Test Book,Ann Author,,=\"\",=\"\",5,Paperback,2024/03/09,read,0\n"
                      "502,Quiet Rivers,Dee Poet,,=\"\",=\"\",0,Paperback,,to-read,1\n"));
        QTRY_VERIFY(desk.value("goodreadsOffer", "visible").toBool());
        QCOMPARE(text("goodreadsOffer"), QString("goodreads_library_export.csv is in your Downloads folder."));
        Library *library = desk.backend->library();
        QCOMPARE(library->books().size(), 1);
        desk.shot("reading-sites-offer");
        click("goodreadsTakeOffer");
        // The book that is here is updated, and the other, of which a copy
        // is owned, is added as a paper book. Goodreads is connected now.
        QTRY_VERIFY(text("goodreadsStatus").contains("1 new, 1 updated"));
        QVERIFY(!sites->working());
        QVERIFY(!desk.value("goodreadsOffer", "visible").toBool());
        QCOMPARE(library->book(id)->shelf, QString("read"));
        QCOMPARE(library->book(id)->rating, 5);
        QCOMPARE(library->books("is:paper status:to-read").size(), 1);
        QTRY_COMPARE(desk.value("grid", "count").toInt(), 2);
        QCOMPARE(text("goodreadsState"), QString("Connected as Ann Reader · 2 books on 2 shelves · Last brought in today"));
        // The same file through the file dialog changes nothing more.
        desk.picker.next = {QUrl::fromLocalFile(desk.dir.filePath("Downloads/goodreads_library_export.csv"))};
        click("goodreadsImport");
        QCOMPARE(desk.picker.asked.last(), FilePicker::ImportReading);
        QTRY_VERIFY(text("goodreadsStatus").contains("0 new, 0 updated, 2 unchanged"));
        // And the way back is a file for Goodreads' own import page.
        desk.picker.next = {QUrl::fromLocalFile(desk.dir.filePath("for-goodreads.csv"))};
        click("goodreadsExport");
        QCOMPARE(desk.picker.asked.last(), FilePicker::ExportReading);
        QTRY_VERIFY(text("goodreadsStatus").contains("goodreads.com/review/import"));
        QFile written(desk.dir.filePath("for-goodreads.csv"));
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(written.readAll().contains("501,The Test Book,Ann Author"));

        // Down goes to the next row, and Enter opens it. A site that wants
        // a token says so; with one it is connected, and says as whom.
        desk.find("goodreadsRow")->setProperty("focus", true);
        QMetaObject::invokeMethod(desk.find("goodreadsRow"), "forceActiveFocus");
        desk.key(Qt::Key_Down);
        QTRY_VERIFY(desk.value("openlibraryRow", "activeFocus").toBool());
        desk.key(Qt::Key_J);
        QTRY_VERIFY(desk.value("hardcoverRow", "activeFocus").toBool());
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.value("readingSites", "current").toString(), QString("hardcover"));
        click("hardcoverConnect");
        QTRY_VERIFY(text("hardcoverStatus").contains("needs a token"));
        QCOMPARE(text("hardcoverState"), QString("Not connected"));
        desk.find("hardcoverSecret")->setProperty("text", "hc_pat_SECRET_TOKEN_3");
        click("hardcoverConnect");
        QTRY_COMPARE(text("hardcoverState"), QString("Connected as Ann Reader · 2 books on 2 shelves"));
        // The token is in the keyring; the field doesn't keep it, and the
        // settings never saw it.
        QCOMPARE(secrets.get("hardcover"), QString("hc_pat_SECRET_TOKEN_3"));
        QCOMPARE(text("hardcoverSecret"), QString());
        QFile config(desk.dir.filePath("config.json"));
        QVERIFY(config.open(QIODevice::ReadOnly));
        QVERIFY(!config.readAll().contains("SECRET_TOKEN"));

        // Sync brings its shelves in and never clobbers what is here: the
        // stars the file set stay, and are not sent either while that is off.
        click("hardcoverSync");
        QTRY_VERIFY(text("hardcoverStatus").contains("1 new, 1 updated"));
        QCOMPARE(library->book(id)->rating, 5);
        QCOMPARE(library->books().size(), 3);
        QVERIFY(sent.isEmpty());
        // "Also update Hardcover": the first time, what would be sent is
        // shown and waits for a click.
        QVERIFY(!desk.value("hardcoverWrite", "checked").toBool());
        sites->setWrite("hardcover", true);
        QTRY_VERIFY(desk.value("hardcoverWrite", "checked").toBool());
        QVERIFY(library->setReading(id, "read", 2, "2024-03-09"));
        click("hardcoverSync");
        // The day it was finished is news to Hardcover, and so is the book
        // the Goodreads file brought. The stars differ on both sides and
        // nobody can say which are newer: they stay as they are, unsent.
        QTRY_COMPARE(text("hardcoverPending"), QString("The Test Book: finished 2024-03-09\nQuiet Rivers: add: shelf to-read"));
        QVERIFY(sent.isEmpty());
        desk.shot("reading-sites-send");
        click("hardcoverSend");
        QTRY_COMPARE(sent.size(), 2);
        QCOMPARE(sent.first().finished, QString("2024-03-09"));
        QCOMPARE(sent.first().rating, 0);
        QVERIFY(sent.last().isNew);
        QTRY_VERIFY(text("hardcoverStatus").contains("sent to Hardcover"));

        // Disconnecting forgets who and the token, and leaves every book.
        click("hardcoverDisconnect");
        QTRY_COMPARE(text("hardcoverState"), QString("Not connected"));
        QCOMPARE(secrets.get("hardcover"), QString());
        QCOMPARE(library->books().size(), 3);
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("readingSites", "visible").toBool());

        // N asks what to read next. The model sees the reading that came
        // from the sites, and each book it names is looked up before it is shown.
        desk.ai.answer = "{\"suggestions\": [{\"title\": \"The Long Way Home\", \"author\": \"Ann Author\", \"why\": \"You like roads.\"}, "
                         "{\"title\": \"A Book Never Written\", \"author\": \"Nobody\", \"why\": \"Made up.\"}], "
                         "\"first\": \"Start with One Day.\"}";
        desk.key(Qt::Key_N);
        QTRY_VERIFY(desk.value("askLibrary", "opened").toBool());
        QTRY_COMPARE(desk.librarian->suggestions().size(), 2);
        const QString asked = desk.ai.requests.last().messages.first().text;
        QVERIFY(asked.contains("\"stars\":2") && asked.contains("Quiet Rivers — Dee Poet") && asked.contains("One Day — Dee Poet"));
        QTRY_COMPARE(desk.librarian->answer(), QString("Start with One Day."));
        QTRY_COMPARE(desk.value("askLibrarySuggestions", "count").toInt(), 2);
        QVariantMap first = desk.librarian->suggestions().at(0).toMap(), second = desk.librarian->suggestions().at(1).toMap();
        QCOMPARE(first.value("state").toString(), QString("found"));
        QCOMPARE(first.value("rating").toString(), QString("4.21 on Stub"));
        QVERIFY(first.value("canWant").toBool());
        // The one no site knows is marked, and can't be wished for.
        QCOMPARE(second.value("state").toString(), QString("missing"));
        QVERIFY(!second.value("canWant").toBool());
        QTRY_VERIFY(desk.find("askLibraryWant0"));
        desk.shot("suggestions");

        // "Add to to-read" makes it a book that is only known of.
        QMetaObject::invokeMethod(desk.find("askLibraryWant0"), "clicked");
        QTRY_COMPARE(library->books("is:wishlist status:to-read title:Long").size(), 1);
        QCOMPARE(library->bookWithIdentifier("stub", "9"), library->books("title:Long").first().id);
        first = desk.librarian->suggestions().at(0).toMap();
        QVERIFY(!first.value("canWant").toBool());
        QCOMPARE(first.value("note").toString(), QString("On your to-read list"));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("askLibrary", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theHelpIsMadeFromTheCommandsAndSearched() {
        // Nothing in the help is a list written by hand: it shows the keys
        // keys.json gives, a plugin's command beside the built-in ones, and
        // typing finds a command that Enter then runs.
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(QDir(desk.dir.path()).mkpath("custom"));
        write(desk.dir.filePath("custom/keys.json"), "{\"library\": {\"R\": \"read\"}}");
        QVERIFY(QDir(desk.dir.path()).mkpath("plugins/shelver"));
        write(desk.dir.filePath("plugins/shelver/main.js"),
              "omabook.commands.register({id: \"shelve\", title: \"Shelve it\", place: \"library\", key: \"Ctrl+R\",\n"
              "    run: function (context) { omabook.say(\"Shelved\"); }});\n");
        write(desk.dir.filePath("plugins/shelver/plugin.json"),
              "{\"name\": \"Shelver\", \"version\": \"1.0\", \"script\": \"main.js\", \"permissions\": []}");
        QTRY_COMPARE(desk.custom->keyText().value("library.read").toString(), QString("Space  R"));
        QTRY_COMPARE(desk.custom->keyText().value("library.shelver:shelve").toString(), QString("Ctrl+R"));
        QVERIFY(desk.show());

        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        QTRY_VERIFY(desk.value("helpField", "activeFocus").toBool());
        QCOMPARE(desk.value("help", "page").toString(), QString("keys"));
        QVERIFY(desk.find("helpRow-library.read"));
        QCOMPARE(desk.value("helpRow-library.read", "keys").toString(), QString("Space  R"));
        QCOMPARE(desk.value("helpRow-library.shelver:shelve", "keys").toString(), QString("Ctrl+R"));
        // On the shelf, the shelf's keys lead.
        QCOMPARE(desk.value("help", "rows").toList().at(1).toMap().value("id").toString(), QString("library.read"));
        desk.shot("help");

        // Typing searches, in the words a person would use, and never runs anything.
        desk.type("how do i open the settings");
        QTRY_VERIFY(desk.find("helpRow-global.settings"));
        QVERIFY(!desk.find("helpRow-library.read"));
        QVERIFY(desk.value("help", "opened").toBool());
        QVERIFY(!desk.value("settings", "opened").toBool());
        desk.shot("help-search");
        // Enter runs what is selected, once the help is out of the way.
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        QVERIFY(!desk.value("help", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("settings", "visible").toBool());

        // F1 opens it too. A key typed as itself finds its command; one
        // that belongs to the reader says so instead of running.
        desk.key(Qt::Key_F1);
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        QCOMPARE(desk.value("helpField", "text").toString(), QString());
        desk.type("bookmark");
        QTRY_VERIFY(desk.find("helpRow-reader.bookmark"));
        desk.key(Qt::Key_Return);
        QVERIFY(desk.value("help", "opened").toBool());
        QCOMPARE(desk.value("helpNote", "text").toString(), QString("That one works while a book is open."));
        // Escape empties the search first, and ? is a character while there is one.
        QTest::keyClick(desk.window, '?');
        QVERIFY(desk.value("help", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QCOMPARE(desk.value("helpField", "text").toString(), QString());
        QVERIFY(desk.value("help", "opened").toBool());

        // Tab turns to the pages; a page lists the commands it names.
        desk.key(Qt::Key_Tab);
        QCOMPARE(desk.value("help", "page").toString(), QString("start"));
        QVERIFY(desk.find("helpRow-library.add"));
        QVERIFY(desk.find("helpLink-tour"));
        desk.shot("help-topic");
        desk.key(Qt::Key_Backtab);
        QCOMPARE(desk.value("help", "page").toString(), QString("keys"));
        // The key that opened it closes it.
        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(!desk.value("help", "visible").toBool());

        // Over a book, reading leads, and a search runs in the reader.
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("readerView"));
        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        QVERIFY(desk.value("help", "reading").toBool());
        QCOMPARE(desk.value("help", "rows").toList().at(1).toMap().value("place").toString(), QString("reader"));
        QCOMPARE(desk.value("help", "pages").toList().at(1).toMap().value("id").toString(), QString("reading"));
        desk.shot("help-reading");
        desk.type("bookmark");
        QTRY_VERIFY(desk.find("helpRow-reader.bookmark"));
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.value("toastText", "text").toString(), QString("Bookmarked"));
        QTRY_VERIFY(!desk.value("help", "visible").toBool());
        QCOMPARE(desk.reader->bookmarks().size(), 1);
        desk.key(Qt::Key_F1);
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        desk.key(Qt::Key_F1);
        QTRY_VERIFY(!desk.value("help", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theHelpFitsASmallWindowInEveryTheme() {
        // The old help was two fixed columns that ran off a small window.
        // This one is never larger than the window, scrolls what doesn't
        // fit, and puts its pages along the top when there is no room beside.
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                 "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";
        struct Case {
            QByteArray language, colors;
            QSize size;
            const char *shot;
        };
        for (const Case &look : {Case{"en", {}, QSize(640, 480), "help-small"}, Case{"en", {}, QSize(1180, 780), "help-full"},
                                 Case{"en", light, QSize(1180, 780), "help-light"},
                                 Case{"ar", light, QSize(640, 480), "help-rtl"}, Case{"nl", {}, QSize(720, 480), "help-nl"}}) {
            Desk desk({}, look.language, look.colors);
            QVERIFY(desk.show());
            QCOMPARE(stock(desk).size(), 28);
            desk.window->setMinimumSize(QSize(320, 240));
            desk.window->resize(look.size);
            QTRY_COMPARE(desk.window->size(), look.size);
            QTest::keyClick(desk.window, '?');
            QTRY_VERIFY(desk.value("help", "opened").toBool());
            QQuickItem *page = qobject_cast<QQuickItem *>(desk.find("helpPage"));
            QQuickItem *pages = qobject_cast<QQuickItem *>(desk.find("helpPages"));
            QVERIFY(page && pages);
            const auto fits = [&] {
                // The popup is inside the window, and nothing in it is wider
                // than the room it has: what is long scrolls down, not across.
                const QRectF window(0, 0, desk.window->width(), desk.window->height());
                for (QQuickItem *item : {page, pages})
                    if (!window.contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))))
                        return false;
                return desk.value("help", "width").toReal() <= desk.window->width()
                       && desk.value("help", "height").toReal() <= desk.window->height()
                       && page->property("contentWidth").toReal() <= page->width() + 0.5;
            };
            QTRY_VERIFY(fits());
            QCOMPARE(desk.value("help", "wide").toBool(), look.size.width() > 700);
            // The keys are more than fit, so they scroll; Down walks them.
            QVERIFY(page->property("contentHeight").toReal() > page->height());
            for (int i = 0; i < 30; ++i)
                desk.key(Qt::Key_Down);
            QVERIFY(page->property("contentY").toReal() > 0);
            QVERIFY(desk.value("help", "current").toInt() > 20);
            desk.key(Qt::Key_PageUp);
            desk.key(Qt::Key_PageUp);
            desk.key(Qt::Key_PageUp);
            desk.key(Qt::Key_PageUp);
            desk.shot(look.shot);
            // Every page fits too, read with the same keys.
            const int count = desk.value("help", "pages").toList().size();
            QCOMPARE(count, 10);
            for (int i = 1; i < count; ++i) {
                desk.key(Qt::Key_Tab);
                QVERIFY(fits());
                for (int press = 0; press < 12; ++press)
                    desk.key(Qt::Key_Down);
                if (i == 3)
                    desk.shot(QString(look.shot) + "-page");
            }
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("help", "visible").toBool());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    void theTourIsOfferedOnceAndWalksAnEmptyLibrary() {
        // A first start: no books, and nothing remembered. The tour is
        // offered in a note that takes no focus, runs from the keyboard, and
        // is not offered again whichever way it ends.
        {
            Desk desk;
            desk.settings->setTour("");
            QVERIFY(desk.manual->tourOffered());
            QVERIFY(desk.show());
            QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
            QVERIFY(!desk.value("tour", "running").toBool());
            // The shelf has the keyboard, not the note: nothing was interrupted.
            QVERIFY(desk.value("emptyState", "visible").toBool());
            QVERIFY(desk.find("emptyPaper") && desk.find("emptyFolder"));
            desk.shot("tour-offer");

            // Taken from the help, by keyboard alone.
            QTest::keyClick(desk.window, '?');
            QTRY_VERIFY(desk.value("help", "opened").toBool());
            desk.type("tour");
            QTRY_VERIFY(desk.find("helpLink-tour"));
            desk.key(Qt::Key_Return);
            QTRY_VERIFY(desk.value("tour", "running").toBool());
            QTRY_VERIFY(desk.value("tour", "activeFocus").toBool());
            QVERIFY(!desk.value("tourNote", "visible").toBool());
            const int steps = desk.value("tour", "steps").toList().size();
            QStringList seen;
            for (const QVariant &step : desk.value("tour", "steps").toList())
                seen << step.toMap().value("id").toString();
            // With no books it leads with how to get some, and skips what only a book has.
            QCOMPARE(seen, (QStringList{"welcome", "add", "search", "views", "filter", "sites", "companion", "end"}));
            QCOMPARE(desk.value("tourTitle", "text").toString(), QString("Welcome to Omabook"));
            // The keys of the shelf are off while it runs.
            desk.key(Qt::Key_Comma);
            QVERIFY(!desk.value("settings", "opened").toBool());
            desk.key(Qt::Key_Right);
            QCOMPARE(desk.value("tour", "at").toInt(), 1);
            // It points at the real thing, with the card clear of it and inside the window.
            QQuickItem *spot = qobject_cast<QQuickItem *>(desk.find("tourSpot"));
            QQuickItem *card = qobject_cast<QQuickItem *>(desk.find("tourCard"));
            QQuickItem *empty = qobject_cast<QQuickItem *>(desk.find("emptyState"));
            QVERIFY(spot && card && empty);
            QTRY_VERIFY(spot->isVisible());
            QTRY_VERIFY(std::abs(spot->width() - (empty->width() + 12)) < 1);
            QTest::qWait(300);
            QVERIFY(!QRectF(card->x(), card->y(), card->width(), card->height())
                         .intersects(QRectF(spot->x(), spot->y(), spot->width(), spot->height())));
            QVERIFY(card->x() >= 0 && card->y() >= 0 && card->x() + card->width() <= desk.window->width()
                    && card->y() + card->height() <= desk.window->height());
            QVERIFY(desk.value("tourText", "text").toString().contains("<b>Shift+A</b>"));
            desk.shot("tour-empty");
            desk.key(Qt::Key_Left);
            QCOMPARE(desk.value("tour", "at").toInt(), 0);
            for (int i = 0; i < steps - 1; ++i)
                desk.key(i % 2 ? Qt::Key_Space : Qt::Key_Return);
            QCOMPARE(desk.value("tour", "at").toInt(), steps - 1);
            QVERIFY(desk.value("tour", "running").toBool());
            desk.key(Qt::Key_Return);
            QVERIFY(!desk.value("tour", "running").toBool());
            // Remembered, and the shelf has its keys back.
            QCOMPARE(desk.settings->tour(), QString("library"));
            QVERIFY(!desk.manual->tourOffered());
            QVERIFY(!desk.value("tourNote", "visible").toBool());
            desk.key(Qt::Key_Comma);
            QTRY_VERIFY(desk.value("settings", "opened").toBool());
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("settings", "visible").toBool());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        {
            // Escape leaves at any step, and that is an answer too.
            Desk desk;
            desk.settings->setTour("");
            QVERIFY(desk.show());
            QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
            QMetaObject::invokeMethod(desk.find("tourNoteAccept"), "clicked");
            QTRY_VERIFY(desk.value("tour", "running").toBool());
            desk.key(Qt::Key_Right);
            desk.key(Qt::Key_Right);
            desk.key(Qt::Key_Escape);
            QVERIFY(!desk.value("tour", "running").toBool());
            QCOMPARE(desk.settings->tour(), QString("done"));
            QVERIFY(!desk.value("tourNote", "visible").toBool());
            // Taking it again from the help never brings the offer back.
            QVERIFY(QMetaObject::invokeMethod(desk.window, "startTour", Q_ARG(QVariant, "tour")));
            QTRY_VERIFY(desk.value("tour", "running").toBool());
            for (int i = 0; i < 12 && desk.value("tour", "running").toBool(); ++i)
                desk.key(Qt::Key_Right);
            QVERIFY(!desk.value("tour", "running").toBool());
            QCOMPARE(desk.settings->tour(), QString("done"));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        {
            // "No thanks" is the end of it as well.
            Desk desk;
            desk.settings->setTour("");
            QVERIFY(desk.show());
            QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
            QMetaObject::invokeMethod(desk.find("tourNoteDecline"), "clicked");
            QTRY_VERIFY(!desk.value("tourNote", "visible").toBool());
            QCOMPARE(desk.settings->tour(), QString("done"));
            QCOMPARE(desk.settings->newsSeen(), QString(OMABOOK_VERSION));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    void theTourWalksAFullLibraryAndTheReaderByTap() {
        // With books it points at one, and at its details; taken to the end
        // it is offered once more in the first book, for the reader. All of
        // it with a finger, in a light theme, then mirrored.
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                 "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";
        QPointingDevice *finger = QTest::createTouchDevice();
        for (const QByteArray &language : {QByteArray("en"), QByteArray("ar")}) {
            Desk desk({}, language, language == "en" ? QByteArray() : light);
            desk.settings->setTour("");
            QVERIFY(desk.show());
            const QList<qint64> ids = stock(desk);
            QCOMPARE(ids.size(), 28);
            auto tap = [&](const QString &name) {
                QQuickItem *item = qobject_cast<QQuickItem *>(desk.find(name));
                QVERIFY2(item && item->isVisible(), qPrintable(name));
                const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
                QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
                QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
            };
            QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
            // A first touch anywhere brings out the buttons fingers need.
            tap("bookCount");
            QTRY_VERIFY(desk.window->property("touch").toBool());
            QVERIFY(!desk.value("filterLabel", "visible").toBool());
            tap("tourNoteAccept");
            QTRY_VERIFY(desk.value("tour", "running").toBool());
            QStringList seen;
            for (const QVariant &step : desk.value("tour", "steps").toList())
                seen << step.toMap().value("id").toString();
            // A finger has no "Filter" label but a button, and the tour points at that.
            QCOMPARE(seen, (QStringList{"welcome", "book", "details", "search", "views", "filter", "sites", "companion", "end"}));
            QQuickItem *spot = qobject_cast<QQuickItem *>(desk.find("tourSpot"));
            QQuickItem *card = qobject_cast<QQuickItem *>(desk.find("tourCard"));
            const QRectF window(0, 0, desk.window->width(), desk.window->height());
            for (int i = 0; i < seen.size(); ++i) {
                QCOMPARE(desk.value("tour", "at").toInt(), i);
                QTest::qWait(280);
                const bool points = !desk.value("tour", "steps").toList().at(i).toMap().value("spot").toString().isEmpty();
                QCOMPARE(spot->isVisible(), points);
                const QRectF about(card->x(), card->y(), card->width(), card->height());
                QVERIFY2(window.contains(about), qPrintable(seen.at(i)));
                if (points)
                    QVERIFY2(!about.intersects(QRectF(spot->x(), spot->y(), spot->width(), spot->height())), qPrintable(seen.at(i)));
                if (seen.at(i) == "book" || seen.at(i) == "details" || seen.at(i) == "views")
                    desk.shot(QString("tour-%1-%2").arg(seen.at(i), QString::fromLatin1(language)));
                if (i == 2) {
                    // Back is a tap too.
                    tap("tourBack");
                    QCOMPARE(desk.value("tour", "at").toInt(), 1);
                    tap("tourNext");
                }
                tap("tourNext");
            }
            QVERIFY(!desk.value("tour", "running").toBool());
            QCOMPARE(desk.settings->tour(), QString("library"));
            QVERIFY(!desk.value("tourNote", "visible").toBool());

            // The first book: the reader's half is offered there, once.
            qint64 book = 0;
            for (const qint64 id : ids)
                if (book == 0 && !desk.backend->library()->book(id)->formats.isEmpty())
                    book = id;
            desk.reader->openBook(book);
            QTRY_VERIFY(desk.reader->isOpen());
            QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
            QVERIFY(desk.value("tourNoteText", "text").toString() != QString());
            tap("tourNoteAccept");
            QTRY_VERIFY(desk.value("tour", "running").toBool());
            seen.clear();
            for (const QVariant &step : desk.value("tour", "steps").toList())
                seen << step.toMap().value("id").toString();
            QVERIFY2(seen == (QStringList{"page", "place", "contents", "assistant", "fingers", "leave"}), qPrintable(seen.join(' ')));
            for (int i = 0; i < seen.size(); ++i) {
                QTest::qWait(280);
                // The panes it talks about are opened to be pointed at, and closed after.
                QCOMPARE(desk.value("assistantPane", "visible").toBool(), seen.at(i) == "assistant");
                QCOMPARE(desk.value("contents", "visible").toBool(), seen.at(i) == "contents");
                if (i > 0 && i < seen.size() - 1) {
                    QVERIFY2(spot->isVisible(), qPrintable(seen.at(i)));
                    const QRectF about(card->x(), card->y(), card->width(), card->height());
                    QVERIFY2(window.contains(about), qPrintable(seen.at(i)));
                    QVERIFY2(!about.intersects(QRectF(spot->x(), spot->y(), spot->width(), spot->height())), qPrintable(seen.at(i)));
                }
                if (seen.at(i) == "assistant")
                    desk.shot("tour-reader-" + QString::fromLatin1(language));
                tap("tourNext");
            }
            QVERIFY(!desk.value("tour", "running").toBool());
            QVERIFY(!desk.value("assistantPane", "visible").toBool());
            QCOMPARE(desk.settings->tour(), QString("done"));
            QVERIFY(!desk.value("tourNote", "visible").toBool());
            // The page has the keyboard again.
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.reader->isOpen());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    void whatIsNewIsOfferedOnlyWhenTheVersionMoved() {
        // The same version as last time: nothing. A newer one with features
        // to show: a quiet note, never a dialog, and a tour of just those.
        Desk desk;
        QVERIFY(!desk.manual->newsOffered());
        QVERIFY(desk.show());
        QVERIFY(!desk.value("tourNote", "visible").toBool());
        desk.settings->setNewsSeen("0.0.9");
        QVERIFY(desk.manual->newsOffered());
        QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
        QVERIFY(desk.value("tourNoteText", "text").toString().contains(OMABOOK_VERSION));
        QVERIFY(!desk.value("tour", "running").toBool());
        QVERIFY(!desk.window->property("dialogOpen").toBool());
        QMetaObject::invokeMethod(desk.find("tourNoteAccept"), "clicked");
        QTRY_VERIFY(desk.value("tour", "running").toBool());
        const QVariantList steps = desk.value("tour", "steps").toList();
        QCOMPARE(steps.size(), Manual::features().size());
        // The first is about the views, and points at them; one that
        // belongs to the reader is said without pointing.
        QCOMPARE(steps.first().toMap().value("id").toString(), QString("views"));
        QTRY_VERIFY(desk.value("tourSpot", "visible").toBool());
        QTest::qWait(280);
        desk.shot("whats-new");
        for (int i = 0; i < 3; ++i)
            desk.key(Qt::Key_Right);
        QCOMPARE(steps.at(3).toMap().value("id").toString(), QString("companion"));
        QTest::qWait(280);
        QVERIFY(!desk.value("tourSpot", "visible").toBool());
        for (int i = 3; i < steps.size(); ++i)
            desk.key(Qt::Key_Right);
        QVERIFY(!desk.value("tour", "running").toBool());
        QCOMPARE(desk.settings->newsSeen(), QString(OMABOOK_VERSION));
        QVERIFY(!desk.manual->newsOffered());
        QVERIFY(!desk.value("tourNote", "visible").toBool());

        // Turned down, it is as seen as when it was taken.
        desk.settings->setNewsSeen("0.0.1");
        QTRY_VERIFY(desk.value("tourNote", "visible").toBool());
        QMetaObject::invokeMethod(desk.find("tourNoteDecline"), "clicked");
        QTRY_VERIFY(!desk.value("tourNote", "visible").toBool());
        QCOMPARE(desk.settings->newsSeen(), QString(OMABOOK_VERSION));
        // It is under the help whenever it is wanted again.
        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("help", "opened").toBool());
        QMetaObject::invokeMethod(desk.find("helpNews"), "clicked");
        QTRY_VERIFY(desk.value("tour", "running").toBool());
        QCOMPARE(desk.value("tour", "kind").toString(), QString("news"));
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.value("tour", "running").toBool());

        // Only what arrived since is shown: a release with nothing to say
        // says nothing, and counts as seen. An older build than the one
        // last seen says nothing either.
        desk.settings->setNewsSeen(OMABOOK_VERSION);
        const Manual later(&*desk.settings, &*desk.custom, "99.0.0");
        QVERIFY(!later.newsOffered());
        QCOMPARE(desk.settings->newsSeen(), QString("99.0.0"));
        const Manual earlier(&*desk.settings, &*desk.custom, "0.0.5");
        QVERIFY(!earlier.newsOffered());
        QVERIFY(earlier.news().isEmpty());
        // Someone who has never seen the tour is offered that, not the news.
        desk.settings->setTour("");
        desk.settings->setNewsSeen("");
        QVERIFY(desk.manual->tourOffered() && !desk.manual->newsOffered());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void whatIsSaidStillExists() {
        // The tour, what's new and the help's pages name commands and parts
        // of the window. A rename must fail here, not leave a card pointing
        // at nothing.
        const QString sources = QFileInfo(QFINDTESTDATA("../src/Main.qml")).absolutePath();
        QSet<QString> names{"book"};
        static const QRegularExpression objectName("objectName:\\s*\"([A-Za-z0-9_-]+)\"");
        for (const QFileInfo &info : QDir(sources).entryInfoList({"*.qml"}, QDir::Files)) {
            QFile file(info.filePath());
            QVERIFY(file.open(QIODevice::ReadOnly));
            for (auto it = objectName.globalMatch(QString::fromUtf8(file.readAll())); it.hasNext();)
                names << it.next().captured(1);
        }
        QVERIFY(names.contains("search") && names.contains("assistantPane"));
        QSet<QString> commands;
        for (const Customization::Command &command : Customization::commands())
            commands << command.id;
        const QVersionNumber current = QVersionNumber::fromString(OMABOOK_VERSION);
        QStringList problems;
        QSet<QString> ids;
        const auto check = [&](const QString &where, const Manual::Step &step, bool feature) {
            const QString what = where + " \"" + step.id + "\": ";
            if (step.id.isEmpty() || ids.contains(where + step.id))
                problems << what + "needs an id of its own";
            ids << where + step.id;
            if (step.title.isEmpty() || step.text.isEmpty())
                problems << what + "needs a title and a text";
            for (const QString &spot : step.spot.split('|', Qt::SkipEmptyParts))
                if (!names.contains(spot))
                    problems << what + "points at \"" + spot + "\", which no view has as an objectName";
            if (!step.command.isEmpty() && !commands.contains(step.command))
                problems << what + "there is no command " + step.command;
            for (const QString &command : Manual::mentioned(step.text))
                if (!commands.contains(command))
                    problems << what + "its text names {" + command + "}, which is no command";
            if (step.text.contains('{') && Manual::mentioned(step.text).size() != step.text.count('{'))
                problems << what + "a brace in its text isn't a command";
            if (!QStringList{"", "library", "reader"}.contains(step.place))
                problems << what + "its place is \"library\", \"reader\" or nothing";
            if (!QStringList{"", "empty", "books", "touch"}.contains(step.when))
                problems << what + "when is \"empty\", \"books\", \"touch\" or nothing";
            const QVersionNumber version = QVersionNumber::fromString(step.version);
            if (feature && (version.segmentCount() != 3 || version > current))
                problems << what + "arrives in \"" + step.version + "\", which isn't a version up to " + OMABOOK_VERSION;
        };
        for (const Manual::Step &step : Manual::tourSteps())
            check("tour.cpp", step, false);
        for (const Manual::Step &feature : Manual::features())
            check("whatsnew.cpp", feature, true);
        QVERIFY(!Manual::features().isEmpty());
        QSet<QString> topics;
        for (const Manual::Topic &topic : Manual::topicList()) {
            if (topic.id.isEmpty() || topics.contains(topic.id) || topic.title.isEmpty() || topic.summary.isEmpty() || topic.sections.isEmpty())
                problems << "helptopics.cpp \"" + topic.id + "\": needs an id of its own, a title, a summary and a section";
            topics << topic.id;
            for (const Manual::Section &section : topic.sections) {
                for (const QString &command : Manual::mentioned(section.text))
                    if (!commands.contains(command))
                        problems << "helptopics.cpp \"" + topic.id + "\": names {" + command + "}, which is no command";
                if (Manual::mentioned(section.text).size() > section.text.count('{'))
                    problems << "helptopics.cpp \"" + topic.id + "\": a brace isn't a command";
            }
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));

        // The pages are the same text for the window and the command line,
        // with the keys as they are bound: here, one of them moved.
        QTemporaryDir dir;
        QVERIFY(write(dir.filePath("keys.json"), "{\"library\": {\"Ctrl+N\": \"add\"}}"));
        Customization custom(dir.path());
        const Manual manual(nullptr, &custom, "0.1.0");
        const QString page = manual.topicText("start");
        QVERIFY2(page.contains("A adds book files") && page.contains("A  Ctrl+N"), qPrintable(page));
        QVERIFY(!page.contains('{'));
        QCOMPARE(manual.topicJson("start").value("title").toString(), QString("Getting started"));
        QVERIFY(manual.topicText("nothing").isEmpty());
        bool inWindow = false;
        for (const QVariant &topic : manual.topics())
            if (topic.toMap().value("id").toString() == "start")
                inWindow = topic.toMap().value("sections").toList().first().toMap().value("html").toString().contains("<b>A</b> adds book files");
        QVERIFY(inWindow);
        // Every command is in the help, with a description to show.
        QCOMPARE(manual.commands().size(), Customization::commands().size());
        for (const QVariant &command : manual.commands())
            QVERIFY(!command.toMap().value("text").toString().isEmpty());
    }

    // --- The plugin manager ---

    // Plugins of the user's own for the plugin manager's tests: a program
    // that answers look-ups, a script that runs, and a script that doesn't.
    static void stockPlugins(Desk &desk) {
        QDir folder(desk.dir.path());
        QVERIFY(folder.mkpath("plugins/shelfmark"));
        write(desk.dir.filePath("plugins/shelfmark/lookup"),
              "#!/bin/sh\ncat > /dev/null\n"
              "echo '{\"candidates\": [{\"title\": \"Pride and Prejudice\", \"authors\": [\"Jane Austen\"], \"published\": \"1813\"}]}'\n");
        QVERIFY(QFile::setPermissions(desk.dir.filePath("plugins/shelfmark/lookup"), QFile::Permissions(0x7755)));
        write(desk.dir.filePath("plugins/shelfmark/plugin.json"),
              QByteArray("{\"name\": \"Shelfmark\", \"version\": \"0.3\", \"author\": \"Me\", \"description\": \"My own catalog.\", "
                         "\"fields\": [\"title\", \"series\"], \"command\": [\"")
                  + desk.dir.filePath("plugins/shelfmark/lookup").toUtf8() + "\"]}");
        QVERIFY(folder.mkpath("plugins/finished"));
        write(desk.dir.filePath("plugins/finished/main.js"),
              "omabook.on(\"book.closed\", function (event) {});\n"
              "omabook.commands.register({id: \"mark\", title: \"Mark as read\", place: \"library\", key: \"Ctrl+R\", run: function () {}});\n");
        write(desk.dir.filePath("plugins/finished/plugin.json"),
              "{\"name\": \"Finished\", \"version\": \"1.0\", \"author\": \"Me\", \"description\": \"Marks a book as read when it "
              "is closed on its last page.\", \"script\": \"main.js\", \"permissions\": [\"http\"]}");
        QVERIFY(folder.mkpath("plugins/reading-log"));
        write(desk.dir.filePath("plugins/reading-log/main.js"), "var list = [];\nomabook.nothing.here();\n");
        write(desk.dir.filePath("plugins/reading-log/plugin.json"),
              "{\"name\": \"Reading Log\", \"version\": \"0.1\", \"description\": \"Keeps a list of what was read.\", "
              "\"script\": \"main.js\", \"permissions\": []}");
    }

    void thePluginManagerIsRunFromTheKeyboard() {
        Desk desk;
        stockPlugins(desk);
        // Nothing is opened on the desktop of whoever runs the tests.
        QList<QUrl> opened;
        desk.backend->pluginManager()->setOpener([&](const QUrl &url) {
            opened << url;
            return true;
        });
        QVERIFY(desk.show());
        QTRY_VERIFY(QFile::exists(desk.dir.filePath("plugins/reading-log.error")));
        desk.key(Qt::Key_P);
        QTRY_VERIFY(desk.value("plugins", "opened").toBool());
        QObject *dialog = desk.find("plugins");
        const auto current = [&] { return dialog->property("currentId").toString(); };
        const auto show = [&](const QString &id) { QMetaObject::invokeMethod(dialog, "show", Q_ARG(QVariant, id)); };
        QVERIFY(dialog->property("wide").toBool());
        // Sorted by what they do: the first is a source of book details.
        QCOMPARE(current(), QString("builtin:Open Library"));
        QCOMPARE(desk.value("pluginsList", "count").toInt(), int(desk.backend->pluginManager()->plugins().size()));
        desk.shot("pluginmanager");

        // One line says what needs attention, and A leads to the plugin,
        // whose error is shown with its file and line.
        QVERIFY(desk.value("pluginsHealth", "visible").toBool());
        QCOMPARE(desk.value("pluginsHealthText", "text").toString(), QString("Reading Log stopped with an error"));
        desk.key(Qt::Key_A);
        QCOMPARE(current(), QString("folder:reading-log"));
        QVERIFY(desk.value("pluginError", "visible").toBool());
        QVERIFY2(desk.value("pluginErrorMessage", "text").toString().contains("TypeError"),
                 qPrintable(desk.value("pluginErrorMessage", "text").toString()));
        QVERIFY2(desk.value("pluginErrorWhere", "text").toString().startsWith("main.js, line 2"),
                 qPrintable(desk.value("pluginErrorWhere", "text").toString()));
        desk.shot("pluginmanager-broken");
        // The picture in the README: a script's details, with the line above.
        show("folder:finished");
        desk.shot("pluginmanager-doc");
        show("folder:reading-log");
        // Put right and loaded again (Shift+R), the error and the line are gone.
        write(desk.dir.filePath("plugins/reading-log/main.js"), "var list = [];\n");
        desk.key(Qt::Key_R, Qt::ShiftModifier);
        QTRY_VERIFY(!desk.value("pluginsHealth", "visible").toBool());
        QVERIFY(!desk.value("pluginError", "visible").toBool());

        // / finds by name, kind or state; Escape clears it.
        QTest::keyClick(desk.window, '/');
        QTRY_VERIFY(desk.value("pluginsFilter", "activeFocus").toBool());
        desk.type("crossref");
        QCOMPARE(desk.value("pluginsList", "count").toInt(), 1);
        QCOMPARE(current(), QString("builtin:Crossref"));
        QCOMPARE(desk.value("pluginDetailName", "text").toString(), QString("Crossref"));
        QVERIFY(desk.value("pluginBrings", "text").toString().contains("publisher"));
        desk.key(Qt::Key_Escape);
        QVERIFY(desk.value("plugins", "opened").toBool());
        QCOMPARE(desk.value("pluginsFilter", "text").toString(), QString());
        QVERIFY(!desk.value("pluginsFilter", "activeFocus").toBool());
        QTest::keyClick(desk.window, '/');
        desk.type("your own");
        QCOMPARE(desk.value("pluginsList", "count").toInt(), 3);
        desk.key(Qt::Key_Escape);

        // Space switches the one selected, and it stays selected.
        show("builtin:Crossref");
        desk.key(Qt::Key_Space);
        QVERIFY(!desk.settings->pluginEnabled("builtin:Crossref", true));
        QCOMPARE(current(), QString("builtin:Crossref"));
        QCOMPARE(desk.value("pluginDetailSwitch", "checked").toBool(), false);
        desk.key(Qt::Key_Space);
        QVERIFY(desk.settings->pluginEnabled("builtin:Crossref", false));
        // What Omabook is made of has no switch.
        show("format:EPUB");
        QVERIFY(!desk.value("pluginDetailSwitch", "visible").toBool());
        desk.key(Qt::Key_Space);
        QVERIFY(desk.settings->pluginEnabled("format:EPUB", true));

        // A source is asked earlier with Alt+Up, and that is kept.
        show("builtin:Crossref");
        const QString before = desk.value("pluginOrder", "text").toString();
        desk.key(Qt::Key_Up, Qt::AltModifier);
        QVERIFY(desk.value("pluginOrder", "text").toString() != before);
        QVERIFY(desk.settings->pluginOrder().contains("builtin:Crossref"));
        QCOMPARE(current(), QString("builtin:Crossref"));

        // A script says what it added and what it may use.
        show("folder:finished");
        const QString adds = desk.value("pluginAdds", "text").toString();
        QVERIFY2(adds.contains("A command: Mark as read (Ctrl+R, in the library)") && adds.contains("a book is closed"), qPrintable(adds));
        QCOMPARE(desk.value("pluginPermissions", "text").toString(), QString("the network"));
        desk.shot("pluginmanager-script");
        // O opens its folder.
        desk.key(Qt::Key_O);
        QCOMPARE(opened, QList<QUrl>{QUrl::fromLocalFile(desk.dir.filePath("plugins/finished"))});

        // T tries one look-up against a source, and shows what came back.
        show("folder:shelfmark");
        desk.key(Qt::Key_T);
        QTRY_COMPARE(desk.value("pluginTryState", "text").toString(), QString("It answered with 1 book:"));
        QCOMPARE(desk.value("pluginTryResult", "text").toString(), QString("Pride and Prejudice · Jane Austen · 1813"));
        desk.shot("pluginmanager-try");

        // N starts one from a template, named as typed; it is selected and
        // its folder is shown.
        desk.key(Qt::Key_N);
        QTRY_VERIFY(desk.value("pluginNew", "opened").toBool());
        desk.type("My Notes");
        desk.shot("pluginmanager-new");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("pluginNew", "visible").toBool());
        QVERIFY(QFile::exists(desk.dir.filePath("plugins/my-notes/main.js")));
        QCOMPARE(current(), QString("folder:my-notes"));
        QCOMPARE(opened.last(), QUrl::fromLocalFile(desk.dir.filePath("plugins/my-notes")));

        // Delete asks first: Escape keeps it, Enter sends it to the trash.
        desk.key(Qt::Key_Delete);
        QTRY_VERIFY(desk.value("pluginRemoval", "opened").toBool());
        desk.shot("pluginmanager-remove");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("pluginRemoval", "visible").toBool());
        QVERIFY(QFile::exists(desk.dir.filePath("plugins/my-notes/main.js")));
        QVERIFY(desk.value("plugins", "opened").toBool());
        desk.key(Qt::Key_Delete);
        QTRY_VERIFY(desk.value("pluginRemoval", "opened").toBool());
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("pluginRemoval", "visible").toBool());
        QVERIFY(!QFile::exists(desk.dir.filePath("plugins/my-notes")));
        // What is built in can't be removed: Delete does nothing there.
        show("builtin:Crossref");
        desk.key(Qt::Key_Delete);
        QVERIFY(!desk.value("pluginRemoval", "opened").toBool());

        // I installs one: picked, looked at, and only in once it was said
        // whether it goes in switched on.
        QVERIFY(Fixtures::zip(desk.dir.filePath("gutenberg.zip"),
                              {{"gutenberg/plugin.json", "{\"name\": \"Gutenberg\", \"version\": \"2.0\", \"author\": \"Someone\", "
                                                         "\"description\": \"Looks books up in Project Gutenberg.\", \"script\": \"main.js\", "
                                                         "\"permissions\": [\"http\", \"run\"]}"},
                               {"gutenberg/main.js", "omabook.on(\"book.added\", function () {});\n"}}));
        desk.picker.next = {QUrl::fromLocalFile(desk.dir.filePath("gutenberg.zip"))};
        desk.key(Qt::Key_I);
        QTRY_VERIFY(desk.value("pluginInstall", "opened").toBool());
        desk.shot("pluginmanager-install");
        QMetaObject::invokeMethod(desk.find("pluginInstallZip"), "clicked");
        QCOMPARE(desk.picker.asked.last(), FilePicker::InstallPlugin);
        QTRY_COMPARE(desk.value("pluginInstallTitle", "text").toString(), QString("Install Gutenberg?"));
        QCOMPARE(desk.value("pluginInstallPermissions", "text").toString(),
                 QString("It may use: the network, programs on this computer"));
        QVERIFY(!QFile::exists(desk.dir.filePath("plugins/gutenberg")));
        desk.shot("pluginmanager-install-offer");
        QMetaObject::invokeMethod(desk.find("pluginInstallOff"), "clicked");
        QTRY_VERIFY(!desk.value("pluginInstall", "visible").toBool());
        QVERIFY(QFile::exists(desk.dir.filePath("plugins/gutenberg/main.js")));
        QVERIFY(!desk.settings->pluginEnabled("folder:gutenberg", true));
        QCOMPARE(current(), QString("folder:gutenberg"));
        QVERIFY(!desk.backend->scripts()->plugin("gutenberg"));
        // Something that is no plugin says why, and nothing goes in.
        QVERIFY(Fixtures::zip(desk.dir.filePath("evil.zip"), {{"plugin.json", "{\"script\": \"main.js\"}"}, {"main.js", "1"}, {"../evil.js", "x"}}));
        desk.picker.next = {QUrl::fromLocalFile(desk.dir.filePath("evil.zip"))};
        desk.key(Qt::Key_I);
        QTRY_VERIFY(desk.value("pluginInstall", "opened").toBool());
        QMetaObject::invokeMethod(desk.find("pluginInstallZip"), "clicked");
        QTRY_VERIFY(desk.value("pluginInstallError", "text").toString().contains("outside"));
        QVERIFY(!desk.value("pluginInstallOn", "visible").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("pluginInstall", "visible").toBool());
        QVERIFY(!QFile::exists(desk.dir.filePath("evil.js")));

        // The mark before a plugin is its switch, for the pointer; where the
        // window is touched, the rows and the mark are large enough for a finger.
        show("builtin:Open Library");
        auto *mark = qobject_cast<QQuickItem *>(desk.find("pluginMark_builtin:Crossref"));
        QVERIFY(mark);
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, mark->mapToScene(QPointF(mark->width() / 2, mark->height() / 2)).toPoint());
        QTRY_VERIFY(!desk.settings->pluginEnabled("builtin:Crossref", true));
        QCOMPARE(current(), QString("builtin:Crossref"));
        desk.key(Qt::Key_Space);
        QVERIFY(desk.settings->pluginEnabled("builtin:Crossref", false));
        desk.window->setProperty("touchSeen", true);
        mark = qobject_cast<QQuickItem *>(desk.find("pluginMark_builtin:Crossref"));
        QTRY_VERIFY(mark->width() >= 44 && mark->height() >= 44);
        desk.window->setProperty("touchSeen", false);

        // ? shows every key in place of the list; Escape goes back a step,
        // and then closes.
        QTest::keyClick(desk.window, '?');
        QTRY_VERIFY(desk.value("pluginsKeys", "visible").toBool());
        desk.shot("pluginmanager-keys");
        desk.key(Qt::Key_Escape);
        QVERIFY(!desk.value("pluginsKeys", "visible").toBool());
        QVERIFY(desk.value("plugins", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("plugins", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void thePluginManagerFitsEveryWindowAndTheme() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n";
        struct Look {
            const char *name;
            QByteArray language, colors;
            QSize size;
        };
        // Dark and light, left to right and right to left, roomy and as
        // small as the window gets.
        const QList<Look> looks{{"dark-small", "en", {}, QSize(720, 480)},
                                {"light", "en", light, QSize(1180, 780)},
                                {"light-small", "en", light, QSize(720, 480)},
                                {"rtl", "ar", {}, QSize(1180, 780)},
                                {"rtl-small", "ar", light, QSize(720, 480)}};
        for (const Look &look : looks) {
            Desk desk({}, look.language, look.colors);
            stockPlugins(desk);
            QVERIFY(desk.show());
            desk.window->resize(look.size);
            QTRY_COMPARE(desk.window->size(), look.size);
            QTRY_VERIFY(QFile::exists(desk.dir.filePath("plugins/reading-log.error")));
            desk.key(Qt::Key_P);
            QTRY_VERIFY(desk.value("plugins", "opened").toBool());
            QObject *dialog = desk.find("plugins");
            const bool wide = look.size.width() > 900;
            QCOMPARE(dialog->property("wide").toBool(), wide);
            auto *list = qobject_cast<QQuickItem *>(desk.find("pluginsList"));
            auto *detail = qobject_cast<QQuickItem *>(desk.find("pluginDetail"));
            QVERIFY(list && detail);
            const QRectF window(0, 0, look.size.width(), look.size.height());
            // Nothing hangs out of the window, and the list scrolls to its end.
            QVERIFY2(window.contains(list->mapRectToScene(list->boundingRect())), look.name);
            desk.shot(QString("pluginmanager-%1").arg(look.name));
            desk.key(Qt::Key_End);
            QCOMPARE(dialog->property("at").toInt(), list->property("count").toInt() - 1);
            QTRY_VERIFY2(list->property("contentY").toReal() > 0, look.name);
            desk.key(Qt::Key_Home);

            // The details stand beside the list, or take its place.
            QMetaObject::invokeMethod(dialog, "show", Q_ARG(QVariant, QString("folder:finished")));
            if (!wide) {
                QVERIFY(detail->isVisible() && !list->isVisible());
                QVERIFY(desk.value("pluginBack", "visible").toBool());
            } else {
                QVERIFY(detail->isVisible() && list->isVisible());
                // Mirrored, the list is on the right of the details.
                QCOMPARE(list->mapToScene(QPointF(0, 0)).x() > detail->mapToScene(QPointF(0, 0)).x(), look.language == "ar");
            }
            QTRY_VERIFY2(window.contains(detail->mapRectToScene(detail->boundingRect())), look.name);
            // Its text is as wide as the pane and no wider: it wraps, and
            // what is too long for the pane's height scrolls.
            auto *flick = qobject_cast<QQuickItem *>(desk.find("pluginDetailFlick"));
            QVERIFY(flick->property("contentWidth").toReal() <= flick->width());
            for (const char *name : {"pluginDetailName", "pluginDetailDescription", "pluginAdds", "pluginFolder"}) {
                auto *label = qobject_cast<QQuickItem *>(desk.find(name));
                QVERIFY2(label && label->width() <= flick->width(), name);
                QVERIFY2(!label->property("truncated").toBool(), name);
            }
            desk.shot(QString("pluginmanager-%1-detail").arg(look.name));
            // The sheets fit too.
            desk.key(Qt::Key_N);
            QTRY_VERIFY(desk.value("pluginNew", "opened").toBool());
            auto *sheet = qobject_cast<QQuickItem *>(desk.find("pluginNewCreate"));
            QVERIFY2(sheet && window.contains(sheet->mapRectToScene(sheet->boundingRect())), look.name);
            desk.shot(QString("pluginmanager-%1-new").arg(look.name));
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("pluginNew", "visible").toBool());

            // Escape steps back out of the details before it closes.
            desk.key(Qt::Key_Escape);
            if (!wide) {
                QVERIFY(desk.value("plugins", "opened").toBool());
                QVERIFY(list->isVisible());
                desk.key(Qt::Key_Escape);
            }
            QTRY_VERIFY(!desk.value("plugins", "visible").toBool());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(QString(look.name) + ": " + desk.warnings.join('\n')));
        }
    }

    // --- The knowledge graph, as nodes ---

private:
    // What a reader of "Harbour of Glass" has met in its two chapters: a
    // made-up story of a few dozen people, places and ideas, each where it
    // was met. Nothing in it is from a real book. Returns the nodes by name.
    static QHash<QString, qint64> storyGraph(Desk &desk, qint64 book) {
        struct Thing {
            const char *kind, *name, *summary;
            int section;
            double fraction;
        };
        static const QList<Thing> things{
            {"character", "Ilse Varga", "A glassblower's daughter who pilots the night ferry.", 0, 0.05},
            {"character", "Aldo Brenn", "The harbourmaster, who taught Ilse the tides.", 0, 0.1},
            {"character", "Tamsin Roe", "A smuggler with a debt to the guild.", 0, 0.2},
            {"character", "Odalys Quill", "Archivist of the Lantern Court.", 0, 0.3},
            {"character", "Captain Merrow", "Commands the guild's cutters.", 0, 0.4},
            {"character", "Pell", "A dock urchin who sees everything.", 0, 0.45},
            {"character", "The Glass Widow", "Rules the upper city from behind a veil.", 0, 0.6},
            {"character", "Brother Anselm", "Keeper of the tide bells.", 0, 0.7},
            {"character", "Nerys Varga", "Ilse's mother, lost on the night of the Breaking.", 1, 0.2},
            {"character", "Wick", "A lamplighter turned informer.", 1, 0.5},
            {"group", "The Tide Guild", "Keeps the ferry lines, and their secrets.", 0, 0.1},
            {"group", "The Lantern Court", "The city's council of nine.", 0, 0.3},
            {"group", "Saltwrights", "The shipbuilders of the lower docks.", 0, 0.5},
            {"group", "The Drowned Choir", "Those who sing beneath the harbour.", 1, 0.4},
            {"place", "Glass Harbour", "A city built on the sea wall it depends on.", 0, 0.05},
            {"place", "Lowwater Stairs", "Where the docks meet the old town.", 0, 0.2},
            {"place", "The Upper City", "Above the fog, and above the law.", 0, 0.6},
            {"place", "Saint Brine's Tower", "The bell tower over the sea gate.", 0, 0.7},
            {"place", "The Saltmarsh", "Tidal flats east of the wall.", 1, 0.1},
            {"place", "The Sunken Archive", "The court's records, under water since the Breaking.", 1, 0.6},
            {"concept", "Tide-singing", "Moving water with the voice. It costs the singer the memory of the song.", 0, 0.35},
            {"concept", "The Nine Tides", "The harbour's calendar, and its law.", 0, 0.15},
            {"concept", "Glasswright's Oath", "No glass is blown for war.", 0, 0.5},
            {"concept", "The memory tithe", "What a song takes from its singer.", 1, 0.3},
            {"item", "The Grey Ferry", "Ilse's boat, older than the guild.", 0, 0.05},
            {"item", "The Tide Bell", "Rung to close the sea gate.", 0, 0.7},
            {"item", "Nerys's Lantern", "Blue glass, still lit.", 1, 0.2},
            {"event", "The Breaking", "The night the sea wall failed, twenty years ago.", 0, 0.25},
            {"event", "The Lantern Vote", "The court votes on the oath.", 0, 0.55},
            {"event", "The Night of Low Water", "The harbour runs dry.", 1, 0.5},
            {"thread", "Who opened the sea gate", "Someone let the Breaking happen.", 0, 0.25},
            {"thread", "Tamsin's debt", "What she owes, and to whom.", 0, 0.2},
            {"thread", "The missing bell", "The tide bell is gone from its tower.", 0, 0.7},
            {"theme", "Memory and its price", "What is worth forgetting for.", 0, 0.35},
            {"theme", "Debt", "Everyone in the harbour owes someone.", 0, 0.5},
            {"topic", "Tidal harbours", "How real tide gates and locks work.", 0, 0.15},
            {"topic", "Glassblowing", "The craft behind the oath.", 0, 0.05},
            {"question", "Why does the guild fear singers?", "Because one singer can open the sea gate alone.", 0, 0.4},
            {"question", "Who is the Glass Widow?", "Nobody has seen her face; Odalys suspects she was at the Breaking.", 1, 0.05},
        };
        struct Tie {
            const char *from, *relation, *to, *note;
        };
        static const QList<Tie> ties{
            {"Ilse Varga", "pilots", "The Grey Ferry", ""},
            {"Aldo Brenn", "mentor of", "Ilse Varga", "Since she was nine."},
            {"Ilse Varga", "member of", "The Tide Guild", ""},
            {"Aldo Brenn", "harbourmaster of", "Glass Harbour", ""},
            {"Tamsin Roe", "owes", "The Tide Guild", ""},
            {"Tamsin Roe", "friend of", "Ilse Varga", ""},
            {"Odalys Quill", "serves", "The Lantern Court", ""},
            {"Captain Merrow", "commands for", "The Tide Guild", ""},
            {"Captain Merrow", "hunts", "Tamsin Roe", ""},
            {"Pell", "spies for", "Tamsin Roe", ""},
            {"Pell", "lives at", "Lowwater Stairs", ""},
            {"The Glass Widow", "rules", "The Upper City", ""},
            {"The Glass Widow", "leads", "The Lantern Court", ""},
            {"Brother Anselm", "keeps", "The Tide Bell", ""},
            {"The Tide Bell", "hangs in", "Saint Brine's Tower", ""},
            {"Ilse Varga", "can do", "Tide-singing", "She doesn't know it yet."},
            {"The Tide Guild", "forbids", "Tide-singing", ""},
            {"Tide-singing", "raises", "Memory and its price", ""},
            {"The Nine Tides", "governs", "Glass Harbour", ""},
            {"The Breaking", "struck", "Glass Harbour", ""},
            {"The Breaking", "part of", "Who opened the sea gate", ""},
            {"Tamsin's debt", "about", "Tamsin Roe", ""},
            {"Tamsin's debt", "raises", "Debt", ""},
            {"Saltwrights", "built", "The Grey Ferry", ""},
            {"Saltwrights", "rivals of", "The Tide Guild", ""},
            {"The Lantern Vote", "held by", "The Lantern Court", ""},
            {"The Lantern Vote", "decides", "Glasswright's Oath", ""},
            {"Odalys Quill", "witnessed", "The Breaking", ""},
            {"The missing bell", "about", "The Tide Bell", ""},
            {"Brother Anselm", "suspects", "Captain Merrow", ""},
            {"Why does the guild fear singers?", "asked about", "The Tide Guild", ""},
            {"Why does the guild fear singers?", "asked about", "Tide-singing", ""},
            {"Tidal harbours", "explains", "The Nine Tides", ""},
            {"Glassblowing", "behind", "Glasswright's Oath", ""},
            {"Lowwater Stairs", "part of", "Glass Harbour", ""},
            {"The Upper City", "above", "Glass Harbour", ""},
            {"Saint Brine's Tower", "guards", "Glass Harbour", ""},
            {"Nerys Varga", "mother of", "Ilse Varga", ""},
            {"Nerys Varga", "sang at", "The Breaking", ""},
            {"Nerys's Lantern", "belonged to", "Nerys Varga", ""},
            {"Ilse Varga", "carries", "Nerys's Lantern", ""},
            {"The Drowned Choir", "sings in", "The Sunken Archive", ""},
            {"Nerys Varga", "joined", "The Drowned Choir", ""},
            {"The memory tithe", "price of", "Tide-singing", ""},
            {"Wick", "informs", "The Glass Widow", ""},
            {"Wick", "betrays", "Tamsin Roe", ""},
            {"The Night of Low Water", "empties", "The Saltmarsh", ""},
            {"Odalys Quill", "seeks", "The Sunken Archive", ""},
            {"Who is the Glass Widow?", "asked about", "The Glass Widow", ""},
        };
        Graph graph(*desk.backend->library());
        graph.syncLibrary();
        QHash<QString, qint64> ids;
        QHash<QString, GraphPlace> met;
        for (const Thing &thing : things) {
            const GraphPlace place{book, thing.section, thing.fraction};
            const QJsonObject data = QString(thing.name) == "Ilse Varga" ? QJsonObject{{"aliases", QJsonArray{"The Pilot"}}} : QJsonObject();
            ids.insert(thing.name, graph.upsert({}, thing.kind, thing.name, thing.summary, data, place));
            met.insert(thing.name, place);
        }
        for (const Tie &tie : ties) {
            // A relation is learned once both its ends have been met.
            const GraphPlace a = met.value(tie.from), b = met.value(tie.to);
            graph.link(ids.value(tie.from), ids.value(tie.to), tie.relation, tie.note, a.notAfter(b) ? b : a);
        }
        return ids;
    }

    // The graph view in a pane, with what it browses and draws.
    struct GraphParts {
        QQuickItem *pane = nullptr;
        GraphBrowser *browser = nullptr;
        GraphCanvas *canvas = nullptr;
    };
    static GraphParts graphParts(const Desk &desk, const QString &name) {
        GraphParts parts;
        parts.pane = qobject_cast<QQuickItem *>(desk.find(name));
        if (parts.pane) {
            parts.browser = parts.pane->property("browser").value<GraphBrowser *>();
            parts.canvas = parts.pane->findChild<GraphCanvas *>("graphCanvas");
        }
        return parts;
    }
    // An item of a pane by name: a view in a Repeater isn't a QObject child of it.
    static QQuickItem *beside(const GraphParts &graph, const QString &name) {
        QList<QQuickItem *> items{graph.pane};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (item->objectName() == name && item->isVisible())
                return item;
            items << item->childItems();
        }
        return nullptr;
    }
    static QPoint onNode(const GraphParts &graph, qint64 node) {
        return graph.canvas->mapToScene(graph.canvas->pointOf(node)).toPoint();
    }
    static QStringList shownNames(const GraphBrowser *browser) {
        QStringList names;
        for (const QVariant &row : browser->rows())
            names << row.toMap().value("name").toString();
        return names;
    }

private slots:
    void theLibraryIsAGraphToo() {
        Desk desk;
        const QList<qint64> books = stock(desk);
        QVERIFY(!books.isEmpty());
        const QHash<QString, qint64> ids = storyGraph(desk, books.first());
        QVERIFY(desk.show());

        // 6, after the five ways of showing the shelf, is the graph.
        desk.key(Qt::Key_6);
        QTRY_VERIFY(desk.find("libraryGraph"));
        const GraphParts graph = graphParts(desk, "libraryGraph");
        QVERIFY(graph.pane && graph.browser && graph.canvas);
        QTRY_VERIFY(graph.pane->isVisible());
        QVERIFY(!qobject_cast<QQuickItem *>(desk.find("search"))->isVisible());
        // The books with their authors, series and tags, and the story.
        QVERIFY(graph.browser->nodeCount() > 28 + 39);
        QVERIFY(shownNames(graph.browser).contains("Ilse Varga") && shownNames(graph.browser).contains("Mira Okafor"));
        QCOMPARE(desk.value("graphTitle", "text").toString(), QString("Everything read"));
        QVERIFY(desk.find("graphKind-character") && desk.find("graphKind-book"));
        QTRY_VERIFY(!graph.browser->settling());
        desk.shot("graph-library");

        // Find a thing by typing its name.
        desk.key(Qt::Key_Slash);
        QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
        desk.type("ilse");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("graphPalette", "visible").toBool());
        QCOMPARE(graph.browser->selected(), ids.value("Ilse Varga"));
        // Everything known of her is beside the picture.
        QTRY_VERIFY(desk.value("graphDetails", "visible").toBool());
        QCOMPARE(desk.value("graphDetailsName", "text").toString(), QString("Ilse Varga"));
        QCOMPARE(desk.value("graphDetailsKind", "text").toString(), QString("Character"));
        QVERIFY(desk.value("graphDetailsAliases", "text").toString().contains("The Pilot"));
        QVERIFY(desk.value("graphDetailsSummary", "text").toString().contains("night ferry"));
        desk.shot("graph-library-selected");

        // Enter looks around her; the picture is hers and her neighbours'.
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(graph.browser->centre(), ids.value("Ilse Varga"));
        QCOMPARE(graph.browser->depth(), 1);
        QVERIFY(shownNames(graph.browser).contains("Aldo Brenn") && !shownNames(graph.browser).contains("Wick"));
        QVERIFY(desk.find("graphChip-reach"));
        QTRY_VERIFY(!graph.browser->settling());
        // The arrow keys walk along her relations, and H, J, K, L do too.
        const qint64 before = graph.browser->selected();
        desk.key(Qt::Key_Right);
        const qint64 right = graph.browser->selected();
        desk.key(Qt::Key_H);
        desk.key(Qt::Key_Left);
        desk.key(Qt::Key_Down);
        desk.key(Qt::Key_K);
        QVERIFY(right != before || graph.browser->selected() != before);
        // ] reaches a step further, [ a step less.
        const int near = graph.browser->nodeCount();
        desk.key(Qt::Key_BracketRight);
        QTRY_VERIFY(graph.browser->nodeCount() > near);
        desk.shot("graph-library-around");
        desk.key(Qt::Key_BracketLeft);
        QTRY_COMPARE(graph.browser->nodeCount(), near);
        // A tap on a node selects it; a double tap looks around it.
        QTRY_VERIFY(!graph.browser->settling());
        QTest::qWait(300);
        const qint64 aldo = ids.value("Aldo Brenn");
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, onNode(graph, aldo));
        QTRY_COMPARE(graph.browser->selected(), aldo);
        QCOMPARE(desk.value("graphDetailsName", "text").toString(), QString("Aldo Brenn"));
        // His relations read as sentences, and each goes to its other end.
        QVERIFY(graph.browser->details().value("relations").toList().size() >= 2);
        // Backspace goes back to everything.
        desk.key(Qt::Key_Backspace);
        QTRY_COMPARE(graph.browser->depth(), 0);
        QVERIFY(shownNames(graph.browser).contains("Wick"));
        // Tab goes through the nodes in the order of the list.
        graph.browser->select(0);
        desk.key(Qt::Key_Tab);
        QCOMPARE(graph.browser->details().value("name").toString(), shownNames(graph.browser).first());
        desk.key(Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(graph.browser->details().value("name").toString(), shownNames(graph.browser).last());

        // The kinds are the legend and the filter: a click leaves one out.
        const int all = graph.browser->nodeCount();
        auto *chip = qobject_cast<QQuickItem *>(desk.find("graphKind-book"));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
        QTRY_COMPARE(graph.browser->nodeCount(), all - 28);
        // F filters by typing: one book's graph.
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
        desk.type("harbour of");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(graph.browser->bookId(), books.first());
        QCOMPARE(desk.value("graphTitle", "text").toString(), QString("Harbour of Glass"));
        QTRY_VERIFY(!desk.value("graphPalette", "visible").toBool());
        // And back out to everything, with what was asked about only.
        auto *whole = qobject_cast<QQuickItem *>(desk.find("graphChip-whole"));
        QVERIFY(whole);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, whole->mapToScene(QPointF(whole->width() / 2, whole->height() / 2)).toPoint());
        QTRY_COMPARE(graph.browser->showing(), QString("scope"));
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
        desk.type("asked");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(graph.browser->onlyAsked());
        QCOMPARE(shownNames(graph.browser).size(), 5);
        QTRY_VERIFY(!desk.value("graphPalette", "visible").toBool());
        desk.key(Qt::Key_F);
        QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
        desk.type("everything again");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(graph.browser->nodeCount(), all);
        QTRY_VERIFY(!desk.value("graphPalette", "visible").toBool());

        // V shows the same nodes as a list by kind, walked with the same keys.
        desk.key(Qt::Key_V);
        QTRY_VERIFY(desk.value("graphList", "visible").toBool());
        QVERIFY(!graph.canvas->isVisible());
        graph.browser->select(ids.value("Ilse Varga"));
        desk.key(Qt::Key_J);
        QCOMPARE(graph.browser->details().value("name").toString(), QString("Nerys Varga"));
        desk.key(Qt::Key_Up);
        QCOMPARE(graph.browser->selected(), ids.value("Ilse Varga"));
        desk.shot("graph-library-list");
        desk.key(Qt::Key_V);
        QTRY_VERIFY(graph.canvas->isVisible());

        // O on a book reads it; the graph is there to come back to.
        graph.browser->jumpTo(Graph(*desk.backend->library()).bookNode(books.first()));
        QCOMPARE(desk.value("graphDetailsKind", "text").toString(), QString("Book"));
        desk.key(Qt::Key_O);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->bookId(), books.first());
        QVERIFY(!graph.pane->isVisible());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.reader->isOpen());
        QTRY_VERIFY(graph.pane->isVisible());
        // Escape closes the graph, and the shelf is back with its keys.
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!graph.pane->isVisible());
        QVERIFY(qobject_cast<QQuickItem *>(desk.find("search"))->isVisible());
        // U is the graph of the selected book alone.
        desk.key(Qt::Key_U);
        QTRY_VERIFY(graph.pane->isVisible());
        QCOMPARE(graph.browser->showing(), QString("book"));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!graph.pane->isVisible());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theGraphBesideThePageStopsAtThePage() {
        Desk desk;
        const QString file = desk.dir.filePath("harbour.epub");
        QVERIFY(Fixtures::epub(file, "Harbour of Glass"));
        const qint64 book = desk.add(file);
        const QHash<QString, qint64> ids = storyGraph(desk, book);
        QVERIFY(desk.show());
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        // At the end of the first chapter.
        desk.reader->goTo(0, {}, true);
        QTRY_COMPARE(desk.reader->fraction(), 1.0);
        QCOMPARE(desk.reader->section(), 0);

        // M shows the map of what the book has told so far, beside the page.
        desk.key(Qt::Key_M);
        QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
        const GraphParts graph = graphParts(desk, "readerGraph");
        QVERIFY(graph.pane && graph.browser && graph.canvas);
        QVERIFY(graph.browser->bounded());
        QTRY_VERIFY(graph.browser->nodeCount() > 20);
        QCOMPARE(graph.browser->bookId(), book);
        QCOMPARE(desk.value("graphTitle", "text").toString(), QString("Known so far"));
        // Exactly what the storage says a reader at this place has met:
        // nothing of the second chapter, and nothing of the library either.
        const auto met = [&] {
            QStringList names;
            for (const GraphNode &node : Graph(*desk.backend->library()).ofBook(book, desk.reader->place()).nodes)
                names << node.name;
            names.sort();
            return names;
        };
        const auto sorted = [](QStringList names) {
            names.sort();
            return names;
        };
        QCOMPARE(sorted(shownNames(graph.browser)), met());
        QVERIFY(!shownNames(graph.browser).contains("Wick") && !shownNames(graph.browser).contains("Nerys Varga"));
        QVERIFY(!shownNames(graph.browser).contains("Harbour of Glass"));
        // The search doesn't know what lies ahead, and it can't be jumped to.
        QVERIFY(graph.browser->find("wick").isEmpty());
        graph.browser->jumpTo(ids.value("Wick"));
        QCOMPARE(graph.browser->selected(), 0);

        // While the map has the keyboard its keys are its own: J walks the
        // graph and the page stays where it is.
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        const double fraction = desk.reader->fraction();
        desk.key(Qt::Key_J);
        desk.key(Qt::Key_L);
        QVERIFY(graph.browser->selected() > 0);
        QCOMPARE(desk.reader->section(), 0);
        QCOMPARE(desk.reader->fraction(), fraction);
        QTRY_VERIFY(desk.value("graphDetails", "visible").toBool());
        desk.shot("graph-reader-early");

        // Reading on lets in what the next chapter told.
        desk.reader->goTo(1, {}, true);
        QTRY_VERIFY(shownNames(graph.browser).contains("Wick"));
        QCOMPARE(sorted(shownNames(graph.browser)), met());
        QVERIFY(shownNames(graph.browser).contains("Nerys Varga"));
        // Going back to an earlier page doesn't take it away again.
        desk.reader->goTo(0);
        QTest::qWait(600);
        QVERIFY(shownNames(graph.browser).contains("Wick"));

        // From a thing to the place it was first met.
        graph.browser->jumpTo(ids.value("Wick"));
        QCOMPARE(desk.value("graphDetailsName", "text").toString(), QString("Wick"));
        QTRY_VERIFY(!graph.browser->settling());
        graph.canvas->forceActiveFocus();
        desk.key(Qt::Key_O);
        QTRY_COMPARE(desk.reader->section(), 1);
        // And with a tap on where it says so.
        desk.reader->goTo(0);
        QTRY_COMPARE(desk.reader->section(), 0);
        // Once the pane has caught up with the page and stands still.
        QTest::qWait(700);
        QTRY_VERIFY(beside(graph, "graphPlace"));
        auto *place = beside(graph, "graphPlace");
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, place->mapToScene(QPointF(place->width() / 2, place->height() / 2)).toPoint());
        QTRY_COMPARE(desk.reader->section(), 1);
        desk.shot("graph-reader");

        // Escape closes the map, not the book; M brings it back.
        graph.canvas->forceActiveFocus();
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!graph.pane->isVisible());
        QVERIFY(desk.reader->isOpen());
        // Without it, the keys are the book's again.
        desk.key(Qt::Key_Slash);
        QTRY_VERIFY(desk.value("finder", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("finder", "visible").toBool());
        desk.key(Qt::Key_M);
        QTRY_VERIFY(graph.pane->isVisible());
        desk.key(Qt::Key_M);
        QTRY_VERIFY(!graph.pane->isVisible());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.reader->isOpen());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theGraphIsEditedByHand() {
        Desk desk;
        const QString file = desk.dir.filePath("harbour.epub");
        QVERIFY(Fixtures::epub(file, "Harbour of Glass"));
        const qint64 book = desk.add(file);
        const QHash<QString, qint64> ids = storyGraph(desk, book);
        QVERIFY(desk.show());
        desk.key(Qt::Key_6);
        QTRY_VERIFY(desk.find("libraryGraph"));
        const GraphParts graph = graphParts(desk, "libraryGraph");
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        Graph stored(*desk.backend->library());
        const qint64 tamsin = ids.value("Tamsin Roe"), pell = ids.value("Pell");

        // E renames a thing and rewrites what is said of it.
        graph.browser->select(tamsin);
        desk.key(Qt::Key_E);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QCOMPARE(desk.value("graphEditName", "text").toString(), QString("Tamsin Roe"));
        auto *name = qobject_cast<QQuickItem *>(desk.find("graphEditName"));
        QTRY_VERIFY(name->hasActiveFocus());
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.type("tamsin of the stairs");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
        QCOMPARE(stored.node(tamsin)->name, QString("tamsin of the stairs"));
        QCOMPARE(stored.named({}, "character", "Tamsin Roe"), tamsin);
        QTRY_COMPARE(desk.value("graphDetailsName", "text").toString(), QString("tamsin of the stairs"));
        // A name another of its kind has is refused, and says to merge.
        desk.key(Qt::Key_E);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QTRY_VERIFY(name->hasActiveFocus());
        desk.key(Qt::Key_A, Qt::ControlModifier);
        desk.type("pell");
        desk.key(Qt::Key_Return);
        QVERIFY(desk.value("graphEditProblem", "text").toString().contains("Merge"));
        QVERIFY(desk.value("graphEdit", "opened").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());

        // Shift+M folds another thing into the selected one, and asks first.
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        const int before = graph.browser->nodeCount();
        desk.key(Qt::Key_M, Qt::ShiftModifier);
        QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
        QCOMPARE(desk.value("graphPalette", "mode").toString(), QString("merge"));
        desk.type("pell");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QVERIFY(desk.value("graphEditHeading", "text").toString().contains("Pell"));
        desk.shot("graph-merge");
        // Escape keeps both.
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
        QVERIFY(stored.node(pell));
        QCOMPARE(graph.browser->nodeCount(), before);
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        desk.key(Qt::Key_M, Qt::ShiftModifier);
        QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
        desk.type("pell");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QTRY_VERIFY(qobject_cast<QQuickItem *>(desk.find("graphEditConfirm"))->hasActiveFocus());
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!stored.node(pell));
        QCOMPARE(graph.browser->nodeCount(), before - 1);
        QCOMPARE(graph.browser->selected(), tamsin);
        QCOMPARE(stored.named({}, "character", "Pell"), tamsin);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());

        // N adds a thing, related to the selected one.
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        desk.key(Qt::Key_N);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QTRY_VERIFY(name->hasActiveFocus());
        desk.type("odo");
        desk.key(Qt::Key_Tab);
        desk.type("a ferryman");
        desk.key(Qt::Key_Tab);
        desk.type("rows for");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
        const qint64 odo = stored.named({}, "character", "odo");
        QVERIFY(odo > 0);
        QCOMPARE(graph.browser->selected(), odo);
        QCOMPARE(stored.node(odo)->summary, QString("a ferryman"));
        QCOMPARE(graph.browser->details().value("relations").toList().size(), 1);

        // A relation is taken out with the cross on its row; that asks too.
        QTRY_VERIFY(desk.find("graphRelation"));
        auto *relation = qobject_cast<QQuickItem *>(desk.find("graphRelation"));
        QTest::mouseMove(desk.window, relation->mapToScene(QPointF(relation->width() / 2, relation->height() / 2)).toPoint());
        auto *cross = relation->findChild<QQuickItem *>("graphUnlink");
        QTRY_VERIFY(cross && cross->isVisible());
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, cross->mapToScene(QPointF(cross->width() / 2, cross->height() / 2)).toPoint());
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QCOMPARE(desk.value("graphEdit", "what").toString(), QString("unlink"));
        // It did not also jump to the other end of the relation.
        QCOMPARE(graph.browser->selected(), odo);
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(graph.browser->details().value("relations").toList().size(), 0);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());

        // D removes the selected thing, after asking.
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        desk.key(Qt::Key_D);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QVERIFY(desk.value("graphEditHeading", "text").toString().contains("odo"));
        desk.shot("graph-remove");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
        QVERIFY(stored.node(odo));
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        desk.key(Qt::Key_Delete);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QTRY_VERIFY(qobject_cast<QQuickItem *>(desk.find("graphEditConfirm"))->hasActiveFocus());
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!stored.node(odo));
        QCOMPARE(graph.browser->selected(), 0);
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
        // A book is not removed here: that is the library's to do.
        graph.browser->jumpTo(stored.bookNode(book));
        desk.key(Qt::Key_D);
        QTRY_VERIFY(desk.value("toastText", "text").toString().contains("library"));
        QVERIFY(!desk.value("graphEdit", "opened").toBool());
        QVERIFY(desk.backend->library()->book(book));

        // What something else writes to the graph shows without being asked for.
        const qint64 storm = stored.upsert({}, "event", "The Winter Storm", "Sinks the old ferry.");
        stored.link(storm, ids.value("Glass Harbour"), "strikes");
        GraphFeed::announce();
        QTRY_VERIFY(shownNames(graph.browser).contains("The Winter Storm"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void anEmptyGraphSaysHowItGrows() {
        Desk desk;
        const QString file = desk.dir.filePath("quiet.epub");
        QVERIFY(Fixtures::epub(file, "A Quiet Book"));
        const qint64 book = desk.add(file);
        desk.settings->setGraph("off");
        QVERIFY(desk.show());
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.key(Qt::Key_M);
        QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
        const GraphParts graph = graphParts(desk, "readerGraph");
        // Nothing is known of it: that is said, with why and what to do.
        QCOMPARE(graph.browser->nodeCount(), 0);
        QVERIFY(desk.value("graphEmpty", "visible").toBool());
        QCOMPARE(desk.value("graphEmptyTitle", "text").toString(), QString("Nothing noted yet"));
        QVERIFY(desk.value("graphEmptyText", "text").toString().contains("switched off"));
        QVERIFY(!desk.value("graphGrow", "visible").toBool());
        desk.shot("graph-empty-off");
        // Switched on, the AI can be asked to note what was read.
        desk.settings->setGraph("ask");
        QTRY_VERIFY(desk.value("graphEmptyText", "text").toString().contains("when you ask"));
        QVERIFY(desk.value("graphGrow", "visible").toBool());
        desk.settings->setGraph("auto");
        QTRY_VERIFY(desk.value("graphEmptyText", "text").toString().contains("as you finish"));
        desk.shot("graph-empty");
        // Something can be added by hand all the same, and then it isn't empty.
        QTRY_VERIFY(graph.canvas->hasActiveFocus());
        desk.key(Qt::Key_N);
        QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
        QTRY_VERIFY(qobject_cast<QQuickItem *>(desk.find("graphEditName"))->hasActiveFocus());
        desk.type("mara");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(graph.browser->nodeCount(), 1);
        QVERIFY(!desk.value("graphEmpty", "visible").toBool());
        QCOMPARE(desk.value("graphDetailsName", "text").toString(), QString("mara"));
        QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!graph.pane->isVisible());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.reader->isOpen());

        // The library with books but nothing read into them shows the books
        // and says what joins them; filtered to nothing, it says that.
        desk.key(Qt::Key_6);
        QTRY_VERIFY(desk.find("libraryGraph"));
        const GraphParts whole = graphParts(desk, "libraryGraph");
        Graph(*desk.backend->library()).remove(Graph(*desk.backend->library()).named({}, "character", "mara"));
        whole.browser->refresh();
        QVERIFY(whole.browser->nodeCount() > 0 && !whole.browser->storied());
        QVERIFY(desk.value("graphGrows", "visible").toBool());
        for (const QVariant &kind : whole.browser->kinds())
            whole.browser->setKindShown(kind.toMap().value("kind").toString(), false);
        QCOMPARE(whole.browser->nodeCount(), 0);
        QTRY_VERIFY(desk.value("graphShowAll", "visible").toBool());
        QCOMPARE(desk.value("graphEmptyTitle", "text").toString(), QString("Nothing matches"));
        auto *again = qobject_cast<QQuickItem *>(desk.find("graphShowAll"));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, again->mapToScene(QPointF(again->width() / 2, again->height() / 2)).toPoint());
        QTRY_VERIFY(whole.browser->nodeCount() > 0);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theGraphFitsEveryWindowAndTheme() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n";
        struct Look {
            const char *name;
            QByteArray language, colors;
            QSize size;
        };
        const QList<Look> looks{{"dark", "en", {}, QSize(1180, 780)},
                                {"dark-small", "en", {}, QSize(720, 480)},
                                {"light", "en", light, QSize(1180, 780)},
                                {"light-small", "en", light, QSize(720, 480)},
                                {"rtl", "ar", {}, QSize(1180, 780)},
                                {"rtl-small", "ar", light, QSize(720, 480)}};
        for (const Look &look : looks) {
            Desk desk({}, look.language, look.colors);
            const QList<qint64> books = stock(desk);
            QVERIFY(!books.isEmpty());
            const QHash<QString, qint64> ids = storyGraph(desk, books.first());
            QVERIFY(desk.show());
            desk.window->resize(look.size);
            QTRY_COMPARE(desk.window->size(), look.size);
            const QRectF window(0, 0, look.size.width(), look.size.height());
            const bool wide = look.size.width() >= 760;
            const auto inside = [&](const char *name) {
                auto *item = qobject_cast<QQuickItem *>(desk.find(name));
                // By its own size: a text field's bounding box is that of its text.
                return item && item->isVisible() && item->width() > 0 && item->height() > 0
                       && window.adjusted(-1, -1, 1, 1).contains(item->mapRectToScene(QRectF(0, 0, item->width(), item->height())));
            };

            // Over the shelf: the picture, and the selected thing's details
            // beside it or under it.
            desk.key(Qt::Key_6);
            QTRY_VERIFY(desk.find("libraryGraph"));
            const GraphParts graph = graphParts(desk, "libraryGraph");
            QTRY_VERIFY2(graph.pane->isVisible(), look.name);
            QCOMPARE(graph.pane->property("compact").toBool(), !wide);
            QTRY_VERIFY(!graph.browser->settling());
            QVERIFY2(inside("graphCanvas") && inside("graphKinds"), look.name);
            // Every node is in the picture, which is fitted to the view.
            QTest::qWait(300);
            const QRectF picture(0, 0, graph.canvas->width(), graph.canvas->height());
            for (const GraphBrowser::Shown &shown : graph.browser->shown())
                QVERIFY2(picture.contains(graph.canvas->pointOf(shown.node.id)), qPrintable(QString(look.name) + ": " + shown.node.name));
            desk.shot(QString("graph-%1").arg(look.name));
            graph.browser->select(ids.value("Ilse Varga"));
            QTRY_VERIFY2(inside("graphDetails"), look.name);
            QVERIFY2(inside("graphCanvas"), look.name);
            auto *details = qobject_cast<QQuickItem *>(desk.find("graphDetails"));
            const QRectF panel = details->mapRectToScene(details->boundingRect());
            const QRectF drawn = graph.canvas->mapRectToScene(graph.canvas->boundingRect());
            QVERIFY2(!panel.intersects(drawn.adjusted(1, 1, -1, -1)), look.name);
            if (wide) // Mirrored, the details are on the left of the picture.
                QCOMPARE(panel.left() < drawn.left(), look.language == "ar");
            else
                QVERIFY2(panel.top() >= drawn.bottom() - 1, look.name);
            // Its text is as wide as the panel and no wider.
            auto *flick = qobject_cast<QQuickItem *>(desk.find("graphDetailsFlick"));
            for (const char *name : {"graphDetailsName", "graphDetailsSummary"}) {
                auto *label = qobject_cast<QQuickItem *>(desk.find(name));
                QTRY_VERIFY2(label && label->width() <= flick->width() && !label->property("truncated").toBool(),
                             qPrintable(QString("%1 %2: %3 in %4").arg(look.name, name).arg(label ? label->width() : -1).arg(flick->width())));
            }
            desk.key(Qt::Key_Return);
            QTRY_COMPARE(graph.browser->centre(), ids.value("Ilse Varga"));
            QTRY_VERIFY(!graph.browser->settling());
            desk.shot(QString("graph-%1-around").arg(look.name));
            // The list takes the picture's place, and fits as well.
            desk.key(Qt::Key_V);
            QTRY_VERIFY2(inside("graphList"), look.name);
            desk.shot(QString("graph-%1-list").arg(look.name));
            desk.key(Qt::Key_V);
            // The boxes to type in fit too.
            desk.key(Qt::Key_F);
            QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
            desk.shot(QString("graph-%1-filter").arg(look.name));
            QTRY_VERIFY2(inside("graphPaletteField") && inside("graphPaletteList"), look.name);
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("graphPalette", "visible").toBool());
            desk.key(Qt::Key_N);
            QTRY_VERIFY(desk.value("graphEdit", "opened").toBool());
            QTRY_VERIFY2(inside("graphEditName") && inside("graphEditConfirm") && inside("graphEditCancel"), look.name);
            desk.shot(QString("graph-%1-add").arg(look.name));
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("graphEdit", "visible").toBool());
            QTRY_VERIFY(graph.canvas->hasActiveFocus());
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!graph.pane->isVisible());

            // Beside the page.
            desk.reader->openBook(books.first());
            QTRY_VERIFY(desk.reader->isOpen());
            desk.reader->goTo(1, {}, true);
            desk.key(Qt::Key_M);
            QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
            const GraphParts beside = graphParts(desk, "readerGraph");
            QTRY_VERIFY(beside.browser->nodeCount() > 30);
            QTRY_VERIFY(!beside.browser->settling());
            QVERIFY2(inside("readerGraph"), look.name);
            auto *page = qobject_cast<QQuickItem *>(desk.find("contentLoader"));
            // Mirrored, the map is on the left of the page.
            QCOMPARE(beside.pane->mapToScene(QPointF(0, 0)).x() < page->mapToScene(QPointF(0, 0)).x(), look.language == "ar");
            QVERIFY2(page->width() >= look.size.width() * 0.4, look.name);
            beside.browser->select(ids.value("The Tide Guild"));
            QTRY_VERIFY(beside.pane->findChild<QQuickItem *>("graphDetails")->isVisible());
            QTest::qWait(300);
            desk.shot(QString("graph-reader-%1").arg(look.name));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(QString(look.name) + ": " + desk.warnings.join('\n')));
        }
    }

    void theGraphIsBuiltForBooksAlreadyRead() {
        Desk desk;
        // Long enough to be worth asking about: a page or two.
        QByteArray story;
        for (int line = 0; line < 60; ++line)
            story += "Ann walked the sea wall at dusk and counted the lamps along the harbour, as she did every night.\n\n";
        QVERIFY(Fixtures::write(desk.dir.filePath("The Sea Wall.txt"), story));
        const qint64 id = desk.add(desk.dir.filePath("The Sea Wall.txt"));
        desk.backend->library()->setReading(id, "read", 0, {});
        desk.ai.answer = "{\"nodes\": [{\"kind\": \"character\", \"name\": \"Ann\", \"section\": 0}], \"edges\": []}";
        QVERIFY(desk.show());
        // Shift+U: what it would send is said before anything is sent.
        desk.key(Qt::Key_U, Qt::ShiftModifier);
        QTRY_VERIFY(desk.value("graphBuild", "opened").toBool());
        QTRY_VERIFY(!desk.backend->graphJobs()->planning());
        QCOMPARE(desk.backend->graphJobs()->plan().value("books").toInt(), 1);
        QVERIFY(desk.value("graphBuildState", "text").toString().contains("tokens"));
        QCOMPARE(desk.ai.requests.size(), 0);
        QMetaObject::invokeMethod(desk.find("graphBuildStart"), "clicked");
        QTRY_VERIFY(!desk.backend->graphJobs()->busy());
        QTRY_VERIFY(desk.backend->graphJobs()->summary().value("nodes").toInt() >= 1);
        QVERIFY(desk.ai.requests.size() >= 1);
        QVERIFY(Graph(*desk.backend->library()).named({}, "character", "Ann") > 0);
        QVERIFY(desk.value("graphBuildState", "text").toString().contains("added"));
        // Asked again, there is nothing left to do, and the button says so by being off.
        desk.find("graphBuildWhich")->setProperty("currentIndex", 2);
        desk.backend->graphJobs()->consider("read", 0, false);
        QTRY_VERIFY(!desk.backend->graphJobs()->planning());
        QCOMPARE(desk.backend->graphJobs()->plan().value("books").toInt(), 0);
        QVERIFY(!desk.value("graphBuildStart", "enabled").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("graphBuild", "opened").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aGraphBuildOutlivesTheBackendThatStartedIt() {
        // The backend goes while a graph build waits on the model. The
        // worker asks through a copy of the backend's factory, so what the
        // factory holds is still there when it answers: the backend's own
        // members are gone by then, and only its child GraphJobs, which
        // waits for the worker, is left.
        QTemporaryDir dir;
        qputenv("XDG_CACHE_HOME", dir.filePath("cache").toUtf8());
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        FakePicker picker;
        QSemaphore asking, gone;
        std::atomic_int calls = 0;
        QString heard;
        // Enough to be kept on the heap, where a destroyed factory would free it.
        const QString why = "No model here: the window that asked has gone.";
        const auto makeAi = [why, &asking, &gone, &calls, &heard](QString *reason) -> std::unique_ptr<AiProvider> {
            // Only the worker's first question waits, for the backend to go.
            if (QThread::currentThread() != qApp->thread() && calls++ == 0) {
                asking.release();
                gone.acquire();
                heard = why;
            }
            if (reason)
                *reason = why;
            return nullptr;
        };
        std::optional<Backend> backend;
        backend.emplace(&settings, &picker, nullptr, makeAi, [](QObject *) { return QList<MetadataSource *>(); },
                        dir.filePath("plugins"), "-");
        QByteArray story;
        for (int line = 0; line < 60; ++line)
            story += "Ann walked the sea wall at dusk and counted the lamps along the harbour, as she did every night.\n\n";
        QVERIFY(Fixtures::write(dir.filePath("The Sea Wall.txt"), story));
        const qint64 id = backend->library()->add(dir.filePath("The Sea Wall.txt")).id;
        backend->library()->setReading(id, "read", 0, {});
        backend->reload();
        GraphJobs *jobs = backend->graphJobs();
        jobs->consider("read", 0, false);
        QTRY_VERIFY(!jobs->planning());
        QCOMPARE(jobs->plan().value("books").toInt(), 1);
        jobs->start();
        QVERIFY(asking.tryAcquire(1, 10000));
        QObject::connect(&*backend, &QObject::destroyed, [&gone] { gone.release(); });
        backend.reset();
        QCOMPARE(heard, why);
    }

    void aNewEngineIsGivenPicturesOfItsOwn() {
        // An engine owns the image provider it is given and deletes it with
        // itself. The next engine (a test's next window) is given a new one,
        // never the one that went with the first.
        Desk desk;
        {
            QQmlEngine first;
            first.addImageProvider("book", desk.reader->imageProvider());
        }
        QQmlEngine second;
        second.addImageProvider("book", desk.reader->imageProvider());
        auto *provider = dynamic_cast<QQuickImageProvider *>(second.imageProvider("book"));
        QVERIFY(provider);
        QSize size;
        // No book is open, so there is no picture, and asking is safe.
        QVERIFY(provider->requestImage("1/cover.jpg", &size, {}).isNull());
    }

    void theReadingSitesFitASmallWindowInEveryLook() {
        // Three sites, each opening to a form and one to a guide, in a
        // window of 640 by 480: the dialog is never larger than the window,
        // what doesn't fit scrolls, and the open row is brought into view.
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                 "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";
        struct Case {
            QByteArray language, colors;
            QSize size;
            const char *shot;
        };
        for (const Case &look : {Case{"en", {}, QSize(640, 480), "sites-small"}, Case{"ar", light, QSize(640, 480), "sites-rtl"},
                                 Case{"nl", {}, QSize(720, 480), "sites-nl"}, Case{"en", light, QSize(1180, 780), "sites-light"}}) {
            Desk desk({}, look.language, look.colors);
            SessionSecrets secrets;
            desk.librarian->siteDesk()->setSecrets(&secrets);
            desk.librarian->siteDesk()->setDownloads(desk.dir.filePath("Downloads"));
            desk.librarian->siteDesk()->setOpener([](const QUrl &) {});
            // Somebody connected by file long ago, so every kind of line shows.
            desk.settings->setGoodreadsUser("12345");
            desk.settings->setSite("goodreads", QJsonObject{{"state", "connected"}, {"name", "Ann Reader"}, {"books", 312}, {"shelves", 3},
                                                            {"synced", QDateTime::currentSecsSinceEpoch() - 50 * 86400}});
            QVERIFY(desk.show());
            desk.window->setMinimumSize(QSize(320, 240));
            desk.window->resize(look.size);
            QTRY_COMPARE(desk.window->size(), look.size);
            QMetaObject::invokeMethod(desk.window, "showReadingSites");
            QTRY_VERIFY(desk.value("readingSites", "opened").toBool());
            QTRY_VERIFY(desk.value("readingSites", "height").toReal() <= desk.window->height());
            QVERIFY(desk.value("readingSites", "width").toReal() <= desk.window->width());
            // Six weeks on, it says so.
            QVERIFY(!desk.value("sitesNudge", "text").toString().isEmpty());
            const QRectF window(0, 0, desk.window->width(), desk.window->height());
            for (const char *scheme : {"goodreads", "openlibrary", "hardcover"}) {
                desk.find("readingSites")->setProperty("current", scheme);
                QQuickItem *row = qobject_cast<QQuickItem *>(desk.find(QString(scheme) + "Row"));
                QVERIFY(row);
                // The open row's head is on screen, and no wider than the dialog.
                QTRY_VERIFY2(window.contains(row->mapRectToScene(QRectF(0, 0, row->width(), row->height()))), scheme);
                QVERIFY(row->width() <= desk.value("readingSites", "width").toReal());
                QVERIFY(desk.find(QString(scheme) + "Identity"));
                desk.shot(QString("%1-%2").arg(look.shot, scheme));
            }
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("readingSites", "visible").toBool());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

private:
    // A second made-up book, whose things the first one's are tied to
    // across books. Returns its two things by name.
    static QHash<QString, qint64> otherBook(Desk &desk, qint64 harbour, qint64 clock, const QHash<QString, qint64> &ids) {
        Graph graph(*desk.backend->library());
        QHash<QString, qint64> made;
        made.insert("Tide-binding", graph.upsert({}, "concept", "Tide-binding", "Knots that hold water still.", {}, GraphPlace{clock, 0, 0.2}));
        made.insert("Hollis Vane", graph.upsert({}, "character", "Hollis Vane", "A clockmaker who never shows her hands.", {}, GraphPlace{clock, 0, 0.4}));
        graph.link(made.value("Hollis Vane"), made.value("Tide-binding"), "teaches", {}, GraphPlace{clock, 0, 0.4});
        graph.link(ids.value("Tide-singing"), made.value("Tide-binding"), GraphEdge::acrossMark() + "echoes",
                   "Both move the sea by a craft that costs its maker.", GraphPlace{harbour, 0, 0.5});
        graph.link(ids.value("The Glass Widow"), made.value("Hollis Vane"), GraphEdge::acrossMark() + "mirrors",
                   "Each rules unseen.", GraphPlace{harbour, 1, 0.5});
        return made;
    }
    // Whether a pane shows these words somewhere.
    static bool says(const GraphParts &graph, const QString &words) {
        QList<QQuickItem *> items{graph.pane};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (!item->isVisible())
                continue;
            if (item->property("text").toString() == words)
                return true;
            items << item->childItems();
        }
        return false;
    }

private slots:
    void theGraphShowsWhatLiesBeyondTheBook() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n";
        struct Look {
            const char *name;
            QByteArray language, colors;
            QSize size;
        };
        const QList<Look> looks{{"dark", "en", {}, QSize(1180, 780)},
                                {"light", "en", light, QSize(1180, 780)},
                                {"rtl", "ar", {}, QSize(1180, 780)},
                                {"small", "en", light, QSize(420, 360)}};
        for (const Look &look : looks) {
            Desk desk({}, look.language, look.colors);
            QVERIFY(Fixtures::epub(desk.dir.filePath("harbour.epub"), "Harbour of Glass"));
            QVERIFY(Fixtures::epub(desk.dir.filePath("clock.epub"), "The Salt Clock"));
            const qint64 harbour = desk.add(desk.dir.filePath("harbour.epub"));
            const qint64 clock = desk.add(desk.dir.filePath("clock.epub"));
            // Both read to the end, so nothing of either is held back.
            desk.backend->library()->setReading(harbour, "read", 4, {});
            desk.backend->library()->setReading(clock, "read", 5, {});
            const QHash<QString, qint64> ids = storyGraph(desk, harbour);
            const QHash<QString, qint64> far = otherBook(desk, harbour, clock, ids);
            const qint64 binding = far.value("Tide-binding");
            desk.backend->reload();
            QVERIFY(desk.show());
            desk.window->resize(look.size);
            QTRY_COMPARE(desk.window->size(), look.size);

            // The graph of the one book, with what it connects to around it.
            desk.key(Qt::Key_6);
            QTRY_VERIFY(desk.find("libraryGraph"));
            const GraphParts graph = graphParts(desk, "libraryGraph");
            QTRY_VERIFY2(graph.pane->isVisible(), look.name);
            graph.browser->showBook(harbour);
            QCOMPARE(graph.browser->beyondCount(), 2);
            QVERIFY(graph.browser->beyond());
            QCOMPARE(graph.browser->nodeCount(), 39 + 2);
            QVERIFY(graph.browser->shown().at(graph.browser->rowOf(binding)).outer);
            QTRY_VERIFY(!graph.browser->settling());
            QTest::qWait(300);
            // The ring is in the picture, outside the book's own things.
            const QRectF picture(0, 0, graph.canvas->width(), graph.canvas->height());
            QRectF inside;
            for (const GraphBrowser::Shown &shown : graph.browser->shown()) {
                const QPointF at = graph.canvas->pointOf(shown.node.id);
                QVERIFY2(picture.contains(at), qPrintable(QString(look.name) + ": " + shown.node.name));
                if (!shown.outer)
                    inside = inside.isNull() ? QRectF(at, QSizeF(1, 1)) : inside.united(QRectF(at, QSizeF(1, 1)));
            }
            QVERIFY2(!inside.contains(graph.canvas->pointOf(binding)), look.name);
            auto *chip = qobject_cast<QQuickItem *>(desk.find("graphBeyond"));
            QVERIFY2(chip && chip->isVisible(), look.name);
            desk.shot(QString("graph-beyond-%1").arg(look.name));

            // X takes the ring away and brings it back; the book's own
            // picture stays exactly where it was.
            QTRY_VERIFY(graph.canvas->hasActiveFocus());
            const QPointF ilse = graph.browser->positions().at(graph.browser->rowOf(ids.value("Ilse Varga")));
            desk.key(Qt::Key_X);
            QTRY_COMPARE(graph.browser->nodeCount(), 39);
            QVERIFY(!graph.browser->beyond());
            QCOMPARE(graph.browser->positions().at(graph.browser->rowOf(ids.value("Ilse Varga"))), ilse);
            desk.key(Qt::Key_X);
            QTRY_COMPARE(graph.browser->nodeCount(), 41);
            QTRY_VERIFY(!graph.browser->settling());
            QCOMPARE(graph.browser->positions().at(graph.browser->rowOf(ids.value("Ilse Varga"))), ilse);
            // The switch above the picture does the same.
            QTest::mouseClick(desk.window, Qt::LeftButton, {}, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
            QTRY_COMPARE(graph.browser->nodeCount(), 39);
            QTest::mouseClick(desk.window, Qt::LeftButton, {}, chip->mapToScene(QPointF(chip->width() / 2, chip->height() / 2)).toPoint());
            QTRY_COMPARE(graph.browser->nodeCount(), 41);
            QTRY_VERIFY(!graph.browser->settling());
            QTest::qWait(300);

            // A tap on a thing of the other book: the details say which
            // book it is from and what ties it to this one, and why.
            QTest::mouseClick(desk.window, Qt::LeftButton, {}, onNode(graph, binding));
            QTRY_COMPARE(graph.browser->selected(), binding);
            QTRY_VERIFY(desk.value("graphDetails", "visible").toBool());
            QCOMPARE(desk.value("graphDetailsName", "text").toString(), QString("Tide-binding"));
            QCOMPARE(desk.value("graphDetailsFrom", "text").toString(), QString("From The Salt Clock"));
            const QVariantMap tie = graph.browser->details().value("relations").toList().first().toMap();
            QCOMPARE(tie.value("relation").toString(), QString("echoes"));
            QCOMPARE(tie.value("otherName").toString(), QString("Tide-singing"));
            QVERIFY(tie.value("note").toString().contains("costs its maker"));
            desk.shot(QString("graph-beyond-%1-selected").arg(look.name));
            // In a window too low for the picture and the details, the
            // details stand in its place and close back to it.
            if (look.size.height() < 400) {
                auto *close = beside(graph, "graphDetailsClose");
                QVERIFY(close && graph.canvas->height() < 1);
                QTest::mouseClick(desk.window, Qt::LeftButton, {}, close->mapToScene(QPointF(close->width() / 2, close->height() / 2)).toPoint());
                QTRY_VERIFY(graph.canvas->height() > 100);
                QCOMPARE(graph.browser->selected(), binding);
            }
            // Seen from this book's own thing, the tie names the other book.
            graph.browser->select(ids.value("Tide-singing"));
            // There the details wait to be asked for: I shows and hides them.
            if (look.size.height() < 400)
                desk.key(Qt::Key_I);
            QTRY_VERIFY2(says(graph, "in The Salt Clock"), look.name);
            if (look.size.height() < 400)
                desk.key(Qt::Key_I);

            // Enter on the outer thing goes to its book's graph, around it;
            // Backspace comes back.
            graph.browser->select(binding);
            QTRY_VERIFY(graph.canvas->hasActiveFocus());
            desk.key(Qt::Key_Return);
            QTRY_COMPARE(graph.browser->bookId(), clock);
            QCOMPARE(desk.value("graphTitle", "text").toString(), QString("The Salt Clock"));
            QCOMPARE(graph.browser->centre(), binding);
            QVERIFY(shownNames(graph.browser).contains("Hollis Vane"));
            QVERIFY(graph.browser->shown().at(graph.browser->rowOf(ids.value("Tide-singing"))).outer);
            desk.key(Qt::Key_Backspace);
            QTRY_COMPARE(graph.browser->bookId(), harbour);
            QCOMPARE(graph.browser->selected(), binding);
            // O opens the book it is from.
            desk.key(Qt::Key_O);
            QTRY_VERIFY(desk.reader->isOpen());
            QCOMPARE(desk.reader->bookId(), clock);
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.reader->isOpen());
            QTRY_VERIFY(graph.pane->isVisible());

            // As a list, what is in other books is a group of its own.
            QTRY_VERIFY(graph.canvas->hasActiveFocus());
            desk.key(Qt::Key_V);
            QTRY_VERIFY(desk.value("graphList", "visible").toBool());
            auto *list = qobject_cast<QQuickItem *>(desk.find("graphList"));
            QMetaObject::invokeMethod(list, "positionViewAtEnd");
            QTRY_VERIFY2(says(graph, "In other books"), look.name);
            QVERIFY(graph.browser->rows().last().toMap().value("outer").toBool());
            desk.shot(QString("graph-beyond-%1-list").arg(look.name));
            desk.key(Qt::Key_V);

            // In the library's graph: only the connections between books.
            graph.browser->showScope();
            QTRY_VERIFY(graph.canvas->hasActiveFocus());
            desk.key(Qt::Key_F);
            QTRY_VERIFY(desk.value("graphPalette", "opened").toBool());
            desk.type("between books");
            desk.key(Qt::Key_Return);
            QTRY_VERIFY(graph.browser->onlyAcross());
            QStringList joined = shownNames(graph.browser);
            joined.sort();
            QCOMPARE(joined, (QStringList{"Harbour of Glass", "Hollis Vane", "The Glass Widow", "The Salt Clock", "Tide-binding", "Tide-singing"}));
            QTRY_VERIFY(!desk.value("graphPalette", "visible").toBool());
            QTRY_VERIFY(!graph.browser->settling());
            desk.shot(QString("graph-across-%1").arg(look.name));
            auto *narrow = qobject_cast<QQuickItem *>(desk.find("graphChip-across"));
            QVERIFY2(narrow && narrow->isVisible(), look.name);
            QTest::mouseClick(desk.window, Qt::LeftButton, {}, narrow->mapToScene(QPointF(narrow->width() / 2, narrow->height() / 2)).toPoint());
            QTRY_VERIFY(!graph.browser->onlyAcross());
            QVERIFY(shownNames(graph.browser).contains("Ilse Varga"));
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!graph.pane->isVisible());

            // Beside the page it stops where the reader is: nothing of the
            // second chapter's connection before the second chapter.
            desk.backend->library()->setReading(harbour, "reading", 0, {});
            desk.reader->openBook(harbour);
            QTRY_VERIFY(desk.reader->isOpen());
            desk.reader->goTo(0, {}, true);
            QTRY_COMPARE(desk.reader->fraction(), 1.0);
            desk.key(Qt::Key_M);
            QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
            const GraphParts beside = graphParts(desk, "readerGraph");
            QTRY_COMPARE(beside.browser->beyondCount(), 1);
            QVERIFY(shownNames(beside.browser).contains("Tide-binding") && !shownNames(beside.browser).contains("Hollis Vane"));
            desk.reader->goTo(1, {}, true);
            QTRY_COMPARE(beside.browser->beyondCount(), 2);
            QTRY_VERIFY(!beside.browser->settling());
            desk.shot(QString("graph-beyond-%1-reader").arg(look.name));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(QString(look.name) + ": " + desk.warnings.join('\n')));
        }
    }

    void aPluginComesFromGitHubAndIsKeptUpToDate() {
        // GitHub is a stand-in on this computer; nothing here reaches the network.
        FakeGitHub github;
        const QString repository = "someone/omabook-storyteller";
        github.push(repository, "main", sha('a'), Made().packed());
        qputenv("OMABOOK_GITHUB_API", github.api().toUtf8());
        const auto restore = qScopeGuard([] { qunsetenv("OMABOOK_GITHUB_API"); });
        Desk desk;
        QVERIFY(desk.show());
        desk.key(Qt::Key_P);
        QTRY_VERIFY(desk.value("plugins", "opened").toBool());
        QObject *dialog = desk.find("plugins");
        // No plugin came from GitHub yet, so nothing offers to check for updates.
        QVERIFY(!desk.value("pluginsCheckAll", "visible").toBool());

        // I, an address, Enter: it is fetched and described, and nothing is
        // installed until it is said how.
        desk.key(Qt::Key_I);
        QTRY_VERIFY(desk.value("pluginInstall", "opened").toBool());
        QTRY_VERIFY(desk.value("pluginInstallAddress", "activeFocus").toBool());
        desk.find("pluginInstallAddress")->setProperty("text", "https://github.com/someone/omabook-storyteller");
        desk.shot("pluginsources-address");
        desk.key(Qt::Key_Return);
        QTRY_COMPARE(desk.value("pluginInstallTitle", "text").toString(), QString("Install Storyteller?"));
        QVERIFY(!desk.value("pluginInstallFetching", "visible").toBool());
        QVERIFY2(desk.value("pluginInstallOrigin", "text").toString().startsWith("From GitHub: someone/omabook-storyteller, main at aaaaaaa"),
                 qPrintable(desk.value("pluginInstallOrigin", "text").toString()));
        QCOMPARE(desk.value("pluginInstallPermissions", "text").toString(), QString("It may use: files in its own folder"));
        QCOMPARE(desk.value("pluginInstallAdds", "text").toString(), QString("It looks to add: commands, hooks on what happens to books"));
        QVERIFY(!QFile::exists(desk.dir.filePath("plugins/storyteller")));
        desk.shot("pluginsources-offer");
        QMetaObject::invokeMethod(desk.find("pluginInstallOn"), "clicked");
        QTRY_VERIFY(!desk.value("pluginInstall", "visible").toBool());
        QVERIFY(QFile::exists(desk.dir.filePath("plugins/storyteller/main.js")));
        QVERIFY(desk.settings->pluginEnabled("folder:storyteller", false));
        QTRY_VERIFY(desk.backend->scripts()->plugin("storyteller"));

        // It is selected, and says where it came from and what it follows.
        QCOMPARE(dialog->property("currentId").toString(), QString("folder:storyteller"));
        QTRY_VERIFY(desk.value("pluginSource", "visible").toBool());
        QVERIFY(desk.value("pluginDetailOrigin", "text").toString().startsWith("GitHub: someone/omabook-storyteller"));
        QCOMPARE(desk.value("pluginInstalled", "text").toString(), QString("1.0 (aaaaaaa)"));
        QVERIFY(desk.value("pluginsCheckAll", "visible").toBool());
        QVERIFY(!desk.value("pluginUpdateNow", "visible").toBool());

        // A newer version that asks for nothing new: U checks every source
        // and it installs itself, staying switched on; it can be rolled back.
        Made newer;
        newer.version = "1.1";
        github.push(repository, "main", sha('b'), newer.packed());
        desk.key(Qt::Key_U);
        QTRY_COMPARE(desk.value("pluginInstalled", "text").toString(), QString("1.1 (bbbbbbb)"));
        QTRY_COMPARE(desk.value("pluginSourceState", "text").toString(), QString("Storyteller was updated from 1.0 to 1.1."));
        QVERIFY(desk.settings->pluginEnabled("folder:storyteller", false));
        QVERIFY(desk.value("pluginRollBack", "visible").toBool());
        desk.shot("pluginsources-updated");

        // One that asks for the network waits: the line at the top says so,
        // and it is shown before it goes in.
        Made greedy;
        greedy.version = "2.0";
        greedy.permissions = QStringList{"files", "http"};
        github.push(repository, "main", sha('c'), greedy.packed());
        desk.key(Qt::Key_U);
        QTRY_COMPARE(desk.value("pluginsHealthText", "text").toString(),
                     QString("Storyteller has an update that asks for the network: review"));
        QCOMPARE(desk.value("pluginInstalled", "text").toString(), QString("1.1 (bbbbbbb)"));
        QTRY_COMPARE(desk.value("pluginUpdateNow", "text").toString(), QString("Review Update…"));
        desk.shot("pluginsources-waits");
        QMetaObject::invokeMethod(desk.find("pluginUpdateNow"), "clicked");
        QTRY_COMPARE(desk.value("pluginInstallTitle", "text").toString(), QString("Update Storyteller to version 2.0?"));
        QTRY_VERIFY(desk.value("pluginInstall", "opened").toBool());
        QVERIFY(desk.value("pluginInstallNew", "text").toString().startsWith("New in this version: it asks to use the network."));
        desk.shot("pluginsources-review");
        // Not now leaves it as it is.
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("pluginInstall", "visible").toBool());
        QCOMPARE(desk.value("pluginInstalled", "text").toString(), QString("1.1 (bbbbbbb)"));
        QMetaObject::invokeMethod(desk.find("pluginUpdateNow"), "clicked");
        QTRY_VERIFY(desk.value("pluginInstallUpdate", "visible").toBool());
        QMetaObject::invokeMethod(desk.find("pluginInstallUpdate"), "clicked");
        QTRY_COMPARE(desk.value("pluginInstalled", "text").toString(), QString("2.0 (ccccccc)"));
        QTRY_VERIFY(!desk.value("pluginsHealth", "visible").toBool());

        // Rolled back by hand, and pinned to stay there; then forgotten, it
        // stays installed and is no longer followed.
        QMetaObject::invokeMethod(desk.find("pluginRollBack"), "clicked");
        QTRY_COMPARE(desk.value("pluginInstalled", "text").toString(), QString("1.1 (bbbbbbb)"));
        github.tag(repository, "v1.0", sha('a'), Made().packed(), false);
        desk.find("pluginPinField")->setProperty("text", "v1.0");
        QMetaObject::invokeMethod(desk.find("pluginPin"), "clicked");
        QTRY_COMPARE(desk.value("pluginFollows", "text").toString(), QString("Stays at v1.0"));
        QTRY_COMPARE(desk.value("pluginInstalled", "text").toString(), QString("1.0 (aaaaaaa)"));
        QVERIFY(!desk.value("pluginCheckNow", "visible").toBool());
        QMetaObject::invokeMethod(desk.find("pluginForgetSource"), "clicked");
        QTRY_VERIFY(!desk.value("pluginSource", "visible").toBool());
        QVERIFY(QFile::exists(desk.dir.filePath("plugins/storyteller/main.js")));
        QVERIFY(!desk.value("pluginsCheckAll", "visible").toBool());

        // An address that isn't a repository says so in the sheet.
        QTRY_VERIFY(dialog->property("activeFocus").toBool());
        desk.key(Qt::Key_I);
        QTRY_VERIFY(desk.value("pluginInstall", "opened").toBool());
        desk.find("pluginInstallAddress")->setProperty("text", "https://example.com/a/b");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.value("pluginInstallError", "text").toString().contains("Only GitHub"));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("pluginInstall", "visible").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("plugins", "visible").toBool());

        // What is done about updates is a setting.
        QMetaObject::invokeMethod(desk.window, "showSettings");
        QTRY_VERIFY(desk.find("pluginUpdates"));
        QCOMPARE(desk.value("pluginUpdates", "currentIndex").toInt(), 0);
        desk.settings->setPluginUpdates("never");
        QTRY_COMPARE(desk.value("pluginUpdates", "currentIndex").toInt(), 2);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void aGraphWriteLeavesTheShelfAlone() {
        // While a book is read in, or a story planned, the knowledge graph is
        // written many times; the shelf must not read the whole library again
        // for each, only the graph views look again.
        Desk desk;
        QVERIFY(Fixtures::epub(desk.dir.filePath("one.epub")));
        const qint64 id = desk.add(desk.dir.filePath("one.epub"));
        QVERIFY(desk.show());
        QSignalSpy shelf(desk.backend.operator->(), &Backend::revisionChanged);
        QSignalSpy graph(GraphFeed::instance(), &GraphFeed::changed);
        {
            Library elsewhere(desk.backend->library()->root());
            QVERIFY(elsewhere.open());
            Graph written(elsewhere);
            const Library::Batch batch(elsewhere);
            for (int i = 0; i < 40; ++i)
                written.upsert({}, "character", QString("Someone %1").arg(i), {}, {}, GraphPlace{id, 0, 0});
        }
        QTRY_VERIFY(graph.count() >= 1);
        QTest::qWait(400);
        QCOMPARE(shelf.count(), 0);
        // A change the shelf shows is still followed.
        {
            Library elsewhere(desk.backend->library()->root());
            QVERIFY(elsewhere.open());
            elsewhere.setProgress(id, {0, 0.5, 50, 0});
        }
        QTRY_VERIFY(shelf.count() >= 1);
        QTRY_COMPARE(qRound(desk.backend->library()->book(id)->percent), 50);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

private:
    // A row of the reading menu, clicked once it has settled where it stands.
    static void clickItem(Desk &desk, const QString &name) {
        QQuickItem *item = nullptr;
        QTRY_VERIFY((item = qobject_cast<QQuickItem *>(desk.find(name))) && item->isVisible());
        QTest::qWait(100);
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    // The page has the keyboard again, not the map.
    static void canvasFocusAway(Desk &desk) {
        if (auto *flow = qobject_cast<QQuickItem *>(desk.find("flowReader")))
            flow->forceActiveFocus();
    }

    // A made-up chapter that names the story graph's people, places and things.
    static bool markedBook(const QString &file) {
        Fixtures::Entries entries = Fixtures::epubEntries("Harbour of Glass");
        for (auto &entry : entries) {
            if (entry.first == "OEBPS/text/ch1.xhtml")
                entry.second = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1>The Night Ferry</h1>"
                               "<p>Ilse Varga rowed the Grey Ferry out of Glass Harbour while Aldo Brenn watched from the wall. "
                               "Tamsin Roe had warned her that the Tide Guild would be waiting, and that Captain Merrow never slept.</p>"
                               "<p>“You will be careful,” said Pell. Ilse laughed, and thought of the Lantern Court and of "
                               "Odalys Quill, who kept its records in Saint Brine's Tower.</p>"
                               "<p>Above them the Upper City slept behind its veil, where the Glass Widow ruled. Brother Anselm "
                               "rang the Tide Bell twice, and the harbour held its breath.</p></body></html>";
            if (entry.first == "OEBPS/text/ch2.xhtml")
                entry.second = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1>Low Water</h1>"
                               "<p>Wick lit the lamps of the Saltmarsh, and Ilse met Nerys Varga at last.</p></body></html>";
        }
        return Fixtures::zip(file, entries);
    }
    // The page's text, its document, and where a name is in it on the screen.
    struct PageText {
        QQuickItem *item = nullptr;
        QTextDocument *document = nullptr;
        QPoint at(const QString &words, int occurrence = 0) const {
            int position = -1;
            for (int i = 0; i <= occurrence; ++i)
                position = int(document->toPlainText().indexOf(words, position + 1));
            QRectF first, last;
            QMetaObject::invokeMethod(item, "positionToRectangle", Q_RETURN_ARG(QRectF, first), Q_ARG(int, position + 1));
            QMetaObject::invokeMethod(item, "positionToRectangle", Q_RETURN_ARG(QRectF, last), Q_ARG(int, position + int(words.size()) - 1));
            return item->mapToScene(QPointF((first.left() + last.right()) / 2, first.center().y())).toPoint();
        }
        QTextCharFormat formatOf(const QString &words) const {
            QTextCursor cursor(document);
            cursor.setPosition(int(document->toPlainText().indexOf(words)) + 1);
            return cursor.charFormat();
        }
    };
    static PageText pageText(Desk &desk) {
        PageText text;
        text.item = qobject_cast<QQuickItem *>(desk.find("flowText"));
        auto *quick = text.item ? text.item->property("textDocument").value<QQuickTextDocument *>() : nullptr;
        text.document = quick ? quick->textDocument() : nullptr;
        return text;
    }

private slots:
    void theGraphsThingsAreMarkedInTheTextAndOpenACard() {
        Desk desk;
        const QString file = desk.dir.filePath("marked.epub");
        QVERIFY(markedBook(file));
        const qint64 book = desk.add(file);
        const QHash<QString, qint64> ids = storyGraph(desk, book);
        QVERIFY(desk.show());
        desk.window->resize(1400, 860);
        desk.settings->setOption("spread", "never");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.reader->goTo(0, {}, true);
        QTRY_COMPARE(desk.reader->fraction(), 1.0);
        PageText page = pageText(desk);
        QVERIFY(page.document);
        // Nothing is marked while the map is closed (the setting is "map").
        QTest::qWait(100);
        QVERIFY(!page.formatOf("Ilse Varga").hasProperty(QTextFormat::TextUnderlineColor));

        // With the map open, the names are marked in their kind's color, the
        // same as the graph's discs.
        const QString before = page.document->toPlainText();
        desk.key(Qt::Key_M);
        QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
        const GraphParts graph = graphParts(desk, "readerGraph");
        QTRY_VERIFY(page.formatOf("Ilse Varga").hasProperty(QTextFormat::TextUnderlineColor));
        const auto inGraph = [&](const QString &kind) {
            return GraphKinds::color(kind, graph.canvas->colors(), graph.canvas->foreground(), graph.canvas->background());
        };
        QCOMPARE(page.formatOf("Ilse Varga").underlineColor(), inGraph("character"));
        QCOMPARE(page.formatOf("Glass Harbour").underlineColor(), inGraph("place"));
        QCOMPARE(page.formatOf("Tide Guild").underlineColor(), inGraph("group"));
        QCOMPARE(page.formatOf("Grey Ferry").underlineColor(), inGraph("item"));
        QCOMPARE(inGraph("character"), QColor(desk.look->colors().value("blue").value<QColor>()));
        // The text is the text it was, so the page didn't move; "will" is
        // an everyday word and no name; Wick and Nerys are not met yet.
        QCOMPARE(page.document->toPlainText(), before);
        QCOMPARE(desk.reader->fraction(), 1.0);
        QVERIFY(!page.formatOf("will be careful").hasProperty(QTextFormat::TextUnderlineColor));
        desk.shot("reader-marks");

        // Pointing at a name shows its card.
        canvasFocusAway(desk);
        QTest::mouseMove(desk.window, page.at("Aldo Brenn"));
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QCOMPARE(desk.value("readingMenu", "mode").toString(), QString("card"));
        QCOMPARE(desk.value("readingMenuName", "text").toString(), QString("Aldo Brenn"));
        QCOMPARE(desk.value("readingMenuKind", "text").toString(), QString("Character"));
        QCOMPARE(desk.value("readingMenuSummary", "text").toString(), QString("The harbourmaster, who taught Ilse the tides."));
        desk.shot("reader-marks-card");
        // Moving away puts it away.
        QTest::mouseMove(desk.window, page.item->mapToScene(QPointF(page.item->width() / 2, page.item->height() + 20)).toPoint());
        QTRY_VERIFY(!desk.value("readingMenu", "visible").toBool());

        // A click keeps it; "Show in graph" selects the thing there.
        QTest::mouseMove(desk.window, page.at("Odalys Quill"));
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, page.at("Odalys Quill"));
        QTRY_VERIFY(!desk.value("readingMenu", "passing").toBool());
        clickItem(desk, "readingAction_graph");
        QTRY_COMPARE(graph.browser->selected(), ids.value("Odalys Quill"));
        QVERIFY(!desk.value("readingMenu", "visible").toBool());
        QVERIFY(desk.ai.requests.isEmpty());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void selectedWordsOpenAMenuByMouseKeyboardAndFinger() {
        Desk desk;
        const QString file = desk.dir.filePath("marked.epub");
        QVERIFY(markedBook(file));
        const qint64 book = desk.add(file);
        const QHash<QString, qint64> ids = storyGraph(desk, book);
        QVERIFY(desk.show());
        desk.window->resize(1400, 860);
        desk.settings->setOption("spread", "never");
        desk.settings->setMarks("always");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.reader->goTo(0, {}, true);
        QTRY_COMPARE(desk.reader->fraction(), 1.0);
        PageText page = pageText(desk);
        // "always": marked with the map closed.
        QTRY_VERIFY(page.formatOf("Tamsin Roe").hasProperty(QTextFormat::TextUnderlineColor));
        QVERIFY(!desk.value("readerGraph", "visible").toBool());

        // A double click on a word selects it, and its menu opens.
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, page.at("Tamsin"));
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QCOMPARE(desk.value("readingMenu", "mode").toString(), QString("menu"));
        QCOMPARE(desk.value("readingMenuName", "text").toString(), QString("Tamsin Roe"));
        // Suggestions from the graph, with no AI call.
        QVERIFY(desk.find("readingAction_question"));
        QVERIFY(desk.find("readingAction_more"));
        desk.shot("reader-menu");
        // Who is it: from the graph, still no AI.
        clickItem(desk, "readingAction_who");
        QTRY_VERIFY(desk.value("readingMenuMet", "visible").toBool());
        QVERIFY(desk.value("readingMenuMet", "text").toString().startsWith("First met in"));
        QVERIFY(desk.ai.requests.isEmpty());
        // Show in graph opens the map, and selects it there.
        clickItem(desk, "readingAction_graph");
        QTRY_VERIFY(desk.value("readerGraph", "visible").toBool());
        const GraphParts graph = graphParts(desk, "readerGraph");
        QTRY_COMPARE(graph.browser->selected(), ids.value("Tamsin Roe"));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("readerGraph", "visible").toBool());

        // The keyboard: V selects the first word on the screen, L moves it on,
        // Shift+L widens it, Enter shows the menu.
        desk.key(Qt::Key_V);
        QTRY_VERIFY(desk.value("selectHint", "visible").toBool());
        desk.key(Qt::Key_J);
        desk.key(Qt::Key_L);
        desk.key(Qt::Key_L, Qt::ShiftModifier);
        const QString chosen = desk.value("flowReader", "selectedText").toString();
        QVERIFY2(chosen.split(' ', Qt::SkipEmptyParts).size() == 2, qPrintable(chosen));
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QVERIFY(!desk.value("selectHint", "visible").toBool());
        // Ask about this: the AI pane, with the selection to ask about.
        QCOMPARE(desk.value("readingMenu", "current").toInt(), 0);
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(desk.value("assistantPane", "visible").toBool());
        QCOMPARE(desk.value("assistantPane", "selection").toString(), chosen);
        QTRY_VERIFY(desk.value("explainSelection", "visible").toBool());
        QVERIFY(desk.value("question", "activeFocus").toBool());
        QVERIFY(desk.ai.requests.isEmpty());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("assistantPane", "visible").toBool());

        // A word the graph doesn't know: "Add to the graph", and who it is
        // goes to the AI with only its paragraph.
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, page.at("harbour held"));
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QVERIFY(desk.value("readingMenuWords", "visible").toBool());
        QVERIFY(desk.find("readingAction_add"));
        clickItem(desk, "readingAction_who");
        QTRY_COMPARE(desk.ai.requests.size(), 1);
        QVERIFY(desk.ai.requests.first().messages.last().text.contains("the harbour held its breath"));
        QVERIFY(!desk.ai.requests.first().messages.last().text.contains("Grey Ferry"));
        QTRY_VERIFY(!desk.assistant->busy());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("assistantPane", "visible").toBool());

        // A finger: a tap on a name shows its card, a long press selects a word.
        QPointingDevice *finger = QTest::createTouchDevice();
        const QPoint pell = page.at("Pell");
        QTest::touchEvent(desk.window, finger).press(0, pell, desk.window);
        QTest::touchEvent(desk.window, finger).release(0, pell, desk.window);
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QCOMPARE(desk.value("readingMenuName", "text").toString(), QString("Pell"));
        QTest::touchEvent(desk.window, finger).press(0, pell, desk.window);
        QTest::touchEvent(desk.window, finger).release(0, pell, desk.window);
        QTRY_VERIFY(!desk.value("readingMenu", "visible").toBool());
        const QPoint laughed = page.at("laughed");
        QTest::touchEvent(desk.window, finger).press(0, laughed, desk.window);
        QTest::qWait(1100);
        QTest::touchEvent(desk.window, finger).release(0, laughed, desk.window);
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QCOMPARE(desk.value("readingMenu", "words").toString(), QString("laughed"));
        desk.shot("reader-menu-touch");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void anAnswersNamesAreMarkedAndLeadBackToTheBook() {
        Desk desk;
        const QString file = desk.dir.filePath("marked.epub");
        QVERIFY(markedBook(file));
        const qint64 book = desk.add(file);
        const QHash<QString, qint64> ids = storyGraph(desk, book);
        QVERIFY(desk.show());
        desk.window->resize(1400, 860);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.reader->goTo(1, {}, true);
        QTRY_COMPARE(desk.reader->section(), 1);
        desk.ai.answer = "**Ilse Varga**: pilots the night ferry.\n\n**Nerys Varga**: her mother, back at last.";
        desk.key(Qt::Key_W);
        QTRY_VERIFY(desk.value("assistantPane", "visible").toBool());
        QTRY_VERIFY(!desk.assistant->busy());
        // The answer's names are links in their kinds' colors.
        QQuickItem *answer = nullptr;
        QTRY_VERIFY([&] {
            QList<QQuickItem *> items{qobject_cast<QQuickItem *>(desk.find("conversation"))};
            while (!items.isEmpty()) {
                QQuickItem *item = items.takeFirst();
                if (item->property("text").toString().contains("omabook-thing:"))
                    answer = item;
                items << item->childItems();
            }
            return answer != nullptr;
        }());
        QVERIFY(answer->property("text").toString().contains(QString("omabook-thing:%1").arg(ids.value("Nerys Varga"))));
        desk.shot("reader-answer-marks");
        // Clicked, it shows the card, which goes to where the thing was first met.
        QMetaObject::invokeMethod(answer, "linkActivated", Q_ARG(QString, QString("omabook-thing:%1").arg(ids.value("Ilse Varga"))));
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QCOMPARE(desk.value("readingMenuName", "text").toString(), QString("Ilse Varga"));
        clickItem(desk, "readingAction_met");
        QTRY_COMPARE(desk.reader->section(), 0);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theMarksAndTheMenuInALightThemeAndRightToLeft() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                 "selection_background = \"#dcdcdc\"\ncolor1 = \"#c4314b\"\ncolor2 = \"#2f7d32\"\ncolor3 = \"#9a6700\"\n"
                                 "color4 = \"#2459c4\"\ncolor5 = \"#8a3ab9\"\ncolor6 = \"#0f7c8c\"\ncolor8 = \"#6f6f6f\"\n";
        for (const QByteArray &language : {QByteArray("en"), QByteArray("ar")}) {
            Desk desk({}, language, light);
            const QString file = desk.dir.filePath("marked.epub");
            QVERIFY(markedBook(file));
            const qint64 book = desk.add(file);
            storyGraph(desk, book);
            QVERIFY(desk.show());
            desk.window->resize(1400, 860);
            desk.settings->setOption("spread", "never");
            desk.reader->openBook(book);
            QTRY_VERIFY(desk.reader->isOpen());
            desk.reader->goTo(0, {}, true);
            QTRY_COMPARE(desk.reader->fraction(), 1.0);
            desk.key(Qt::Key_M);
            PageText page = pageText(desk);
            QTRY_VERIFY(page.formatOf("Ilse Varga").hasProperty(QTextFormat::TextUnderlineColor));
            QCOMPARE(page.formatOf("Ilse Varga").underlineColor(), QColor("#2459c4"));
            canvasFocusAway(desk);
            QTest::mouseDClick(desk.window, Qt::LeftButton, {}, page.at("Merrow"));
            QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
            desk.shot(language == "en" ? "reader-marks-light" : "reader-marks-rtl");
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("readingMenu", "visible").toBool());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    // Themes: which one the colors come from (themechoice.h), and the picker.
    // Omarchy, Aether and Omabook's own themes are folders of a temp tree.
private:
    struct ThemeHome {
        QTemporaryDir dir;
        ThemePlaces places() const {
            return {dir.filePath("state/omarchy/current"), dir.filePath("config/omarchy/themes"), dir.filePath("config/aether"),
                    dir.filePath("omabook/themes")};
        }
        // Omarchy switched to a theme: its colors and its name.
        bool omarchy(const QByteArray &name, const QByteArray &colors) const {
            return QDir().mkpath(places().omarchy + "/theme") && write(places().omarchy + "/theme/colors.toml", colors)
                   && write(places().omarchy + "/theme.name", name + "\n");
        }
        // Aether writing its theme without Omarchy: to a temp file, then
        // renamed over the old one, as a careful writer does.
        bool aether(const QByteArray &colors) const {
            const QString folder = places().aether + "/theme";
            if (!QDir().mkpath(folder) || !write(folder + "/.colors.toml.new", colors))
                return false;
            QFile::remove(folder + "/colors.toml");
            return QFile::rename(folder + "/.colors.toml.new", folder + "/colors.toml");
        }
        bool own(const QString &id, const QByteArray &colors) const {
            return QDir().mkpath(places().user + "/" + id) && write(places().user + "/" + id + "/colors.toml", colors);
        }
    };
    static QByteArray palette(const char *background, const char *accent, const char *mode = "dark") {
        return QByteArray("mode = \"") + mode + "\"\nbackground = \"" + background + "\"\nforeground = \"#e0e0e0\"\naccent = \"" + accent
               + "\"\nmuted = \"#808080\"\nred = \"#ff0000\"\n";
    }
    static void clickOn(Desk &desk, const QString &name) {
        auto *item = qobject_cast<QQuickItem *>(desk.find(name));
        QVERIFY2(item && item->isVisible(), qPrintable(name));
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
    }
    static int optionIndex(const ThemeChoice &choice, const QString &wanted) {
        const QVariantList options = choice.options();
        for (int i = 0; i < options.size(); ++i)
            if (options.at(i).toMap().value("choice").toString() == wanted)
                return i;
        return -1;
    }
    static QString styleTheme(const Desk &desk) {
        QFile file(desk.dir.filePath("custom/style.json"));
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().value("theme").toString() : QString();
    }

private slots:
    void automaticIsOmarchyThenAetherThenTokyoNight() {
        ThemeHome home;
        ThemeSource source = resolveTheme("auto", home.places());
        QCOMPARE(source.kind, QString("bundled"));
        QCOMPARE(source.id, QString("tokyo-night"));
        QCOMPARE(source.name, QString("Tokyo Night"));
        QCOMPARE(source.path, QString(":/themes/tokyo-night/colors.toml"));
        QVERIFY(source.note.isEmpty());
        QVERIFY(source.why.contains("neither Omarchy nor Aether"));
        // An empty choice, as when style.json says nothing, is the same.
        QCOMPARE(resolveTheme("", home.places()), source);

        QVERIFY(home.aether(palette("#101010", "#00aaff")));
        source = resolveTheme("auto", home.places());
        QCOMPARE(source.kind, QString("aether"));
        QCOMPARE(source.path, home.places().aether + "/theme/colors.toml");

        QVERIFY(home.omarchy("last-horizon", palette("#202020", "#ff8800")));
        source = resolveTheme("auto", home.places());
        QCOMPARE(source.choice, QString("auto"));
        QCOMPARE(source.kind, QString("omarchy"));
        QCOMPARE(source.name, QString("Last Horizon"));
        QCOMPARE(source.path, home.places().omarchy + "/theme/colors.toml");
    }

    void eachChoiceResolvesOrFallsBackAndSaysSo() {
        ThemeHome home;
        // Asked for, but not on this computer: Tokyo Night, and why.
        for (const char *missing : {"omarchy", "aether"}) {
            const ThemeSource source = resolveTheme(missing, home.places());
            QCOMPARE(source.choice, QString(missing));
            QCOMPARE(source.id, QString("tokyo-night"));
            QVERIFY2(!source.note.isEmpty(), missing);
            QVERIFY(Themes::problem(missing, home.places()).isEmpty());
        }
        ThemeSource source = resolveTheme("No Such Theme", home.places());
        QCOMPARE(source.id, QString("tokyo-night"));
        QVERIFY(source.note.contains("No Such Theme"));
        QVERIFY(!Themes::problem("No Such Theme", home.places()).isEmpty());

        // A bundled theme by its id or its name.
        source = resolveTheme("Catppuccin Latte", home.places());
        QCOMPARE(source.kind, QString("bundled"));
        QCOMPARE(source.id, QString("catppuccin-latte"));
        QVERIFY(Themes::palette(source.path, home.places()).light);
        QCOMPARE(resolveTheme("nord", home.places()).path, QString(":/themes/nord/colors.toml"));
        // One of the user's own shadows the bundled one of that name.
        QVERIFY(home.own("nord", palette("#111111", "#123456")));
        source = resolveTheme("nord", home.places());
        QCOMPARE(source.kind, QString("user"));
        QCOMPARE(Themes::palette(source.path, home.places()).colors.value("accent"), QColor("#123456"));

        // A path: to the file, or to the folder holding it.
        QVERIFY(QDir().mkpath(home.dir.filePath("elsewhere/paper")));
        QVERIFY(write(home.dir.filePath("elsewhere/paper/colors.toml"), palette("#fafafa", "#005fcc", "light")));
        for (const QString &path : {home.dir.filePath("elsewhere/paper/colors.toml"), home.dir.filePath("elsewhere/paper")}) {
            source = resolveTheme(path, home.places());
            QCOMPARE(source.kind, QString("file"));
            QCOMPARE(source.name, QString("Paper"));
            QCOMPARE(source.path, home.dir.filePath("elsewhere/paper/colors.toml"));
            QVERIFY(Themes::problem(path, home.places()).isEmpty());
        }
        source = resolveTheme(home.dir.filePath("nowhere/colors.toml"), home.places());
        QCOMPARE(source.id, QString("tokyo-night"));
        QVERIFY(source.note.contains("nowhere"));
        QVERIFY(!Themes::problem(home.dir.filePath("nowhere/colors.toml"), home.places()).isEmpty());

        // Aether on Omarchy writes a theme into Omarchy and switches to it.
        QVERIFY(home.omarchy("gruvbox", palette("#282828", "#d79921")));
        QVERIFY(QDir().mkpath(home.places().omarchyThemes + "/aether"));
        QVERIFY(write(home.places().omarchyThemes + "/aether/colors.toml", palette("#0a0a0a", "#aa00ff")));
        QCOMPARE(resolveTheme("aether", home.places()).path, home.places().omarchyThemes + "/aether/colors.toml");
        QCOMPARE(resolveTheme("omarchy", home.places()).name, QString("Gruvbox"));
        QVERIFY(home.omarchy("aether", palette("#0a0a0a", "#aa00ff")));
        QCOMPARE(resolveTheme("aether", home.places()).path, home.places().omarchy + "/theme/colors.toml");
        QCOMPARE(resolveTheme("auto", home.places()).name, QString("Aether"));
    }

    void theUsersColorsWinOverTheThemeWhichWinsOverTokyoNight() {
        ThemeHome home;
        QTemporaryDir config, desktop;
        // A theme of the user's, in Omabook's folder, that names only a few colors.
        ThemePlaces places = home.places();
        places.user = config.filePath("themes");
        QVERIFY(QDir().mkpath(places.user + "/sparse"));
        QVERIFY(write(places.user + "/sparse/colors.toml", "background = \"#102030\"\naccent = \"#405060\"\n"));
        QVERIFY(write(config.filePath("style.json"), R"({"theme": "sparse", "colors": {"accent": "#abcdef"}})"));
        Customization custom(config.path());
        QVERIFY(!QFile::exists(config.filePath("style.error")));
        Theme theme(desktop.path());
        ThemeChoice choice(&custom, places);
        Look look(&theme, &choice, &custom);
        QCOMPARE(choice.choice(), QString("sparse"));
        QCOMPARE(look.accent(), QColor("#abcdef"));       // style.json
        QCOMPARE(look.background(), QColor("#102030"));   // the theme
        const Theme::Palette tokyo = Theme::readThemeColors(":/themes/tokyo-night/colors.toml");
        QCOMPARE(look.colors().value("cyan").value<QColor>(), tokyo.colors.value("cyan")); // Tokyo Night
        QCOMPARE(look.foreground(), tokyo.colors.value("foreground"));
        QVERIFY(!look.light());
        // mode in style.json wins over the theme's.
        QVERIFY(write(config.filePath("style.json"), R"({"theme": "white", "mode": "dark"})"));
        custom.check();
        QCOMPARE(look.background(), Theme::readThemeColors(":/themes/white/colors.toml").colors.value("background"));
        QVERIFY(!look.light());
        QVERIFY(write(config.filePath("style.json"), R"({"theme": "white"})"));
        custom.check();
        QVERIFY(look.light());
        // A theme that doesn't exist is refused, and the last good one stays.
        QVERIFY(write(config.filePath("style.json"), R"({"theme": "plaid"})"));
        QVERIFY(custom.check().contains("plaid"));
        QCOMPARE(choice.choice(), QString("white"));
        QVERIFY(look.light());
        // With no bundled themes at all, Theme's built-in fallback is the floor.
        ThemePlaces bare = places;
        bare.bundled = home.dir.filePath("none");
        QVERIFY(write(config.filePath("style.json"), "{}"));
        custom.check();
        ThemeChoice nothing(&custom, bare);
        Look floor(&theme, &nothing, &custom);
        QCOMPARE(floor.accent(), QColor("#FFD60A"));
    }

    void theChoiceFollowsOmarchyAndAetherAsTheyChange() {
        ThemeHome home;
        QTemporaryDir config;
        Customization custom(config.path());
        ThemeChoice choice(&custom, home.places());
        QSignalSpy changed(&choice, &ThemeChoice::changed);
        QCOMPARE(choice.source().id, QString("tokyo-night"));
        QCOMPARE(choice.options().size(), 1 + 23);

        // Aether arrives, then rewrites its file: followed, once each.
        QVERIFY(home.aether(palette("#101010", "#00aaff")));
        QTRY_COMPARE(choice.source().kind, QString("aether"));
        QCOMPARE(choice.palette().colors.value("accent"), QColor("#00aaff"));
        QTRY_COMPARE(optionIndex(choice, "aether"), 1);
        changed.clear();
        QVERIFY(home.aether(palette("#101010", "#ff00aa")));
        QTRY_COMPARE(choice.palette().colors.value("accent"), QColor("#ff00aa"));
        QTest::qWait(250);
        QCOMPARE(changed.count(), 1);

        // Omarchy installed while Omabook runs takes over Automatic.
        QVERIFY(home.omarchy("kanagawa", palette("#1f1f28", "#7e9cd8")));
        QTRY_COMPARE(choice.source().kind, QString("omarchy"));
        QCOMPARE(choice.source().name, QString("Kanagawa"));
        QCOMPARE(choice.palette().colors.value("accent"), QColor("#7e9cd8"));
        QVERIFY(home.omarchy("kanagawa", palette("#1f1f28", "#c0a36e")));
        QTRY_COMPARE(choice.palette().colors.value("accent"), QColor("#c0a36e"));

        // A theme chosen in style.json stops the following.
        QVERIFY(write(config.filePath("style.json"), R"({"theme": "nord"})"));
        QTRY_COMPARE(choice.source().id, QString("nord"));
        const QColor nord = choice.palette().colors.value("accent");
        QVERIFY(home.omarchy("kanagawa", palette("#1f1f28", "#00ff00")));
        QTest::qWait(300);
        QCOMPARE(choice.palette().colors.value("accent"), nord);

        // A colors.toml of the user's, wherever it is, is followed too.
        QVERIFY(QDir().mkpath(home.dir.filePath("mine")));
        QVERIFY(write(home.dir.filePath("mine/colors.toml"), palette("#000000", "#111111")));
        QVERIFY(choice.choose(home.dir.filePath("mine/colors.toml")).isEmpty());
        QCOMPARE(choice.palette().colors.value("accent"), QColor("#111111"));
        QVERIFY(write(home.dir.filePath("mine/colors.toml"), palette("#000000", "#222222")));
        QTRY_COMPARE(choice.palette().colors.value("accent"), QColor("#222222"));
    }

    void everyBundledThemeParsesAndStaysReadable() {
        ThemeHome home;
        const QStringList ids = QDir(":/themes").entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(ids.size(), 23);
        QVERIFY(QFile::exists(":/themes/LICENSE.omarchy"));
        QVERIFY(QFile::exists(":/themes/README"));
        const QStringList names{"accent", "background", "foreground", "selection", "muted", "red",
                                "green",  "yellow",     "orange",     "blue",      "magenta", "cyan"};
        QStringList problems;
        for (const QString &id : ids) {
            const QString path = ":/themes/" + id + "/colors.toml";
            const Theme::Palette own = Theme::readThemeColors(path);
            if (own.colors.size() < 8)
                problems << id + ": doesn't parse";
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(file.readAll());
            if (text.contains("mode = \"light\"") != own.light)
                problems << id + ": light isn't what its mode says";
            // Every name Omabook draws with, from the theme or after aliases.
            for (const QString &name : names)
                if (!own.colors.contains(name) && !(name == "orange" && own.colors.contains("yellow")))
                    problems << id + ": no " + name;
            const Theme::Palette shown = Themes::palette(path, home.places());
            const QColor background = shown.colors.value("background"), foreground = shown.colors.value("foreground"),
                         muted = shown.colors.value("muted"), accent = shown.colors.value("accent"),
                         selection = shown.colors.value("selection");
            const QColor onAccent = relativeLuminance(accent) > .179 ? QColor("black") : QColor("white");
            const auto check = [&](const char *what, const QColor &a, const QColor &b, double least) {
                if (Themes::contrast(a, b) < least)
                    problems << QString("%1: %2 is %3:1, under %4:1").arg(id, what).arg(Themes::contrast(a, b), 0, 'f', 2).arg(least);
            };
            check("text on the background", foreground, background, 4.5);
            check("text on the selection", foreground, selection, 4.5);
            check("muted text on the background", muted, background, 4.5);
            check("muted text on the selection", muted, selection, 3.0);
            check("a caption on the accent", onAccent, accent, 4.5);
            // Readable, and still the theme's muted where it already was.
            if (Themes::contrast(own.colors.value("muted"), background) >= 4.5
                && Themes::contrast(own.colors.value("muted"), selection) >= 3.0 && muted != own.colors.value("muted"))
                problems << id + ": a readable muted was changed";
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void anAetherBlueprintBecomesATheme() {
        ThemeHome home;
        QJsonArray colors;
        for (const char *color : {"#101418", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#dcdfe4",
                                  "#5c6370", "#e06c75", "#98c379", "#e5c07b", "#61afef", "#c678dd", "#56b6c2", "#ffffff"})
            colors << color;
        // Aether's default selection: the foreground and background swapped.
        QJsonObject palette{{"colors", colors}, {"mode", "normal"}, {"lightMode", false},
                            {"extendedColors", QJsonObject{{"accent", "#ffaa00"}, {"selection_background", "#dcdfe4"},
                                                           {"selection_foreground", "#101418"}}},
                            {"nativeColors", QJsonObject{{"cyan", "#00ffee"}, {"nonsense", "#123456"}}}};
        QVERIFY(QDir().mkpath(home.places().aether + "/blueprints"));
        QVERIFY(write(home.places().aether + "/blueprints/harbour.json",
                      QJsonDocument(QJsonObject{{"name", "Harbour Dusk"}, {"palette", palette}}).toJson()));
        QString problem;
        const QString id = Themes::importBlueprint("Harbour Dusk", home.places(), &problem);
        QVERIFY2(id == "harbour-dusk", qPrintable(problem));
        const Theme::Palette read = Theme::readThemeColors(home.places().user + "/harbour-dusk/colors.toml");
        QCOMPARE(read.colors.value("background"), QColor("#101418"));
        QCOMPARE(read.colors.value("red"), QColor("#e06c75"));
        QCOMPARE(read.colors.value("foreground"), QColor("#dcdfe4"));
        QCOMPARE(read.colors.value("muted"), QColor("#5c6370"));
        QCOMPARE(read.colors.value("accent"), QColor("#ffaa00"));
        QCOMPARE(read.colors.value("cyan"), QColor("#00ffee"));
        QVERIFY(!read.light);
        // The swapped pair gives way to the background a little lighter.
        QVERIFY(read.colors.value("selection") != QColor("#dcdfe4"));
        QVERIFY(read.colors.value("selection").lightnessF() > QColor("#101418").lightnessF());
        QVERIFY(read.colors.value("orange").lightnessF() > QColor("#e06c75").lightnessF());
        QCOMPARE(resolveTheme("harbour-dusk", home.places()).kind, QString("user"));
        // By its file, too; a light one says so.
        palette.insert("lightMode", true);
        palette.insert("extendedColors", QJsonObject{});
        QVERIFY(write(home.dir.filePath("Paper Day.json"), QJsonDocument(QJsonObject{{"palette", palette}}).toJson()));
        QCOMPARE(Themes::importBlueprint(home.dir.filePath("Paper Day.json"), home.places(), &problem), QString("paper-day"));
        QVERIFY(Theme::readThemeColors(home.places().user + "/paper-day/colors.toml").light);
        QCOMPARE(Theme::readThemeColors(home.places().user + "/paper-day/colors.toml").colors.value("accent"), QColor("#61afef"));

        // What isn't a blueprint says why.
        QVERIFY(write(home.dir.filePath("broken.json"), "{ not json"));
        QVERIFY(Themes::importBlueprint(home.dir.filePath("broken.json"), home.places(), &problem).isEmpty());
        QVERIFY(problem.contains("isn't an Aether blueprint"));
        colors.removeLast();
        QVERIFY(write(home.dir.filePath("short.json"), QJsonDocument(QJsonObject{{"palette", QJsonObject{{"colors", colors}}}}).toJson()));
        QVERIFY(Themes::importBlueprint(home.dir.filePath("short.json"), home.places(), &problem).isEmpty());
        QVERIFY(problem.contains("15"));
        QVERIFY(Themes::importBlueprint("nothing-here", home.places(), &problem).isEmpty());
        QVERIFY(problem.contains("no Aether blueprint"));
    }

    void thePickerPreviewsByKeyboardAndEscapePutsItBack() {
        ThemeHome home;
        QVERIFY(home.omarchy("catppuccin", "mode = \"dark\"\nbackground = \"#1e1e2e\"\nforeground = \"#cdd6f4\"\n"
                                           "accent = \"#89b4fa\"\nmuted = \"#6c7086\"\nselection = \"#313244\"\n"));
        Desk desk;
        ThemeChoice choice(&*desk.custom, home.places());
        desk.look.emplace(&*desk.theme, &choice, &*desk.custom);
        const auto closing = qScopeGuard([&desk] {
            desk.interface.reset();
            desk.engine.reset();
        });
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.window->resize(1180, 780);
        QCOMPARE(desk.look->background(), QColor("#1e1e2e"));
        QCOMPARE(desk.window->color(), QColor("#1e1e2e"));

        // The settings say what Automatic came to, and open the picker.
        QMetaObject::invokeMethod(desk.window, "showSettings");
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        QCOMPARE(desk.value("themeInUse", "text").toString(), QString("Automatic: Omarchy — Catppuccin"));
        desk.shot("theme-settings");
        clickOn(desk, "chooseTheme");
        QTRY_VERIFY(desk.value("themePicker", "opened").toBool());
        QTRY_VERIFY(!desk.value("settings", "visible").toBool());
        QCOMPARE(desk.value("themeList", "currentIndex").toInt(), 0);
        desk.shot("theme-picker-dark");

        // Down the list, each theme on the window as it is reached.
        const int nord = optionIndex(choice, "nord");
        QVERIFY(nord > 1);
        for (int i = 0; i < nord; ++i)
            desk.key(Qt::Key_Down);
        QCOMPARE(desk.value("themeList", "currentIndex").toInt(), nord);
        QCOMPARE(choice.previewing(), QString("nord"));
        const QColor nordBackground = Theme::readThemeColors(":/themes/nord/colors.toml").colors.value("background");
        QCOMPARE(desk.look->background(), nordBackground);
        QTRY_COMPARE(desk.window->color(), nordBackground);
        // Escape puts back what there was, and nothing is written.
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("themePicker", "opened").toBool());
        QCOMPARE(choice.previewing(), QString());
        QCOMPARE(desk.look->background(), QColor("#1e1e2e"));
        QVERIFY(!QFile::exists(desk.dir.filePath("custom/style.json")));

        // A light one, kept with Enter: written down, and light it stays.
        QMetaObject::invokeMethod(desk.window, "showThemes");
        QTRY_VERIFY(desk.value("themePicker", "opened").toBool());
        const int white = optionIndex(choice, "white");
        for (int i = 0; i < white; ++i)
            desk.key(Qt::Key_Down);
        QVERIFY(desk.look->light());
        desk.shot("theme-picker-light");
        desk.key(Qt::Key_Return);
        QTRY_VERIFY(!desk.value("themePicker", "opened").toBool());
        QCOMPARE(styleTheme(desk), QString("white"));
        QCOMPARE(choice.choice(), QString("white"));
        QVERIFY(desk.look->light());
        QCOMPARE(desk.window->property("color").value<QColor>(), Theme::readThemeColors(":/themes/white/colors.toml").colors.value("background"));
        // Once it is gone: a popup gives the keyboard back when its closing ends.
        QTRY_VERIFY(!desk.value("themePicker", "visible").toBool());
        QMetaObject::invokeMethod(desk.window, "showSettings");
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        QCOMPARE(desk.value("themeInUse", "text").toString(), QString("White"));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("settings", "visible").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void thePickerTakesAPointerToo() {
        ThemeHome home;
        Desk desk;
        ThemeChoice choice(&*desk.custom, home.places());
        desk.look.emplace(&*desk.theme, &choice, &*desk.custom);
        const auto closing = qScopeGuard([&desk] {
            desk.interface.reset();
            desk.engine.reset();
        });
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.window->resize(1180, 780);
        // No Omarchy and no Aether here: Automatic is Tokyo Night, and only it is offered beside the themes.
        QCOMPARE(optionIndex(choice, "omarchy"), -1);
        QCOMPARE(optionIndex(choice, "aether"), -1);
        QMetaObject::invokeMethod(desk.window, "showThemes");
        QTRY_VERIFY(desk.value("themePicker", "opened").toBool());
        QObject *list = desk.find("themeList");
        QVERIFY(list);
        // A click shows a theme, Use Theme keeps it.
        const int gruvbox = optionIndex(choice, "gruvbox");
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, gruvbox), Q_ARG(int, 1 /* ListView.Center */));
        QTest::qWait(50);
        clickOn(desk, "theme-gruvbox");
        QTRY_COMPARE(choice.previewing(), QString("gruvbox"));
        clickOn(desk, "themeUse");
        QTRY_VERIFY(!desk.value("themePicker", "opened").toBool());
        QCOMPARE(styleTheme(desk), QString("gruvbox"));
        // A double click keeps one at once; Automatic takes the key out again.
        QMetaObject::invokeMethod(desk.window, "showThemes");
        QTRY_VERIFY(desk.value("themePicker", "opened").toBool());
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, 0), Q_ARG(int, 1));
        QTest::qWait(50);
        auto *automatic = qobject_cast<QQuickItem *>(desk.find("theme-auto"));
        QVERIFY(automatic);
        QTest::mouseDClick(desk.window, Qt::LeftButton, {}, automatic->mapToScene(QPointF(automatic->width() / 2, automatic->height() / 2)).toPoint());
        QTRY_VERIFY(!desk.value("themePicker", "opened").toBool());
        QCOMPARE(choice.choice(), QString("auto"));
        QVERIFY(QFile::exists(desk.dir.filePath("custom/style.json")));
        QCOMPARE(styleTheme(desk), QString());
        // Cancel keeps what there was.
        QMetaObject::invokeMethod(desk.window, "showThemes");
        QTRY_VERIFY(desk.value("themePicker", "opened").toBool());
        desk.key(Qt::Key_Down);
        QVERIFY(!choice.previewing().isEmpty());
        clickOn(desk, "themeCancel");
        QTRY_VERIFY(!desk.value("themePicker", "opened").toBool());
        QCOMPARE(choice.previewing(), QString());
        QCOMPARE(choice.source().id, QString("tokyo-night"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theShelfInBundledThemes() {
        // For looking over (OMABOOK_SHOTS): the shelf in a dark, a light and
        // a warm theme, and the picker over it.
        ThemeHome home;
        Desk desk;
        ThemeChoice choice(&*desk.custom, home.places());
        desk.look.emplace(&*desk.theme, &choice, &*desk.custom);
        const auto closing = qScopeGuard([&desk] {
            desk.interface.reset();
            desk.engine.reset();
        });
        QVERIFY(!stock(desk).isEmpty());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.window->resize(1180, 780);
        for (const char *id : {"catppuccin", "flexoki-light", "gruvbox"}) {
            QVERIFY(choice.choose(id).isEmpty());
            QTRY_COMPARE(desk.window->color(), Theme::readThemeColors(QString(":/themes/") + id + "/colors.toml").colors.value("background"));
            desk.shot(QString("shelf-") + id);
        }
        QMetaObject::invokeMethod(desk.window, "showThemes");
        QTRY_VERIFY(desk.value("themePicker", "opened").toBool());
        for (int i = 0; i < 3; ++i)
            desk.key(Qt::Key_Down);
        desk.shot("themes");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("themePicker", "opened").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The reading format: the "Aa" panel beside the page, and the same
    // controls in the settings (readingformat.h).
private:
    // A chapter long enough to scroll, which names the story graph's
    // people, and has one paragraph in Arabic to read right to left.
    static bool longBook(const QString &file) {
        Fixtures::Entries entries = Fixtures::epubEntries("The Long Tide");
        QString body = "<h1>The Long Tide</h1>";
        for (int i = 1; i <= 70; ++i)
            body += QString("<p>Paragraph %1. Ilse Varga rowed out past the wall while Aldo Brenn kept the lamps; the tide "
                            "turned slowly under the ferry and the harbour went on breathing in the dark.</p>").arg(i);
        body += QString::fromUtf8("<p>ذهبت إيلسه إلى الميناء في الليل وكان البحر هادئا والمصابيح مضاءة على الجدار.</p>");
        body += "<p>Short last line.</p>";
        for (auto &entry : entries)
            if (entry.first == "OEBPS/text/ch1.xhtml")
                entry.second = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>" + body.toUtf8() + "</body></html>";
        return Fixtures::zip(file, entries);
    }
    static QTextBlock blockOf(const PageText &page, const QString &words) {
        return page.document->find(words).block();
    }
    // How far down the chapter the page is, as the view has it.
    static double through(Desk &desk) {
        QObject *flow = desk.find("flowReader");
        return flow && flow->property("range").toReal() > 0 ? flow->property("contentY").toReal() / flow->property("range").toReal() : -1;
    }

private slots:
    void theTextIsSetLiveFromAPanelAndThePlaceIsKept() {
        Desk desk;
        const QString file = desk.dir.filePath("long.epub");
        QVERIFY(longBook(file));
        const qint64 book = desk.add(file);
        storyGraph(desk, book);
        QVERIFY(desk.show());
        desk.window->resize(1400, 860);
        desk.settings->setOption("spread", "never");
        desk.settings->setMarks("always");
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        desk.reader->goToPlace(0, 0.5);
        QTRY_VERIFY(qAbs(through(desk) - 0.5) < 0.02);
        PageText page = pageText(desk);
        QVERIFY(page.document);
        QTRY_VERIFY(page.formatOf("Aldo Brenn").hasProperty(QTextFormat::TextUnderlineColor));
        QCOMPARE(blockOf(page, "Paragraph 40.").blockFormat().lineHeight(), 150.0);

        // Its key opens it beside the page, with the keyboard in it.
        desk.key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(desk.value("formatSide", "visible").toBool());
        QTRY_VERIFY(desk.value("formatPane", "focused").toBool());
        QVERIFY(!desk.value("formatSheet", "visible").toBool());
        QTRY_VERIFY(qAbs(through(desk) - 0.5) < 0.03);
        const double place = desk.reader->fraction();

        // More space between the lines: the page shows it at once, at the same place.
        for (int i = 0; i < 3; ++i)
            clickItem(desk, "format_line_height_more");
        QTRY_COMPARE(blockOf(page, "Paragraph 40.").blockFormat().lineHeight(), 180.0);
        QCOMPARE(desk.settings->lineHeight(), 180);
        QCOMPARE(desk.value("format_line_height_value", "text").toString(), QString("180%"));
        QTest::qWait(100);
        QTRY_VERIFY2(qAbs(through(desk) - place) < 0.03, QByteArray::number(through(desk)));
        QVERIFY(qAbs(desk.reader->fraction() - place) < 0.03);
        // The keyboard steps it too, and the page's own keys wait meanwhile.
        QVERIFY(QMetaObject::invokeMethod(desk.find("format_paragraph_spacing"), "forceActiveFocus"));
        desk.key(Qt::Key_Right);
        desk.key(Qt::Key_Right);
        QTRY_COMPARE(desk.settings->paragraphSpacing(), 16);
        QTRY_COMPARE(blockOf(page, "Paragraph 40.").blockFormat().bottomMargin(), 16.0);
        desk.key(Qt::Key_J);
        QVERIFY(qAbs(desk.reader->fraction() - place) < 0.03);

        // Justified: filled out to both sides; centred: in the middle.
        clickItem(desk, "formatAlign_justify");
        QTRY_COMPARE(int(blockOf(page, "Paragraph 40.").blockFormat().alignment()), int(Qt::AlignJustify));
        QCOMPARE(desk.settings->align(), QString("justify"));
        desk.shot("reader-format-justify");
        clickItem(desk, "formatAlign_center");
        QTRY_VERIFY(blockOf(page, "Short last line.").blockFormat().alignment() & Qt::AlignHCenter);
        // Where the first (or last) line of a paragraph is: the blocks are
        // made anew each time the page is set, so they are looked up again.
        const auto lineOf = [&page](const QString &words, bool last = false) {
            const QTextLayout *layout = blockOf(page, words).layout();
            return layout->lineAt(last ? layout->lineCount() - 1 : 0).naturalTextRect();
        };
        const QString arabic = QString::fromUtf8("المصابيح");
        QVERIFY(lineOf("Short last line.").x() > page.item->width() / 4);
        desk.shot("reader-format-center");
        // At the end: against the right in English, against the left in Arabic.
        clickItem(desk, "formatAlign_end");
        QTRY_VERIFY(lineOf("Short last line.").x() > page.item->width() / 2);
        QVERIFY(lineOf(arabic, true).x() < page.item->width() / 4);
        clickItem(desk, "formatAlign_start");
        QTRY_VERIFY(lineOf("Short last line.").x() < 1);
        QVERIFY(lineOf(arabic, true).right() > page.item->width() * 3 / 4);
        // An indent, but not after the heading.
        clickItem(desk, "format_indent_more");
        QTRY_COMPARE(blockOf(page, "Paragraph 40.").blockFormat().textIndent(), 4.0);
        QCOMPARE(blockOf(page, "Paragraph 1.").blockFormat().textIndent(), 0.0);
        QTRY_VERIFY(qAbs(desk.reader->fraction() - place) < 0.03);

        // The graph's marks are where their names are, after all that.
        QTRY_VERIFY(page.formatOf("Aldo Brenn").hasProperty(QTextFormat::TextUnderlineColor));
        // One of its names on the screen, near the middle.
        QPoint aldo;
        for (int occurrence = 0; occurrence < 70 && aldo.isNull(); ++occurrence) {
            const QPoint at = page.at("Aldo Brenn", occurrence);
            if (at.y() > desk.window->height() / 3 && at.y() < desk.window->height() * 2 / 3)
                aldo = at;
        }
        QVERIFY(!aldo.isNull());
        QTest::mouseMove(desk.window, aldo);
        QTRY_VERIFY(desk.value("readingMenu", "visible").toBool());
        QCOMPARE(desk.value("readingMenuName", "text").toString(), QString("Aldo Brenn"));
        QTest::mouseMove(desk.window, QPoint(5, 5));
        QTRY_VERIFY(!desk.value("readingMenu", "visible").toBool());

        // For this book alone: the defaults stay, the book has its own.
        clickItem(desk, "formatThisBook");
        QTRY_VERIFY(desk.value("readerView", "formatBookOnly").toBool());
        const int size = desk.settings->fontSize();
        clickItem(desk, "format_font_size_more");
        clickItem(desk, "format_font_size_more");
        QTRY_COMPARE(desk.backend->library()->bookFormat(book).value("font_size").toInt(), size + 2);
        QCOMPARE(desk.settings->fontSize(), size);
        QTRY_COMPARE(desk.value("flowText", "font").value<QFont>().pixelSize(), size + 2);
        QVERIFY(desk.value("formatResetBook", "visible").toBool());
        desk.shot("reader-format");
        // The zoom keys go where the size is: the book's own.
        canvasFocusAway(desk);
        desk.key(Qt::Key_Minus);
        QTRY_COMPARE(desk.backend->library()->bookFormat(book).value("font_size").toInt(), size + 1);
        QCOMPARE(desk.settings->fontSize(), size);
        // Back to the defaults, by its own button.
        clickItem(desk, "formatResetBook");
        QTRY_VERIFY(desk.backend->library()->bookFormat(book).isEmpty());
        QVERIFY(!desk.value("readerView", "formatBookOnly").toBool());
        QTRY_COMPARE(desk.value("flowText", "font").value<QFont>().pixelSize(), size);

        // Reset puts back what there was when the panel opened.
        clickItem(desk, "formatRestore");
        QTRY_COMPARE(desk.settings->lineHeight(), 150);
        QCOMPARE(desk.settings->paragraphSpacing(), 12);
        QCOMPARE(desk.settings->align(), QString("start"));
        QCOMPARE(desk.settings->indent(), 0);
        QTRY_COMPARE(blockOf(page, "Paragraph 40.").blockFormat().lineHeight(), 150.0);
        QTRY_VERIFY(qAbs(desk.reader->fraction() - place) < 0.03);

        // Escape closes it, and the page has the keyboard again.
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("formatSide", "visible").toBool());
        QVERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.value("flowReader", "activeFocus").toBool());

        // A finger: the bar's Aa opens it.
        QObject *view = desk.find("readerView");
        view->setProperty("showBar", true);
        clickItem(desk, "barFormat");
        QTRY_VERIFY(desk.value("formatSide", "visible").toBool());
        QVERIFY(!desk.value("readerBar", "visible").toBool());
        // It and the AI take turns at the side.
        view->setProperty("showAssistant", true);
        QTRY_VERIFY(!desk.value("formatSide", "visible").toBool());
        QVERIFY(!view->property("showFormat").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    void theTextPanelIsUnderThePageInACompactWindowAndMirrored() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n"
                                 "selection_background = \"#dcdcdc\"\ncolor8 = \"#6f6f6f\"\n";
        for (const QByteArray &language : {QByteArray("en"), QByteArray("ar")}) {
            Desk desk({}, language, light);
            const QString file = desk.dir.filePath("long.epub");
            QVERIFY(longBook(file));
            const qint64 book = desk.add(file);
            QVERIFY(desk.show());
            desk.window->resize(1200, 820);
            desk.reader->openBook(book);
            QTRY_VERIFY(desk.reader->isOpen());
            desk.key(Qt::Key_A, Qt::ShiftModifier);
            QTRY_VERIFY(desk.value("formatSide", "visible").toBool());
            // Start is on the side the text starts at, and so is its picture.
            auto *start = qobject_cast<QQuickItem *>(desk.find("formatAlign_start"));
            auto *end = qobject_cast<QQuickItem *>(desk.find("formatAlign_end"));
            QVERIFY(start && end);
            const bool mirrored = language == "ar";
            QCOMPARE(start->mapToScene(QPointF()).x() > end->mapToScene(QPointF()).x(), mirrored);
            // The pane is at the side the text ends at.
            auto *side = qobject_cast<QQuickItem *>(desk.find("formatSide"));
            QCOMPARE(side->mapToScene(QPointF()).x() < desk.window->width() / 2, mirrored);
            desk.shot(language == "en" ? "reader-format-light" : "reader-format-rtl");

            // Compact: along the foot of the page, which still shows above it.
            desk.window->resize(560, 820);
            QTRY_VERIFY(desk.value("formatSheet", "visible").toBool());
            QVERIFY(!desk.value("formatSide", "visible").toBool());
            auto *sheet = qobject_cast<QQuickItem *>(desk.find("formatSheet"));
            QVERIFY(sheet->height() <= desk.window->height() * 0.61);
            QTRY_VERIFY(desk.value("formatPane", "focused").toBool());
            clickItem(desk, "format_font_size_more");
            QTRY_COMPARE(desk.settings->fontSize(), 20);
            desk.shot(language == "en" ? "reader-format-compact" : "reader-format-compact-rtl");
            desk.key(Qt::Key_Escape);
            QTRY_VERIFY(!desk.value("formatSheet", "visible").toBool());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }

    void thePicturesOfAPdfAreNotSetAndTheSettingsHoldTheDefaults() {
        Desk desk;
        const QString pdf = desk.dir.filePath("pages.pdf");
        QVERIFY(Fixtures::pdf(pdf, "Pictures", {"One page", "Another"}));
        const qint64 book = desk.add(pdf);
        QVERIFY(desk.show());
        desk.window->resize(1200, 820);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QCOMPARE(desk.reader->kind(), QString("pages"));
        desk.key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(desk.value("formatSide", "visible").toBool());
        QVERIFY(desk.value("formatPictures", "visible").toBool());
        QVERIFY(!desk.find("format_font_size")->property("enabled").toBool());
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("formatSide", "visible").toBool());
        desk.reader->close();
        QTRY_VERIFY(!desk.reader->isOpen());

        // The settings have the same controls, for the defaults.
        QMetaObject::invokeMethod(desk.window, "showSettings");
        QTRY_VERIFY(desk.value("settings", "opened").toBool());
        QVERIFY(desk.find("settingsFormat"));
        const int measure = desk.settings->measure();
        QTest::qWait(300);
        QVERIFY(QMetaObject::invokeMethod(desk.find("settingsFormat_measure_more"), "clicked"));
        QTRY_COMPARE(desk.settings->measure(), measure + 2);
        QVERIFY(QMetaObject::invokeMethod(desk.find("settingsFormatAlign_justify"), "clicked"));
        QTRY_COMPARE(desk.settings->align(), QString("justify"));
        desk.shot("settings-format");
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.value("settings", "opened").toBool());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A long made-up chapter with headings, naming the story graph's
    // people and places on every page, then a short one.
    static bool longMarkedBook(const QString &file) {
        Fixtures::Entries entries = Fixtures::epubEntries("Harbour of Glass");
        const QStringList names{"Ilse Varga", "Aldo Brenn", "Glass Harbour", "Tamsin Roe", "Pell", "The Tide Guild"};
        QString chapter = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1>The Night Ferry</h1>";
        for (int i = 0; i < 140; ++i) {
            if (i % 12 == 11)
                chapter += QString("<h2>Watch %1</h2>").arg(i / 12 + 1);
            chapter += QString("<p>%1 rowed out past the sea wall at the turn of the tide, and the lamps of the docks went "
                               "small behind; the oars knocked, the water talked under the hull, and %2 kept count of the "
                               "bells from the shore until there were none left to count. Paragraph %3.</p>")
                           .arg(names.at(i % names.size()), names.at((i + 3) % names.size()))
                           .arg(i + 1);
        }
        chapter += "</body></html>";
        for (auto &entry : entries) {
            if (entry.first == "OEBPS/text/ch1.xhtml")
                entry.second = chapter.toUtf8();
            if (entry.first == "OEBPS/text/ch2.xhtml")
                entry.second = "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1>Low Water</h1><p>The end.</p></body></html>";
        }
        return Fixtures::zip(file, entries);
    }
    // Until the pages stop being cut anew: the chapter is set again at a
    // new width, and its pictures and fonts arrive, a moment after a resize.
    static void settled(QObject *spread) {
        QVariant before;
        for (int calm = 0; calm < 3;) {
            QTest::qWait(100);
            const QVariant now = spread->property("breaks");
            calm = now == before ? calm + 1 : 0;
            before = now;
        }
    }
    static QQuickItem *sheet(Desk &desk, int index) { return qobject_cast<QQuickItem *>(desk.find(QString("page%1").arg(index))); }
    // The page that shows a position of the chapter, or -1.
    static int sheetShowing(QObject *spread, int position) {
        auto *first = spread->property("firstText").value<QQuickItem *>();
        QRectF rect;
        QMetaObject::invokeMethod(first, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        const QVariantList breaks = spread->property("breaks").toList();
        const int page = spread->property("page").toInt(), columns = spread->property("columns").toInt();
        for (int i = 0; i < columns && page + i + 1 < breaks.size(); ++i)
            if (rect.y() >= breaks.at(page + i).toDouble() - 0.5 && rect.y() < breaks.at(page + i + 1).toDouble() - 0.5)
                return i;
        return -1;
    }

private slots:
    // As many pages side by side as the room has: one in a narrow window,
    // two in a medium one, three on a wide screen; a choice of one, two or
    // three where it fits, and the most that do where it doesn't; no
    // flicker at the boundary; the place kept when the count changes.
    void thePagesSideBySideAreAsManyAsTheRoomHas() {
        Desk desk;
        const QString file = desk.dir.filePath("long.epub");
        QVERIFY(longMarkedBook(file));
        const qint64 book = desk.add(file);
        QVERIFY(desk.show());
        PageFormat *format = desk.reader->format();
        desk.window->resize(800, 900);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_COMPARE(format->available(), 800.0);

        // Narrow: one page that scrolls, no wider than the measure.
        QTRY_VERIFY(desk.find("flowReader"));
        QCOMPARE(format->columns(), 1);
        QVERIFY(format->lineWidth() > 66 * 0.35 * 19 && format->lineWidth() < 66 * 0.7 * 19);
        QVERIFY(desk.value("flowText", "width").toReal() <= format->lineWidth() + 0.5);
        desk.shot("pages-narrow");

        // Medium: two.
        desk.window->resize(1300, 900);
        QTRY_VERIFY(desk.find("spreadReader"));
        QCOMPARE(format->columns(), 2);
        QVERIFY(!sheet(desk, 2)->isVisible());
        desk.shot("pages-medium");

        // Wide: three, side by side in reading order, each as wide as the
        // measure allows and no narrower than the minimum.
        desk.window->resize(2300, 1000);
        QTRY_COMPARE(format->columns(), 3);
        QObject *spread = desk.find("spreadReader");
        QTRY_COMPARE(spread->property("columns").toInt(), 3);
        QTRY_VERIFY(spread->property("pages").toInt() > 6);
        QQuickItem *first = sheet(desk, 0), *second = sheet(desk, 1), *third = sheet(desk, 2);
        QVERIFY(third->isVisible());
        QVERIFY(second->x() > first->x() + first->width() && third->x() > second->x() + second->width());
        QVERIFY(first->width() <= format->lineWidth() + 0.5);
        QVERIFY(first->width() >= PageLayout::minimumMeasure * format->characterWidth() - 0.5);
        QCOMPARE(second->property("index").toInt(), 1);
        QCOMPARE(third->property("index").toInt(), 2);
        desk.shot("pages-wide");
        // A turn moves on by the whole spread; the place is the first page's.
        settled(spread);
        desk.key(Qt::Key_Space);
        QCOMPARE(spread->property("page").toInt(), 3);
        desk.key(Qt::Key_Space);
        QCOMPARE(spread->property("page").toInt(), 6);
        QCOMPARE(third->property("index").toInt(), 8);
        const int place = spread->property("placeAt").toInt();
        QVERIFY(place > 0);
        QCOMPARE(sheetShowing(spread, place), 0);

        // Two pages again: the words that began the spread are still in sight.
        desk.window->resize(1300, 900);
        QTRY_COMPARE(spread->property("columns").toInt(), 2);
        QTRY_VERIFY(sheetShowing(spread, place) >= 0);
        QCOMPARE(spread->property("page").toInt() % 2, 0);
        // And one: the scrolling page is near the same place.
        const double fraction = desk.reader->fraction();
        desk.window->resize(800, 900);
        QTRY_VERIFY(desk.find("flowReader"));
        QTRY_VERIFY(qAbs(desk.reader->fraction() - fraction) < 0.1);

        // A choice that fits is honoured where Automatic would show fewer ...
        desk.window->resize(1600, 900);
        QTRY_COMPARE(format->columns(), 2);
        desk.settings->setPages(1);
        QTRY_VERIFY(desk.find("flowReader"));
        QCOMPARE(format->columns(), 1);
        QVERIFY(format->pagesHonoured());
        // (The panel beside the page leaves less room, as any pane does.)
        desk.key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(desk.find("formatPagesName"));
        QTRY_COMPARE(format->available(), 1600.0 - qobject_cast<QQuickItem *>(desk.find("formatSide"))->width());
        // ... and one that doesn't shows the most that do, and says so.
        desk.settings->setPages(3);
        QTRY_VERIFY(desk.find("spreadReader"));
        QCOMPARE(format->columns(), 2);
        QVERIFY(!format->pagesHonoured());
        QTRY_VERIFY(desk.value("formatPagesName", "text").toString().contains("45"));
        QVERIFY(desk.value("format_measure_value", "text").toString().contains("66"));
        QVERIFY(desk.value("formatPages_3", "checked").toBool());
        desk.shot("pages-format");
        // Automatic, from the panel, says what it gives here.
        QVERIFY(QMetaObject::invokeMethod(desk.find("formatPages_0"), "clicked"));
        QTRY_COMPARE(desk.settings->pages(), 0);
        QTRY_COMPARE(desk.value("formatPagesName", "text").toString(), QString("Automatic: 2 pages here"));
        desk.key(Qt::Key_Escape);
        QTRY_VERIFY(!desk.find("formatPane"));
        // Shift+P goes round the choices.
        desk.key(Qt::Key_P, Qt::ShiftModifier);
        QTRY_COMPARE(desk.settings->pages(), 1);
        QTRY_VERIFY(desk.find("flowReader"));
        for (int i = 0; i < 3; ++i)
            desk.key(Qt::Key_P, Qt::ShiftModifier);
        QTRY_COMPARE(desk.settings->pages(), 0);

        // At the boundary between one page and two: two from where each
        // page has 80% of the measure, and kept until each would lose three
        // characters more, so a drag across it doesn't flicker.
        const double character = format->characterWidth();
        const int boundary = int(std::ceil(2 * std::round(66 * PageLayout::automaticShare) * character + 2 * format->margin() + format->gutter()));
        desk.window->resize(boundary + 2, 900);
        QTRY_COMPARE(format->columns(), 2);
        desk.window->resize(boundary - int(2 * character), 900);
        QTRY_COMPARE(format->available(), double(boundary - int(2 * character)));
        QCOMPARE(format->columns(), 2);
        desk.window->resize(boundary - int(10 * character), 900);
        QTRY_COMPARE(format->columns(), 1);
        desk.window->resize(boundary - int(2 * character), 900);
        QTRY_COMPARE(format->available(), double(boundary - int(2 * character)));
        QCOMPARE(format->columns(), 1);
        // A pane beside the page takes from the room, as a narrower window does.
        desk.window->resize(1300, 900);
        QTRY_COMPARE(format->columns(), 2);
        desk.find("readerView")->setProperty("showAssistant", true);
        QTRY_COMPARE(format->columns(), 1);
        QTRY_VERIFY(desk.find("flowReader"));
        desk.find("readerView")->setProperty("showAssistant", false);
        QTRY_COMPARE(format->columns(), 2);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // In every count of pages, the graph's marks, a selection and a found
    // word land on the words they are about, on the page that shows them.
    void marksSelectionsAndFindsLandOnEveryPage() {
        Desk desk;
        const QString file = desk.dir.filePath("long.epub");
        QVERIFY(longMarkedBook(file));
        const qint64 book = desk.add(file);
        storyGraph(desk, book);
        desk.settings->setMarks("always");
        QVERIFY(desk.show());
        desk.window->resize(2300, 1000);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        for (const int columns : {3, 2}) {
            desk.settings->setPages(columns);
            QTRY_VERIFY(desk.find("spreadReader"));
            QObject *spread = desk.find("spreadReader");
            QTRY_COMPARE(spread->property("columns").toInt(), columns);
            settled(spread);
            // Read to the end of the chapter, so everything named is met.
            desk.reader->goTo(0, {}, true);
            QTRY_COMPARE(spread->property("page").toInt(), spread->property("lastLeft").toInt());
            QTRY_VERIFY(spread->property("pages").toInt() > columns);
            // A spread with every page written on.
            desk.key(Qt::Key_Space, Qt::ShiftModifier);
            QTRY_VERIFY(sheet(desk, columns - 1)->property("there").toBool());
            auto *first = spread->property("firstText").value<QQuickItem *>();
            QTextDocument *document = first->property("textDocument").value<QQuickTextDocument *>()->textDocument();
            const QString plain = document->toPlainText();
            const QVariantList breaks = spread->property("breaks").toList();
            const int page = spread->property("page").toInt();
            for (int offset = 0; offset < columns; ++offset) {
                QQuickItem *shown = sheet(desk, offset);
                // The first "Ilse Varga" on this page.
                QRectF rect;
                int position = -1;
                for (int at = int(plain.indexOf("Ilse Varga")); at >= 0; at = int(plain.indexOf("Ilse Varga", at + 1))) {
                    QMetaObject::invokeMethod(first, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, at + 3));
                    if (rect.y() >= breaks.at(page + offset).toDouble() && rect.bottom() <= breaks.at(page + offset + 1).toDouble()) {
                        position = at;
                        break;
                    }
                }
                QVERIFY2(position >= 0, qPrintable(QString("no name on page %1 of %2").arg(offset).arg(columns)));
                // Where it is on the screen, through this page's own text.
                QQuickItem *own = nullptr;
                for (QQuickItem *child : shown->childItems())
                    for (QQuickItem *inner : child->childItems())
                        if (inner->inherits("QQuickTextEdit"))
                            own = inner;
                QVERIFY(own);
                QMetaObject::invokeMethod(own, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position + 3));
                const QPointF onSpread = own->mapToItem(qobject_cast<QQuickItem *>(spread), rect.center());
                // The mark there is the name's.
                QVariant found;
                QTRY_VERIFY(QMetaObject::invokeMethod(spread, "thingAt", Q_RETURN_ARG(QVariant, found), Q_ARG(QVariant, onSpread.x()),
                                                      Q_ARG(QVariant, onSpread.y()))
                            && found.toMap().value("name").toString() == QString("Ilse Varga"));
                const QRectF markRect = found.toMap().value("rect").toRectF();
                QVERIFY2(markRect.contains(onSpread), qPrintable(QString("page %1 of %2").arg(offset).arg(columns)));
                // A word selected there is that word, and its menu stands by it.
                QVariant selected;
                QVERIFY(QMetaObject::invokeMethod(spread, "selectAt", Q_RETURN_ARG(QVariant, selected), Q_ARG(QVariant, onSpread.x()),
                                                  Q_ARG(QVariant, onSpread.y())));
                QVERIFY(selected.toBool());
                QVariant chosen;
                QVERIFY(QMetaObject::invokeMethod(spread, "selection", Q_RETURN_ARG(QVariant, chosen)));
                QCOMPARE(chosen.toMap().value("text").toString(), QString("Ilse"));
                const QRectF chosenRect = chosen.toMap().value("rect").toRectF();
                QVERIFY2(qAbs(chosenRect.center().y() - onSpread.y()) < rect.height() && chosenRect.left() <= onSpread.x() + 1
                             && chosenRect.right() >= onSpread.x() - rect.width(),
                         qPrintable(QString("page %1 of %2").arg(offset).arg(columns)));
                QMetaObject::invokeMethod(spread, "clearSelection");
            }
            // A word found in the book is on the spread it goes to.
            QVariantList results;
            QVERIFY(QMetaObject::invokeMethod(desk.reader.operator->(), "find", Q_RETURN_ARG(QVariantList, results), Q_ARG(QString, "Paragraph 77.")));
            QCOMPARE(results.size(), 1);
            desk.reader->goToPlace(0, results.first().toMap().value("fraction").toDouble());
            QTRY_VERIFY(sheetShowing(spread, int(plain.indexOf("Paragraph 77."))) >= 0);
            desk.shot(QString("pages-marks-%1").arg(columns));
        }
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Right to left, and on a light theme, the pages run the other way:
    // the first on the right.
    void thePagesRunRightToLeftOnALightTheme() {
        const QByteArray light = "mode = \"light\"\naccent = \"#005fcc\"\nbackground = \"#fafafa\"\nforeground = \"#202020\"\n";
        Desk desk({}, "ar", light);
        const QString file = desk.dir.filePath("long.epub");
        QVERIFY(longMarkedBook(file));
        const qint64 book = desk.add(file);
        QVERIFY(desk.show());
        desk.window->resize(2300, 1000);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("spreadReader"));
        QTRY_COMPARE(desk.find("spreadReader")->property("columns").toInt(), 3);
        QQuickItem *first = sheet(desk, 0), *second = sheet(desk, 1), *third = sheet(desk, 2);
        QTRY_VERIFY(first->x() > second->x() + second->width() && second->x() > third->x() + third->width());
        QCOMPARE(desk.value("flowText", "color").value<QColor>(), QColor("#202020"));
        desk.shot("pages-rtl-light");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(UiTests)
#include "ui_tests.moc"
