#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "booklistmodel.h"
#include "customization.h"
#include "desk.h"
#include "fixtures.h"
#include "reader.h"
#include "scriptplugins.h"

// A plugin's own view (#77): a QML file of its folder in place of the shelf,
// or beside the page, with the way back, keys in the commands table and the
// help, a ⋯ for a pointer and a finger, and the ink surface (#78) as a part.
// The stand-in is a notebook, which is what the view is for first: pages to
// go back through, a pen to take, a page to write on.

namespace {

const QByteArray manifest = R"({"name": "Notebooks", "version": "1.0", "script": "main.js"})";

const QByteArray script = R"js(
omabook.ui.view({id: "notebooks", title: "Notebooks", source: "View.qml", key: "Ctrl+Shift+N", width: 420});
omabook.commands.register({id: "pen", title: "Take the pen", place: "view", key: "Ctrl+Shift+E",
                           run: function () { omabook.ui.set("tool", "pen"); }});
omabook.ui.launcher({id: "open", title: "Open the notebooks", open: function () { omabook.ui.showView("notebooks", {page: 2}); }});
function show(args) { omabook.ui.showView("notebooks", args); }
function close() { omabook.ui.closeView("notebooks"); }
function nothing() { omabook.ui.showView("drawings"); }
)js";

// The view: a page number it goes back through, a button to turn on, and a
// page of ink. What Omabook hands it shows in its words.
const QByteArray view = R"(import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Omabook

FocusScope {
    id: root
    objectName: "notebookView"
    property bool beside: false
    property var args: ({})
    property int page: 0
    readonly property var mine: backend.scripts.data["notebooks"] || ({})
    function back() {
        if (page === 0) return false
        page -= 1
        return true
    }
    function takeFocus() { ink.forceActiveFocus() }
    onArgsChanged: if (args.page !== undefined) page = args.page

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Label {
                objectName: "notebookPage"
                Layout.fillWidth: true
                text: "Page " + (root.page + 1) + (root.beside ? " beside" : "") + (root.mine.tool ? " · " + root.mine.tool : "")
                elide: Text.ElideRight
            }
            Button {
                objectName: "notebookNext"
                text: "Next page"
                focusPolicy: Qt.NoFocus
                onClicked: root.page += 1
            }
        }
        InkSurface {
            id: ink
            objectName: "notebookInk"
            Layout.fillWidth: true
            Layout.fillHeight: true
            focus: true
        }
    }
}
)";

QPoint middle(QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); }

// The window with a library of a few books and the notebook plugin in it.
bool withNotebooks(Desk &desk) {
    const QString folder = desk.dir.filePath("plugins/notebooks");
    if (!QDir().mkpath(folder) || !Fixtures::write(folder + "/plugin.json", manifest) || !Fixtures::write(folder + "/main.js", script)
        || !Fixtures::write(folder + "/View.qml", view))
        return false;
    desk.backend->scripts()->reload();
    return QTest::qWaitFor([&] { return desk.custom->keyText().contains("library.notebooks:notebooks"); }, 3000);
}

QString pageText(Desk &desk) { return desk.find("notebookPage") ? desk.find("notebookPage")->property("text").toString() : QString(); }

} // namespace

class PluginViewTests : public QObject {
    Q_OBJECT

    QStringList found;
    int looked = 0;

