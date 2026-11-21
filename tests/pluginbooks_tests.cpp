// A plugin's own kind of book opens in the plugin's view, not the reader
// (#125): a notebook on the shelf opens as a notebook, by key, click and
// touch, from search or the command line; with its plugin off, it says
// which plugin opens it. The stand-in is a jotter, whose jottings are books
// of the format "jot" and whose pad is its view.

#include <QDir>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "booklistmodel.h"
#include "customization.h"
#include "desk.h"
#include "fixtures.h"
#include "formats.h"
#include "reader.h"
#include "scriptplugins.h"

namespace {

// The format is said in plugin.json, so it is known while the plugin is off.
const QByteArray manifest = R"({"name": "Jotter", "version": "1.0", "script": "main.js",
    "formats": [{"extension": "jot", "name": "Jotting", "view": "pad"}]})";

const QByteArray script = R"js(
omabook.ui.view({id: "pad", title: "Jotter", source: "Pad.qml"});
// A format with a view needs no open; one without either is a mistake.
omabook.format.register({name: "Sketch", extensions: ["sketch"], view: "pad"});
function withoutEither() { omabook.format.register({name: "Bare", extensions: ["bare"]}); }
)js";

// The view, saying the book it was given.
const QByteArray pad = R"(import QtQuick
import QtQuick.Controls

FocusScope {
    objectName: "padView"
    property var args: ({})
    Label {
        objectName: "padBook"
        anchors.centerIn: parent
        text: args.book ? args.book.title + " #" + args.book.id : "no book"
    }
}
)";

bool writeJotter(const QString &plugins) {
    const QString folder = plugins + "/jotter";
    return QDir().mkpath(folder) && Fixtures::write(folder + "/plugin.json", manifest) && Fixtures::write(folder + "/main.js", script)
           && Fixtures::write(folder + "/Pad.qml", pad);
}

// A jotting in the desk's library, with the jotter running.
qint64 withAJotting(Desk &desk, const QString &title = "Harbour Notes") {
    if (!writeJotter(desk.dir.filePath("plugins")))
        return 0;
    desk.backend->scripts()->reload();
    if (!QTest::qWaitFor([&] { return desk.custom->keyText().contains("library.jotter:pad"); }, 3000))
        return 0;
    const QString file = desk.dir.filePath(title + ".jot");
    if (!Fixtures::write(file, "a page of jottings"))
        return 0;
    const Library::Added added = desk.backend->library()->add(file);
    desk.backend->reload();
    return added.error.isEmpty() ? added.id : 0;
}

QString padBook(Desk &desk) { return desk.find("padBook") ? desk.find("padBook")->property("text").toString() : QString(); }

// Where the one book on the shelf is.
QPoint theBook(const Desk &desk) {
    QList<QQuickItem *> items{desk.window->contentItem()};
    while (!items.isEmpty()) {
        QQuickItem *item = items.takeFirst();
        if (!item->isVisible())
            continue;
        if (QString::fromLatin1(item->metaObject()->className()).startsWith("BookTaps") && item->width() >= 8)
            return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        items << item->childItems();
    }
    return {};
}

// The view in place of the shelf with the book, and no reader.
bool showsTheJotting(Desk &desk, qint64 id) {
    return QTest::qWaitFor([&] { return desk.shows("padView") && padBook(desk) == "Harbour Notes #" + QString::number(id); }, 3000)
           && desk.shows("pluginViewFull") && !desk.reader->isOpen() && !desk.reader->loading() && desk.reader->error().isEmpty();
}

bool backToTheShelf(Desk &desk) {
    QTest::keyClick(desk.window, Qt::Key_Escape);
    return QTest::qWaitFor([&] { return desk.shows("library") && !desk.shows("padView"); }, 3000);
}

} // namespace

class PluginBooksTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // plugin.json's formats are the plugin's from the start, and a format
    // registered with a view needs no open; the view is asked for with the
    // book, and a book with a file Omabook reads itself is read.
    void aFormatWithAViewOpensThere() {
        QTemporaryDir dir;
        QVERIFY(writeJotter(dir.filePath("plugins")));
        const ScriptManifest read = ScriptManifest::read(dir.filePath("plugins/jotter"));
        QVERIFY2(read.problem.isEmpty(), qPrintable(read.problem));
        QCOMPARE(read.formats.size(), 1);
        QCOMPARE(read.formats.first().extension, QString("jot"));
        QCOMPARE(read.formats.first().view, QString("pad"));
        QCOMPARE(read.formats.first().title, QString("Jotter"));

        ScriptPlugins plugins({}, dir.filePath("plugins"), nullptr, false);
        ScriptPlugin *jotter = plugins.plugin("jotter");
        QVERIFY(jotter);
        QStringList views;
        for (const ScriptPlugin::Format &format : jotter->formats())
            views << format.name + ":" + format.view;
        QCOMPARE(views, (QStringList{"Jotting:pad", "Sketch:pad"}));
        QString problem;
        jotter->callNow("withoutEither", {}, &problem);
        QVERIFY2(problem.contains("The format's open should be a function."), qPrintable(problem));

        Book book;
        book.id = 7;
        book.meta.title = "Harbour Notes";
        book.formats = {"jot"};
        QSignalSpy requested(&plugins, &ScriptPlugins::viewRequested);
        QVERIFY(plugins.openInView(book));
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), QString("jotter"));
        QCOMPARE(requested.at(0).at(1).toString(), QString("pad"));
        const QVariantMap given = requested.at(0).at(2).toMap().value("book").toMap();
        QCOMPARE(given.value("id").toLongLong(), 7);
        QCOMPARE(given.value("title").toString(), QString("Harbour Notes"));
        QVERIFY(!requested.at(0).at(3).toBool());
        // A jotting that is an EPUB as well is read; a book in no format of the plugin's is too.
        book.formats = {"jot", "epub"};
        QVERIFY(!plugins.openInView(book));
        book.formats = {"story"};
        QVERIFY(!plugins.openInView(book));
        QCOMPARE(requested.size(), 1);

        // A plugin.json format that isn't one says so.
        const QString wrong = dir.filePath("plugins/wrong");
        QVERIFY(QDir().mkpath(wrong));
        QVERIFY(Fixtures::write(wrong + "/main.js", ""));
        QVERIFY(Fixtures::write(wrong + "/plugin.json", R"({"script": "main.js", "formats": [{"extension": "jot"}]})"));
        QVERIFY(ScriptManifest::read(wrong).problem.contains("Each of plugin.json's \"formats\""));
        QVERIFY(Fixtures::write(wrong + "/plugin.json",
                                R"({"script": "main.js", "formats": [{"extension": "jot", "name": "J", "view": "Not Ok"}]})"));
        QVERIFY(ScriptManifest::read(wrong).problem.contains("a view's id is lower-case"));
    }

    // In the window: opening the jotting by key, by a double click and as
    // the command line, search and recent books do (Reader::openBook) shows
    // the pad with it; the reader never opens. Asked to read it as text, it
    // says why it can't.
    void theJottingOpensInThePad() {
        Desk desk("en", {}, false);
        const qint64 id = withAJotting(desk);
        QVERIFY(id > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.quicken();
        QCOMPARE(desk.backend->books()->idAt(0), id);
        QCOMPARE(bookToMap(*desk.backend->library()->book(id), desk.backend->library()->root()).value("needsPlugin").toString(), QString());

        // By key: selected on the shelf, Return.
        desk.call("selectOnShelf", id);
        desk.settle();
        QTest::keyClick(desk.window, Qt::Key_Return);
        QVERIFY(showsTheJotting(desk, id));
        QVERIFY(backToTheShelf(desk));
        // By a double click.
        const QPoint at = theBook(desk);
        QVERIFY(!at.isNull());
        QTest::mouseDClick(desk.window, Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(showsTheJotting(desk, id));
        QVERIFY(backToTheShelf(desk));
        // As the command line and every other way in opens a book, and while another is read.
        desk.reader->openBook(id);
        QVERIFY(showsTheJotting(desk, id));
        QVERIFY(backToTheShelf(desk));

        QString error;
        QVERIFY(!Formats::open(desk.backend->library()->formatPath(id, "jot"), &error));
        QVERIFY2(error.contains("opens in a view of its own"), qPrintable(error));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // A finger's tap on the jotting opens the pad too.
    void aTapOpensThePad() {
        Desk desk("en", {}, true);
        const qint64 id = withAJotting(desk);
        QVERIFY(id > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(800, 1200)));
        desk.quicken();
        const QPoint at = theBook(desk);
        QVERIFY(!at.isNull());
        QPointingDevice *finger = QTest::createTouchDevice();
        QTest::touchEvent(desk.window, finger).press(0, at, desk.window);
        QTest::touchEvent(desk.window, finger).release(0, at, desk.window);
        QVERIFY(showsTheJotting(desk, id));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // With the jotter off, the jotting stays, and it and the reader say
    // which plugin opens it and to switch it on, not to install it; on
    // again, it opens in the pad. In German too, at every size.
    void offItSaysWhichPluginOpensIt() {
        for (const QByteArray language : {"en", "de"}) {
            Desk desk(language, {}, false);
            const qint64 id = withAJotting(desk);
            QVERIFY(id > 0);
            desk.backend->setPluginEnabled("folder:jotter", false);
            QTRY_VERIFY(!desk.backend->scripts()->plugin("jotter"));
            QTRY_COMPARE(Formats::pluginFormat("jot").value_or(PluginFormat{}).installed, true);
            QVERIFY(Formats::isBook("x.jot"));
            const QVariantMap shown = bookToMap(*desk.backend->library()->book(id), desk.backend->library()->root());
            QCOMPARE(shown.value("needsPlugin").toString(), QString("Jotter"));
            QCOMPARE(shown.value("pluginAddress").toString(), QString());
            desk.backend->reload();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            desk.call("selectOnShelf", id);
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                desk.settle();
                if (desk.shows("detailsNeedsPlugin")) {
                    Survey survey{desk.window, desk.touch, {}};
                    survey.walk();
                    QVERIFY2(survey.problems.isEmpty(), qPrintable(QString("%1 %2x%3: %4")
                                                                       .arg(QString::fromLatin1(language))
                                                                       .arg(size.width())
                                                                       .arg(size.height())
                                                                       .arg(survey.problems.join('\n'))));
                    desk.shot(QString("jotting-off-%1-%2x%3").arg(QString::fromLatin1(language)).arg(size.width()).arg(size.height()));
                }
            }
            QVERIFY(desk.resize(QSize(1180, 780)));
            QTRY_VERIFY(desk.shows("detailsNeedsPlugin"));
            const QString said = desk.find("detailsNeedsPlugin")->property("text").toString();
            QVERIFY2(said.contains("Jotter"), qPrintable(said));
            if (language == "en")
                QCOMPARE(said, QString("Opened by the Jotter plugin, which isn't running. Switch it on under Plugins."));
            QVERIFY(!desk.shows("installNeededPlugin"));

            desk.reader->openBook(id);
            QTRY_VERIFY(!desk.reader->error().isEmpty());
            const QString reason = desk.reader->error();
            QVERIFY2(reason.contains("Jotter"), qPrintable(reason));
            if (language == "en")
                QVERIFY2(reason.contains("Switch it on under Plugins"), qPrintable(reason));
            desk.settle();
            desk.shot(QString("jotting-off-reader-%1").arg(QString::fromLatin1(language)));
            desk.reader->close();

            desk.backend->setPluginEnabled("folder:jotter", true);
            QTRY_VERIFY(desk.custom->keyText().contains("library.jotter:pad"));
            QVERIFY(!Formats::neededPlugin({"jot"}));
            desk.reader->openBook(id);
            QVERIFY(showsTheJotting(desk, id));
            desk.shot(QString("jotting-pad-%1").arg(QString::fromLatin1(language)));
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
    }
};

QTEST_MAIN(PluginBooksTests)
#include "pluginbooks_tests.moc"
