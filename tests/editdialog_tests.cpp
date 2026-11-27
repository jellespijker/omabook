// What is changed elsewhere while a dialog of the window is open (the command
// line, an agent over MCP, a sync: another connection to the library) is not
// written back over by saving it (#223): only what was changed in the dialog
// is saved. A field changed on both sides is said first, and a book removed
// meanwhile is said rather than the edits dropped without a word. The same
// holds for a collection, a thing of the graph and a margin note.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "graph.h"
#include "graphmodel.h"

namespace {

// The window with one book on paper, and the library as another program has it.
struct Bench {
    Desk desk{"en", {}, false};
    qint64 id = 0;
    std::optional<Library> other;

    [[nodiscard]] bool open() {
        Library *own = desk.backend->library();
        BookMeta meta;
        meta.title = "The Salt Road";
        meta.authors = {"Edda Marlowe"};
        id = own->addEntry(meta, true).id;
        desk.backend->reload();
        other.emplace(own->root());
        if (id <= 0 || !other->open() || !desk.show())
            return false;
        desk.quicken();
        return true;
    }
    [[nodiscard]] bool edit() {
        QMetaObject::invokeMethod(desk.window, "editBook", Q_ARG(QVariant, id));
        return QTest::qWaitFor([&] { return property("edit", "opened").toBool(); }, 3000);
    }
    [[nodiscard]] QVariant property(const QString &name, const char *key) const {
        const QObject *found = desk.find(name);
        return found ? found->property(key) : QVariant();
    }
    void set(const QString &name, const char *key, const QVariant &value) const {
        if (QObject *found = desk.find(name))
            found->setProperty(key, value);
        else
            QFAIL(qPrintable("nothing is called " + name));
    }
    void call(const QString &name, const char *method) const { QMetaObject::invokeMethod(desk.find(name), method); }
    [[nodiscard]] Book book() const { return *other->book(id); }
    // Whether the item is on screen to be read: shown, inside the window, and
    // cut off by nothing it stands in.
    [[nodiscard]] bool inSight(const QString &name) const {
        const QQuickItem *item = desk.item(name);
        if (!item || !item->isVisible() || item->width() <= 0 || item->height() <= 0)
            return false;
        const QRectF at = item->mapRectToScene(item->boundingRect());
        for (const QQuickItem *up = item->parentItem(); up; up = up->parentItem())
            if (up->clip() && !up->mapRectToScene(up->boundingRect()).adjusted(-1, -1, 1, 1).contains(at))
                return false;
        return QRectF(QPointF(), QSizeF(desk.window->size())).contains(at);
    }
    // As `omabook set` and an agent do it: some fields, on a connection of its own.
    [[nodiscard]] bool changeElsewhere(const QString &title = {}) {
        BookMeta meta = book().meta;
        if (!title.isEmpty())
            meta.title = title;
        meta.publisher = "Changed Elsewhere";
        meta.description = "written by another program";
        meta.seriesIndex = 4;
        meta.tags = {"fromcli"};
        return other->setMeta(id, meta) && other->setReading(id, "reading", 2, {}) && other->setPaper(id, false);
    }
};

} // namespace

class EditDialogTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void onlyWhatWasChangedInTheDialogIsSaved() {
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        QVERIFY(bench.changeElsewhere());
        bench.set("editTitle", "text", "The Salt Road X");
        bench.call("edit", "save");
        QTRY_VERIFY(!bench.property("edit", "visible").toBool());
        const Book saved = bench.book();
        QCOMPARE(saved.meta.title, QString("The Salt Road X"));
        QCOMPARE(saved.meta.publisher, QString("Changed Elsewhere"));
        QCOMPARE(saved.meta.description, QString("written by another program"));
        QCOMPARE(saved.meta.seriesIndex, 4.0);
        QCOMPARE(saved.shelf, QString("reading"));
        QCOMPARE(saved.rating, 2);
        QVERIFY(!saved.paper);
        QCOMPARE(saved.meta.tags, QStringList{"fromcli"});
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    void oneThingOfTheReadingChangedLeavesTheRest() {
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        QVERIFY(bench.other->setReading(bench.id, "read", 0, "2026-03-01"));
        bench.set("editStars", "value", 5);
        bench.call("edit", "save");
        QTRY_VERIFY(!bench.property("edit", "visible").toBool());
        QCOMPARE(bench.book().shelf, QString("read"));
        QCOMPARE(bench.book().finished, QString("2026-03-01"));
        QCOMPARE(bench.book().rating, 5);
    }