    void look(Desk &desk, const QString &state) {
        desk.settle();
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        ++looked;
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("view-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // ui.view gives the plugin a view and two commands of its id, for the
    // shelf and the page, with its key; ui.showView and ui.closeView ask
    // the window, a key's command to put the view away again.
    void aViewComesWithItsCommands() {
        QTemporaryDir dir;
        const QString folder = dir.filePath("plugins/notebooks");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(Fixtures::write(folder + "/plugin.json", manifest));
        QVERIFY(Fixtures::write(folder + "/main.js", script));
        QVERIFY(Fixtures::write(folder + "/View.qml", view));
        ScriptPlugins plugins({}, dir.filePath("plugins"), nullptr, false);
        ScriptPlugin *notebooks = plugins.plugin("notebooks");
        QVERIFY(notebooks);

        const QVariantList panels = plugins.panels();
        QCOMPARE(panels.size(), 1);
        const QVariantMap entry = panels.first().toMap();
        QCOMPARE(entry.value("place").toString(), QString("view"));
        QCOMPARE(entry.value("plugin").toString(), QString("notebooks"));
        QCOMPARE(entry.value("view").toString(), QString("notebooks"));
        QCOMPARE(entry.value("title").toString(), QString("Notebooks"));
        QCOMPARE(entry.value("width").toInt(), 420);
        QHash<QString, QString> keys;
        for (const QVariant &command : plugins.commands())
            keys.insert(command.toMap().value("id").toString(), command.toMap().value("key").toString());
        QCOMPARE(keys.value("library.notebooks:notebooks"), QString("Ctrl+Shift+N"));
        QCOMPARE(keys.value("reader.notebooks:notebooks"), QString("Ctrl+Shift+N"));
        QCOMPARE(keys.value("view.notebooks:pen"), QString("Ctrl+Shift+E"));
        // The plugin manager's list says it puts a view in the window.
        QVERIFY(plugins.list().first().panels.contains("view: View.qml"));

        QSignalSpy requested(&plugins, &ScriptPlugins::viewRequested), closed(&plugins, &ScriptPlugins::viewClosed);
        QString problem;
        notebooks->callNow("show", {QJsonObject{{"page", 3}}}, &problem);
        QVERIFY2(problem.isEmpty(), qPrintable(problem));
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.at(0).at(0).toString(), QString("notebooks"));
        QCOMPARE(requested.at(0).at(1).toString(), QString("notebooks"));
        QCOMPARE(requested.at(0).at(2).toMap(), (QVariantMap{{"page", 3}}));
        QVERIFY(!requested.at(0).at(3).toBool());
        // Its command, from a key or a menu, opens it or puts it away.
        plugins.run("reader.notebooks:notebooks");
        QTRY_COMPARE(requested.size(), 2);
        QVERIFY(requested.at(1).at(3).toBool());
        QCOMPARE(requested.at(1).at(2).toMap(), QVariantMap());
        notebooks->callNow("close", {}, &problem);
        QCOMPARE(closed.size(), 1);
        QCOMPARE(closed.at(0).at(1).toString(), QString("notebooks"));
        // A view it never gave is a mistake said at once.
        notebooks->callNow("nothing", {}, &problem);
        QVERIFY2(problem.contains("There is no view \"drawings\""), qPrintable(problem));

        // A view needs an id of the plugin's kind, and a file of its folder.
        const auto refused = [&](const QString &name, const QByteArray &line) {
            const QString other = dir.filePath("plugins/" + name);
            QDir().mkpath(other);
            Fixtures::write(other + "/plugin.json", manifest);
            Fixtures::write(other + "/main.js", line);
            Fixtures::write(other + "/View.qml", view);
            plugins.reload();
            for (const ScriptPlugins::Info &info : plugins.list())
                if (info.manifest.id == name)
                    return info.problem;
            return QString();
        };
        QVERIFY(refused("named", "omabook.ui.view({id: \"Not Ok\", source: \"View.qml\"});").contains("A view's id is lower-case"));
        QVERIFY(refused("lost", "omabook.ui.view({id: \"lost\", source: \"Gone.qml\"});")
                    .contains("There is no \"Gone.qml\" in the plugin's folder for the view."));
        QVERIFY(refused("bare", "omabook.ui.view({id: \"bare\"});").contains("The view's source should be a string."));
    }

    // In place of the shelf: its key opens it and closes it, Backspace goes
    // back through the plugin's own pages and then out, Escape closes it,
    // and its ⋯ lists what can be done in it, its plugin's commands too.
    void theViewTakesTheWindow() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY(withNotebooks(desk));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1180, 780)));
        desk.quicken();

        QTest::keyClick(desk.window, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(desk.shows("notebookView"));
        QVERIFY(desk.shows("pluginViewFull"));
        QVERIFY(!desk.shows("library"));
        QCOMPARE(desk.find("pluginViewTitle")->property("text").toString(), QString("Notebooks"));
        QCOMPARE(pageText(desk), QString("Page 1"));
        // The ink surface is a part of the view, and has the keyboard.
        QTRY_VERIFY(desk.item("notebookInk") && desk.item("notebookInk")->hasActiveFocus());
        QVERIFY(desk.item("notebookInk")->width() > 600);
        // Its plugin's command for it works while it is up, and not on the shelf.
        QTest::keyClick(desk.window, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(pageText(desk), QString("Page 1 · pen"));
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(desk.shows("library"));
        QVERIFY(!desk.shows("notebookView"));
        QVERIFY(!desk.run("view.notebooks:pen"));

        // Asked for with a page: back through the pages, then out.
        QString problem;
        desk.backend->scripts()->plugin("notebooks")->callNow("show", {QJsonObject{{"page", 2}}}, &problem);
        QTRY_COMPARE(pageText(desk), QString("Page 3 · pen"));
        QTest::keyClick(desk.window, Qt::Key_Backspace);
        QTRY_COMPARE(pageText(desk), QString("Page 2 · pen"));
        QTest::keyClick(desk.window, Qt::Key_Backspace);
        QTest::keyClick(desk.window, Qt::Key_Backspace);
        QTRY_VERIFY(desk.shows("library"));
        // Escape closes it at once; so does its way back, clicked.
        QVERIFY(desk.run("library.notebooks:notebooks"));
        QTRY_VERIFY(desk.shows("notebookView"));
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(desk.item("notebookNext")));
        QTRY_COMPARE(pageText(desk), QString("Page 2 · pen"));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(desk.shows("library"));
        QVERIFY(desk.run("library.notebooks:notebooks"));
        QTRY_VERIFY(desk.shows("pluginViewClose"));
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(desk.item("pluginViewClose")));
        QTRY_VERIFY(desk.shows("library"));

        // Its own item on the shelf opens it.
        BookListModel *books = desk.backend->books();
        QTRY_VERIFY(books->idAt(0) < 0);
        QVERIFY(desk.backend->scripts()->launch(books->idAt(0)));
        QTRY_VERIFY(desk.shows("notebookView"));
        QTRY_COMPARE(pageText(desk), QString("Page 3 · pen"));

        // Its ⋯: the view's keys, and its plugin's command.
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(desk.item("pluginViewMore")));
        QVERIFY(desk.waitForPopup());
        QObject *menu = desk.find("commandMenu");
        QCOMPARE(menu->property("kind").toString(), QString("view"));
        QStringList listed;
        for (const QVariant &entry : menu->property("entries").toList())
            listed << entry.toMap().value("id").toString();
        for (const QString id : {"view.back", "view.close", "view.notebooks:pen"})
            QVERIFY2(listed.contains(id), qPrintable(listed.join(", ")));
        QVERIFY(desk.closePopups());

        // A book opened from it is read over it, and closing it comes back.
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(!desk.shows("pluginViewFull"));
        desk.reader->close();
        QTRY_VERIFY(desk.shows("notebookView"));
        // Switched off, its view goes with it.
        desk.backend->setPluginEnabled("folder:notebooks", false);
        QTRY_VERIFY(desk.shows("library"));
        QVERIFY(!desk.find("notebookView"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Beside the page: the book keeps its place on one side and the view
    // has the other, its keys its own while it has the keyboard; Escape
    // puts it away, the map takes its place, its key opens and closes it.
    void theViewStandsBesideThePage() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY(withNotebooks(desk));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1440, 800)));
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.shows("touchArea"));
        const qreal whole = desk.item("touchArea")->width();

        QTest::keyClick(desk.window, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(desk.shows("notebookView"));
        QVERIFY(desk.shows("pluginSide"));
        QVERIFY(!desk.shows("pluginViewClose"));
        QVERIFY(desk.shows("pluginViewHide"));
        QCOMPARE(pageText(desk), QString("Page 1 beside"));
        QCOMPARE(desk.item("pluginSide")->width(), 420.0);
        QTRY_VERIFY(desk.item("touchArea")->width() <= whole - 420 + 1);
        QObject *reading = desk.find("readerView");
        QTRY_VERIFY(reading->property("mapping").toBool());
        QVERIFY(!reading->property("paging").toBool());
        QTest::keyClick(desk.window, Qt::Key_E, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_COMPARE(pageText(desk), QString("Page 1 beside · pen"));
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.shows("pluginSide"));
        QVERIFY(desk.reader->isOpen());
        QTRY_VERIFY(reading->property("paging").toBool());

        // Its key opens it and puts it away; the map takes its place.
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(desk.shows("pluginSide"));
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY(!desk.shows("pluginSide"));
        QVERIFY(desk.run("reader.notebooks:notebooks"));
        QTRY_VERIFY(desk.shows("pluginSide"));
        QVERIFY(desk.run("reader.graph"));
        QTRY_VERIFY(desk.shows("graphSide"));
        QVERIFY(!desk.shows("pluginSide"));
        QVERIFY(desk.run("reader.notebooks:notebooks"));
        QTRY_VERIFY(desk.shows("pluginSide"));
        QVERIFY(!desk.shows("graphSide"));
        // Its × puts it away.
        desk.settle();
        QQuickItem *hide = desk.item("pluginViewHide");
        QVERIFY2(desk.window->contentItem()->contains(desk.window->contentItem()->mapFromScene(QPointF(middle(hide)))),
                 qPrintable(QString("the × is at %1 of a window %2 wide; the pane at %3, %4 wide")
                                .arg(middle(hide).x())
                                .arg(desk.window->width())
                                .arg(desk.item("pluginSide")->mapToScene(QPointF()).x())
                                .arg(desk.item("pluginSide")->width())));
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, middle(desk.item("pluginViewHide")));
        QTRY_VERIFY(!desk.shows("pluginSide"));
        // The book closed, the plugin asking for its view shows it in place of the shelf.
        desk.reader->close();
        QTRY_VERIFY(!desk.reader->isOpen());
        QString problem;
        desk.backend->scripts()->plugin("notebooks")->callNow("show", {QJsonObject{}}, &problem);
        QTRY_VERIFY(desk.shows("pluginViewFull"));
        QCOMPARE(pageText(desk), QString("Page 1 · pen"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Under fingers: the view's buttons and its way back are a fingertip,
    // a tap turns the plugin's page, and the ways out close it, whole and
    // beside the page.
    void theViewUnderFingers() {
        Desk desk("en", {}, true);
        const QList<qint64> ids = stock(desk);
        QVERIFY(withNotebooks(desk));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(800, 1200)));
        QPointingDevice *finger = QTest::createTouchDevice();
        const auto tap = [&](const QString &name) {
            QQuickItem *item = desk.item(name);
            QVERIFY2(item && desk.shows(name), qPrintable(name + " is not on screen"));
            QVERIFY2(std::min(item->width(), item->height()) >= Survey::fingertip - 0.5, qPrintable(name + " is smaller than a fingertip"));
            QTest::touchEvent(desk.window, finger).press(0, middle(item), desk.window);
            QTest::touchEvent(desk.window, finger).release(0, middle(item), desk.window);
        };
        QVERIFY(desk.run("library.notebooks:notebooks"));
        QTRY_VERIFY(desk.shows("notebookView"));
        tap("notebookNext");
        QTRY_COMPARE(pageText(desk), QString("Page 2"));
        tap("pluginViewClose");
        QTRY_VERIFY(desk.shows("library"));

        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen());
        QVERIFY(desk.run("reader.notebooks:notebooks"));
        QTRY_VERIFY(desk.shows("pluginSide"));
        tap("pluginSideMore");
        QVERIFY(desk.waitForPopup());
        QVERIFY(desk.closePopups());
        tap("pluginViewHide");
        QTRY_VERIFY(!desk.shows("pluginSide"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // At every size, in German (whose words are a third longer) with a
    // mouse and under fingers, and right to left: the view whole, and
    // beside the page, where in a narrow window it lies over the page.
    void theViewAtEverySize() {
        const QList<std::pair<QByteArray, bool>> looks{{"de", false}, {"de", true}, {"ar", false}};
        for (const auto &[language, touch] : looks) {
            Desk desk(language, language == "ar" ? lightTheme : QByteArray(), touch);
            const QList<qint64> ids = stock(desk);
            QVERIFY(withNotebooks(desk));
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            QVERIFY(desk.run("library.notebooks:notebooks"));
            QTRY_VERIFY(desk.shows("notebookView"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "whole");
            }
            QVERIFY(desk.run("view.close"));
            desk.reader->openBook(ids.first());
            QTRY_VERIFY(desk.reader->isOpen());
            QVERIFY(desk.run("reader.notebooks:notebooks"));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                QTRY_VERIFY(desk.shows("pluginSide"));
                look(desk, "beside");
            }
            desk.reader->close();
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = found;
        found.clear();
        QVERIFY2(problems.isEmpty(),
                 qPrintable(
                     QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(PluginViewTests)
#include "pluginview_tests.moc"