    void theSameChangeOnBothSidesIsSavedAtOnce() {
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        QVERIFY(bench.changeElsewhere());
        bench.set("editPublisher", "text", "Changed Elsewhere");
        bench.call("edit", "save");
        QTRY_VERIFY(!bench.property("edit", "visible").toBool());
    }

    void aFieldChangedOnBothSidesIsSaidFirst_data() {
        QTest::addColumn<bool>("theirs");
        QTest::newRow("keep what was entered") << false;
        QTest::newRow("take the other change") << true;
    }

    void aFieldChangedOnBothSidesIsSaidFirst() {
        QFETCH(const bool, theirs);
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        QVERIFY(bench.changeElsewhere("The Pepper Road"));
        bench.set("editTitle", "text", "The Salt Road X");
        bench.set("editPublisher", "text", "My Press");
        bench.set("editDescription", "text", "written here");
        bench.call("edit", "save");
        // Nothing is written yet: the dialog names what was changed on both sides.
        QVERIFY(bench.property("edit", "opened").toBool());
        QVERIFY(bench.property("editChanged", "visible").toBool());
        const QString said = bench.property("editChanged", "text").toString();
        QVERIFY2(said.contains("Title, Publisher, Description"), qPrintable(said));
        QCOMPARE(bench.book().meta.title, QString("The Pepper Road"));
        QCOMPARE(bench.book().meta.publisher, QString("Changed Elsewhere"));
        bench.desk.shot("edit-changed-elsewhere");
        if (theirs) {
            bench.call("editTakeTheirs", "clicked");
            QCOMPARE(bench.property("editTitle", "text").toString(), QString("The Pepper Road"));
            QCOMPARE(bench.property("editPublisher", "text").toString(), QString("Changed Elsewhere"));
            QVERIFY(!bench.property("editChanged", "visible").toBool());
        }
        bench.call("edit", "save");
        QTRY_VERIFY(!bench.property("edit", "visible").toBool());
        const Book saved = bench.book();
        QCOMPARE(saved.meta.title, QString(theirs ? "The Pepper Road" : "The Salt Road X"));
        QCOMPARE(saved.meta.publisher, QString(theirs ? "Changed Elsewhere" : "My Press"));
        QCOMPARE(saved.meta.description, QString(theirs ? "written by another program" : "written here"));
        QCOMPARE(saved.rating, 2);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // In a low or a narrow window the dialog scrolls; why Save did not save
    // is in sight all the same, or it is pressed again unread (#257).
    void whySaveDidNotSaveIsInSight_data() {
        QTest::addColumn<QSize>("size");
        QTest::newRow("low") << QSize(1280, 720);
        QTest::newRow("narrow") << QSize(420, 700);
        QTest::newRow("smallest") << QSize(420, 360);
    }

    void whySaveDidNotSaveIsInSight() {
        QFETCH(const QSize, size);
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.desk.resize(size));
        QVERIFY(bench.edit());
        bench.set("editNumber", "text", "abc");
        bench.call("edit", "save");
        QTRY_VERIFY(bench.inSight("editNoNumber"));
        QVERIFY(bench.inSight("editSave"));
        bench.set("editNumber", "text", "");
        QVERIFY(bench.changeElsewhere("Theirs"));
        bench.set("editTitle", "text", "The Salt Road mine");
        bench.call("edit", "save");
        QTRY_VERIFY(bench.inSight("editChanged"));
        QTRY_VERIFY(bench.inSight("editTakeTheirs"));
        QVERIFY(bench.inSight("editSave"));
        bench.desk.shot(QString("edit-changed-%1x%2").arg(size.width()).arg(size.height()));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Tab from the status reaches the stars, and the keys set them (#272).
    void theStarsAreSetByTheKeyboard() {
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        QQuickItem *stars = bench.desk.item("editStars");
        QVERIFY(stars);
        bench.desk.item("editShelf")->forceActiveFocus(Qt::TabFocusReason);
        QTest::keyClick(bench.desk.window, Qt::Key_Tab);
        QTRY_VERIFY(stars->hasActiveFocus());
        const auto value = [stars] { return stars->property("value").toInt(); };
        QTest::keyClick(bench.desk.window, Qt::Key_4);
        QCOMPARE(value(), 4);
        QTest::keyClick(bench.desk.window, Qt::Key_Right);
        QTest::keyClick(bench.desk.window, Qt::Key_Right);
        QCOMPARE(value(), 5);
        QTest::keyClick(bench.desk.window, Qt::Key_Left);
        QCOMPARE(value(), 4);
        QTest::keyClick(bench.desk.window, Qt::Key_0);
        QTest::keyClick(bench.desk.window, Qt::Key_Left);
        QCOMPARE(value(), 0);
        QTest::keyClick(bench.desk.window, Qt::Key_3);
        bench.desk.shot("edit-stars-focused");
        // Tab goes on, and back.
        QTest::keyClick(bench.desk.window, Qt::Key_Tab);
        QTRY_VERIFY(bench.desk.item("editFinished")->hasActiveFocus());
        QTest::keyClick(bench.desk.window, Qt::Key_Backtab, Qt::ShiftModifier);
        QTRY_VERIFY(stars->hasActiveFocus());
        QTest::keyClick(bench.desk.window, Qt::Key_Return);
        QTRY_VERIFY(!bench.property("edit", "visible").toBool());
        QCOMPARE(bench.book().rating, 3);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    void aChangeAfterTheWarningIsSaidAgain() {
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        QVERIFY(bench.changeElsewhere());
        bench.set("editPublisher", "text", "My Press");
        bench.call("edit", "save");
        QVERIFY(bench.property("editChanged", "visible").toBool());
        BookMeta meta = bench.book().meta;
        meta.publisher = "Changed Again";
        QVERIFY(bench.other->setMeta(bench.id, meta));
        bench.call("edit", "save");
        QVERIFY(bench.property("edit", "opened").toBool());
        QCOMPARE(bench.book().meta.publisher, QString("Changed Again"));
    }

    void aBookRemovedMeanwhileIsSaidAndStaysRemoved() {
        Bench bench;
        QVERIFY(bench.open());
        QVERIFY(bench.edit());
        bench.set("editTitle", "text", "The Salt Road X");
        QVERIFY(bench.other->remove(bench.id).has_value());
        bench.call("edit", "save");
        QVERIFY(bench.property("edit", "opened").toBool());
        QVERIFY(bench.property("editChanged", "visible").toBool());
        QVERIFY(bench.property("editChanged", "text").toString().contains("removed"));
        QVERIFY(!bench.property("editSave", "enabled").toBool());
        // What was typed is still there to copy; the book stays gone.
        QCOMPARE(bench.property("editTitle", "text").toString(), QString("The Salt Road X"));
        QVERIFY(bench.other->books().isEmpty());
        // Another book's dialog starts afresh.
        bench.call("edit", "showNew");
        QVERIFY(bench.property("editSave", "enabled").toBool());
        QVERIFY(!bench.property("editChanged", "visible").toBool());
    }

    void aCollectionKeepsWhatWasChangedElsewhere() {
        Bench bench;
        QVERIFY(bench.open());
        const auto made = bench.desk.backend->library()->addCollection("Harbours", "tag:harbour");
        QVERIFY(made.has_value());
        bench.desk.backend->reload();
        QObject *dialog = bench.desk.find("collectionDialog");
        QVERIFY(dialog);
        QMetaObject::invokeMethod(dialog, "change", Q_ARG(QVariant, *made));
        QTRY_VERIFY(dialog->property("opened").toBool());
        QVERIFY(bench.other->renameCollection(*made, "Ports"));
        QTRY_COMPARE(dialog->property("editing").toMap().value("name").toString(), QString("Ports"));
        bench.set("collectionRule", "text", "tag:salt");
        bench.call("collectionDialog", "save");
        QCOMPARE(bench.other->collection(*made)->name, QString("Ports"));
        QCOMPARE(bench.other->collection(*made)->rule, QString("tag:salt"));
    }

    void aThingOfTheGraphKeepsWhatWasWrittenElsewhere() {
        Bench bench;
        QVERIFY(bench.open());
        Library &own = *bench.desk.backend->library();
        QVERIFY(own.setReading(bench.id, "read", 4, {}));
        const qint64 pell = Graph(own).upsert({}, "character", "Pell", "A keeper.", {}, GraphPlace{bench.id, 0, 0.1});
        QVERIFY(pell > 0);
        QTest::keyClick(bench.desk.window, Qt::Key_6);
        QTRY_VERIFY(bench.desk.find("libraryGraph") && bench.desk.find("graphEdit"));
        auto *browser = bench.property("graphEdit", "browser").value<QObject *>();
        QVERIFY(browser);
        const auto details = [browser] { return browser->property("details").toMap(); };
        QTRY_VERIFY(browser->setProperty("selected", pell) && details().value("name").toString() == "Pell");
        QMetaObject::invokeMethod(bench.desk.find("graphEdit"), "show", Q_ARG(QVariant, "edit"), Q_ARG(QVariant, details()),
                                  Q_ARG(QVariant, QVariant()), Q_ARG(QVariant, QVariant()));
        QTRY_VERIFY(bench.property("graphEdit", "opened").toBool());
        // A build or an agent writes what is known of it.
        QVERIFY(Graph::ofLibrary(own).edit(pell, "Pell", "A keeper of lanterns."));
        GraphFeed::announce();
        QTRY_COMPARE(details().value("summary").toString(), QString("A keeper of lanterns."));
        bench.set("graphEditName", "text", "Old Pell");
        bench.call("graphEdit", "save");
        QTRY_VERIFY(!bench.property("graphEdit", "visible").toBool());
        QCOMPARE(Graph::ofLibrary(own).node(pell)->name, QString("Old Pell"));
        QCOMPARE(Graph::ofLibrary(own).node(pell)->summary, QString("A keeper of lanterns."));
    }

    void aNoteLeftAsItWasKeepsWhatWasChangedElsewhere() {
        Desk desk{"en", {}, false};
        const QList<qint64> ids = stock(desk);
        QVERIFY(!ids.isEmpty() && desk.show());
        desk.quicken();
        QVERIFY(desk.resize(QSize(1500, 900)));
        desk.reader->openBook(ids.first());
        QTRY_VERIFY(desk.reader->isOpen() && desk.shows("flowText"));
        desk.settle();
        QMetaObject::invokeMethod(desk.item("flowText"), "select", Q_ARG(int, 0), Q_ARG(int, 7));
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ShiftModifier);
        const auto typing = [&desk] {
            const QQuickItem *focused = desk.window->activeFocusItem();
            return focused && focused->objectName() == "noteField";
        };
        QTRY_VERIFY(typing());
        for (const char letter : QByteArray("Mine"))
            QTest::keyClick(desk.window, letter);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        Library other(desk.backend->library()->root());
        QVERIFY(other.open());
        QTRY_COMPARE(other.annotations(ids.first(), "margin-note").size(), 1);
        Annotation note = other.annotations(ids.first(), "margin-note").first();
        QObject *layer = desk.find("readingNotes");
        // Opened to be changed, and changed elsewhere while it is.
        QMetaObject::invokeMethod(layer, "open", Q_ARG(QVariant, note.id), Q_ARG(QVariant, QVariant::fromValue(desk.item("flowText"))),
                                  Q_ARG(QVariant, 0), Q_ARG(QVariant, 7));
        QTRY_VERIFY(typing());
        note.note = "Theirs";
        QVERIFY(other.storeAnnotation(ids.first(), note).has_value());
        desk.backend->reload(); // as the window does once it sees the change
        desk.settle();
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!typing());
        QCOMPARE(other.annotations(ids.first(), "margin-note").first().note, QString("Theirs"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(EditDialogTests)
#include "editdialog_tests.moc"
