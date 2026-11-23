// The Snapshots panel (#185), from the settings, the library's menu and its
// key: snapshots set up with the recovery key shown once, one taken now, the
// list filtered, the diff against now, a book put back from it after asking
// with the moment snapshot to undo it, the whole library into a new folder
// only, the destinations added, tested and taken off, and the warnings in red.
// In a repository and a library of the test's own, with restic. Surveyed in
// German, under fingers and right to left.

#include <QDir>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QtTest>
#include <utility>

#include "bookmeta.h"
#include "desk.h"
#include "failure.h"
#include "library.h"
#include "snapshotdesk.h"

using namespace Qt::StringLiterals;

namespace {

// restic, run on the worker, can take a while on a busy machine.
constexpr int slow = 60000;

[[nodiscard]] qint64 entry(Library &library, const QString &title) {
    BookMeta meta;
    meta.title = title;
    meta.authors = {"Edda Marlowe"};
    return library.addEntry(meta, true).id;
}

[[nodiscard]] bool rename(Library &library, qint64 id, const QString &title) {
    BookMeta meta = library.book(id)->meta;
    meta.title = title;
    return library.setMeta(id, meta);
}

[[nodiscard]] QString titleOf(const Library &library, qint64 id) {
    const auto book = library.book(id);
    return book ? book->meta.title : QString();
}

} // namespace

class SnapshotsPanelTests : public QObject {
    Q_OBJECT

    static QVariant value(const Desk &desk, const QString &name, const char *property) {
        QObject *found = desk.find(name);
        return found ? found->property(property) : QVariant();
    }
    static QString text(const Desk &desk, const QString &name) {
        QObject *item = onScreen(desk, name);
        return item ? item->property("text").toString() : value(desk, name, "text").toString();
    }
    // The item of that name on screen. A list made again (each answer of the
    // panel's is a new one) leaves the old items for a moment, out of sight,
    // so the first of the name found may be one of those.
    static QQuickItem *onScreen(const Desk &desk, const QString &name, bool pressable = false) {
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (!item->isVisible())
                continue;
            if (item->objectName() == name && item->width() > 0 && item->height() > 0 && (!pressable || item->isEnabled()))
                return item;
            items << item->childItems();
        }
        return nullptr;
    }
    static bool shown(const Desk &desk, const QString &name) { return onScreen(desk, name) != nullptr; }
    static void press(const Desk &desk, const QString &name) {
        QVERIFY2(QTest::qWaitFor([&] { return onScreen(desk, name, true) != nullptr; }, 5000), qPrintable(name));
        QVERIFY(QMetaObject::invokeMethod(onScreen(desk, name, true), "clicked"));
    }
    static void type(const Desk &desk, const QString &name, const QString &words) {
        QObject *field = desk.find(name);
        QVERIFY2(field, qPrintable(name));
        QVERIFY(field->setProperty("text", words));
    }
    // Nothing being done any more.
    static bool idle(const Desk &desk) {
        return QTest::qWaitFor([&] { return desk.snapshotDesk->doing().isEmpty(); }, slow);
    }
    static bool open(Desk &desk) {
        desk.call("showSnapshots");
        return desk.waitForPopup() && value(desk, "snapshots", "opened").toBool() && idle(desk);
    }
    // Set up in the test's own repository, with a snapshot of the library as it is.
    static bool setUpWithOne(Desk &desk) {
        desk.snapshotDesk->setUp(desk.dir.filePath("repository"));
        if (!idle(desk) || !desk.snapshotDesk->ready())
            return false;
        desk.snapshotDesk->takeNow();
        return idle(desk) && desk.snapshotDesk->snapshots().size() == 1;
    }

    QStringList found;
    void look(Desk &desk, const QString &state) {
        QVERIFY2(desk.pictured(), qPrintable(state));
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("snapshots-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        QVERIFY2(!QStandardPaths::findExecutable("restic").isEmpty(), "restic is one of Omabook's dependencies");
    }

    // From the settings: not set up, so set up here, the key shown once; then a snapshot now, and the list.
    void setUpAndTakeOneNow() {
        Desk desk("en", {}, false);
        QVERIFY(entry(*desk.backend->library(), "Moss") > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.call("showSettings");
        QVERIFY(desk.waitForPopup());
        QVERIFY2(text(desk, "snapshotsSummary").startsWith("Not set up"), qPrintable(text(desk, "snapshotsSummary")));
        press(desk, "openSnapshots");
        QTRY_VERIFY(value(desk, "snapshots", "opened").toBool());
        QVERIFY(idle(desk));
        QVERIFY(shown(desk, "snapshotSetUp"));
        QVERIFY(!shown(desk, "snapshotNow"));

        const QString repository = desk.dir.filePath("repository");
        type(desk, "snapshotRepository", repository);
        press(desk, "snapshotSetUp");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        QVERIFY(desk.snapshotDesk->ready());
        QCOMPARE(desk.snapshotDesk->repository(), repository);
        QTRY_VERIFY(shown(desk, "snapshotKey"));
        QCOMPARE(text(desk, "snapshotKeyText").remove('-').size(), 32);
        QVERIFY(QFileInfo::exists(repository + "/config"));
        // Shown this once: gone once kept, and not shown again.
        press(desk, "snapshotKeyKept");
        QTRY_VERIFY(!shown(desk, "snapshotKey"));
        QVERIFY(desk.snapshotDesk->recoveryKey().isEmpty());

        QTRY_VERIFY(shown(desk, "snapshotsNone"));
        press(desk, "snapshotNow");
        QVERIFY(idle(desk));
        QVERIFY2(text(desk, "snapshotsStatus").startsWith("Snapshot ") && text(desk, "snapshotsStatus").endsWith(" taken."),
                 qPrintable(text(desk, "snapshotsStatus")));
        QTRY_COMPARE(value(desk, "snapshotList", "count").toInt(), 1);
        QVERIFY2(text(desk, "snapshotNewest").contains("Taken when asked"), qPrintable(text(desk, "snapshotNewest")));
        // Nothing changed since: none taken.
        press(desk, "snapshotNow");
        QVERIFY(idle(desk));
        QVERIFY2(text(desk, "snapshotsStatus").startsWith("Nothing changed since snapshot"), qPrintable(text(desk, "snapshotsStatus")));
        QCOMPARE(desk.snapshotDesk->snapshots().size(), 1);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The diff against now, a book put back after asking, and undone from the moment snapshot taken first.
    void aBookPutBackFromTheDiffAndUndone() {
        Desk desk("en", {}, false);
        Library &library = *desk.backend->library();
        const qint64 moss = entry(library, "Moss");
        QVERIFY(setUpWithOne(desk));
        QVERIFY(rename(library, moss, "Moss, Renamed"));
        QVERIFY(entry(library, "Granite") > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(open(desk));
        QTRY_COMPARE(value(desk, "snapshotList", "count").toInt(), 1);

        press(desk, "snapshotCompare");
        QVERIFY(idle(desk));
        QTRY_VERIFY(shown(desk, "snapshotBookChanged"));
        QVERIFY2(text(desk, "snapshotDiffFrom").contains("Taken when asked"), qPrintable(text(desk, "snapshotDiffFrom")));
        QVERIFY(shown(desk, "snapshotBookAdded"));
        QVERIFY(!shown(desk, "snapshotBookRemoved"));
        const QVariantMap books = desk.snapshotDesk->diff().value("books").toMap();
        QCOMPARE(books.value("added").toList().size(), 1);
        QCOMPARE(books.value("changed").toList().size(), 1);
        const QVariantMap changed = books.value("changed").toList().first().toMap();
        QCOMPARE(changed.value("title").toString(), QString("Moss, Renamed"));

        // Asked first: what changes, from now to the snapshot, and nothing put back yet.
        press(desk, "snapshotPutBack");
        QVERIFY(idle(desk));
        QTRY_VERIFY(shown(desk, "snapshotPutBackAsk"));
        QVERIFY(shown(desk, "snapshotPutBackDetails"));
        QCOMPARE(titleOf(library, moss), QString("Moss, Renamed"));
        press(desk, "snapshotPutBackCancel");
        QTRY_VERIFY(!shown(desk, "snapshotPutBackAsk"));
        QCOMPARE(titleOf(library, moss), QString("Moss, Renamed"));

        press(desk, "snapshotPutBack");
        QVERIFY(idle(desk));
        QTRY_VERIFY(shown(desk, "snapshotPutBackYes"));
        press(desk, "snapshotPutBackYes");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        QCOMPARE(titleOf(library, moss), QString("Moss"));
        // A moment snapshot was taken first, and is the undo.
        const QString undo = desk.snapshotDesk->restored().value("undo").toString();
        QVERIFY(!undo.isEmpty());
        QVERIFY2(text(desk, "snapshotsStatus").contains(undo.left(8)), qPrintable(text(desk, "snapshotsStatus")));
        QCOMPARE(desk.snapshotDesk->snapshots().size(), 2);
        QCOMPARE(desk.snapshotDesk->snapshots().first().toMap().value("reason").toString(), QString("restore"));
        QVERIFY(shown(desk, "snapshotUndo"));

        press(desk, "snapshotUndo");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        QCOMPARE(titleOf(library, moss), QString("Moss, Renamed"));

        // The list again, filtered: the moment snapshots before a restore, and no nightly one.
        press(desk, "snapshotsBack");
        QTRY_VERIFY(shown(desk, "snapshotList"));
        type(desk, "snapshotFilter", "before a restore");
        QTRY_COMPARE(value(desk, "snapshotList", "count").toInt(), desk.snapshotDesk->snapshots().size() - 1);
        type(desk, "snapshotFilter", "");
        QObject *kinds = desk.find("snapshotKinds");
        QVERIFY(kinds);
        QVERIFY(kinds->setProperty("currentIndex", 1));
        QVERIFY(QMetaObject::invokeMethod(kinds, "activated", Q_ARG(int, 1)));
        QTRY_VERIFY(shown(desk, "snapshotsNone"));
        QCOMPARE(text(desk, "snapshotsNone"), QString("No snapshot fits the filter."));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Between two snapshots, the later one picked as the other side.
    void aDiffBetweenTwoSnapshots() {
        Desk desk("en", {}, false);
        Library &library = *desk.backend->library();
        QVERIFY(entry(library, "Moss") > 0);
        QVERIFY(setUpWithOne(desk));
        QVERIFY(entry(library, "Granite") > 0);
        desk.snapshotDesk->takeNow();
        QVERIFY(idle(desk));
        QVERIFY(entry(library, "Small Hours") > 0);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(open(desk));
        const QString first = desk.snapshotDesk->snapshots().last().toMap().value("id").toString();
        const QString second = desk.snapshotDesk->snapshots().first().toMap().value("id").toString();
        desk.snapshotDesk->compare(first, "");
        QVERIFY(idle(desk));
        QCOMPARE(desk.snapshotDesk->diff().value("books").toMap().value("added").toList().size(), 2);
        desk.snapshotDesk->compare(first, second);
        QVERIFY(idle(desk));
        const QVariantList added = desk.snapshotDesk->diff().value("books").toMap().value("added").toList();
        QCOMPARE(added.size(), 1);
        QCOMPARE(added.first().toMap().value("title").toString(), QString("Granite"));
        QCOMPARE(desk.snapshotDesk->diff().value("to").toMap().value("id").toString(), second);
    }

    // The whole library goes into a new folder, never over the one in use, and can be opened from there.
    void theWholeLibraryIntoANewFolderOnly() {
        Desk desk("en", {}, false);
        Library &library = *desk.backend->library();
        const qint64 moss = entry(library, "Moss");
        QVERIFY(setUpWithOne(desk));
        QVERIFY(rename(library, moss, "Moss, Renamed"));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(open(desk));
        press(desk, "snapshotRestoreWhole");
        QTRY_VERIFY(shown(desk, "snapshotWholeFolder"));
        QVERIFY2(text(desk, "snapshotWholeFolder").startsWith(library.root() + " "), qPrintable(text(desk, "snapshotWholeFolder")));

        // Over the library in use: refused, in red, and nothing changed.
        type(desk, "snapshotWholeFolder", library.root());
        press(desk, "snapshotWholeRestore");
        QVERIFY(idle(desk));
        QVERIFY(desk.snapshotDesk->failed());
        QVERIFY(shown(desk, "snapshotsStatus"));
        QCOMPARE(titleOf(library, moss), QString("Moss, Renamed"));

        const QString folder = desk.dir.filePath("Restored");
        type(desk, "snapshotWholeFolder", folder);
        press(desk, "snapshotWholeRestore");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        QCOMPARE(desk.snapshotDesk->restoredInto(), folder);
        Library restored(folder);
        QVERIFY(restored.open());
        QCOMPARE(restored.books().size(), 1);
        QCOMPARE(restored.books().first().meta.title, QString("Moss"));
        QCOMPARE(titleOf(library, moss), QString("Moss, Renamed"));
        QTRY_VERIFY(shown(desk, "snapshotOpenRestored"));
        press(desk, "snapshotOpenRestored");
        QTRY_COMPARE(desk.backend->libraryPath(), folder);
        QTRY_VERIFY(!value(desk, "snapshots", "opened").toBool());
    }

    // A second disk added, its key shown once, tested, and taken off again after asking.
    void aDestinationAddedTestedAndRemoved() {
        Desk desk("en", {}, false);
        QVERIFY(entry(*desk.backend->library(), "Moss") > 0);
        QVERIFY(setUpWithOne(desk));
        QVERIFY(QDir().mkpath(desk.dir.filePath("second-disk")));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(open(desk));
        QVERIFY2(text(desk, "snapshotBackedUp").startsWith("Not backed up yet"), qPrintable(text(desk, "snapshotBackedUp")));
        press(desk, "openDestinations");
        QTRY_VERIFY(shown(desk, "snapshotDestinationName"));
        type(desk, "snapshotDestinationName", "second-disk");
        type(desk, "snapshotDestinationLocation", desk.dir.filePath("second-disk/snapshots"));
        press(desk, "snapshotDestinationAdd");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        QTRY_VERIFY(shown(desk, "snapshotKey"));
        QCOMPARE(desk.snapshotDesk->recoveryKey().value("for").toString(), QString("second-disk"));
        QCOMPARE(desk.snapshotDesk->destinations().size(), 1);
        QTRY_VERIFY(shown(desk, "snapshotDestination"));
        QVERIFY(text(desk, "snapshotDestinationName").isEmpty());

        press(desk, "snapshotDestinationTest");
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        QTRY_VERIFY(shown(desk, "snapshotDestinationTested"));
        QCOMPARE(text(desk, "snapshotDestinationTested"), QString("Reached. It holds no snapshot yet."));

        // A name it can't have is refused, and said why.
        type(desk, "snapshotDestinationName", "Not A Name");
        type(desk, "snapshotDestinationLocation", desk.dir.filePath("second-disk/other"));
        press(desk, "snapshotDestinationAdd");
        QVERIFY(idle(desk));
        QVERIFY(desk.snapshotDesk->failed());
        QCOMPARE(desk.snapshotDesk->destinations().size(), 1);

        press(desk, "snapshotDestinationRemove");
        QTRY_VERIFY(shown(desk, "snapshotDestinationRemoveYes"));
        QCOMPARE(desk.snapshotDesk->destinations().size(), 1);
        press(desk, "snapshotDestinationRemoveYes");
        QVERIFY(idle(desk));
        QCOMPARE(desk.snapshotDesk->destinations().size(), 0);
        QTRY_VERIFY(!shown(desk, "snapshotDestination"));
        // What was kept there stays.
        QVERIFY(QFileInfo::exists(desk.dir.filePath("second-disk/snapshots/config")));
    }

    // What is wrong comes first, in red, saying what to do; the settings say it too.
    void warningsInRed() {
        Desk desk("en", {}, false);
        const QString warning = "No snapshot of Library for 3 days: take one now, or see why the nightly one fails.";
        desk.snapshotDesk.emplace(
            SnapshotPlace::in(desk.dir.filePath("snapshots")),
            SnapshotContents{.settingsFile = desk.dir.filePath("config.json"), .pluginsFolder = desk.dir.filePath("plugins")},
            [&desk] { return desk.backend->libraryPath(); },
            [warning](const Snapshots &, const Library &) { return QStringList{warning}; });
        QVERIFY(entry(*desk.backend->library(), "Moss") > 0);
        QVERIFY(setUpWithOne(desk));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "snapshotWarning"));
        QCOMPARE(text(desk, "snapshotWarning"), warning);
        const QColor red = desk.look->colors().value("red").value<QColor>();
        QCOMPARE(value(desk, "snapshotWarning", "color").value<QColor>(), red);
        QVERIFY(desk.closePopups());
        desk.call("showSettings");
        QVERIFY(desk.waitForPopup());
        QVERIFY(idle(desk));
        QTRY_COMPARE(text(desk, "snapshotsSummary"), warning);
        QCOMPARE(value(desk, "snapshotsSummary", "color").value<QColor>(), red);
    }

    // The warnings of the checks (#184), as `omabook snapshot status` says them: a second disk and no place away.
    void theChecksWarningsAreShown() {
        Desk desk("en", {}, false);
        desk.snapshotDesk.emplace(
            SnapshotPlace::in(desk.dir.filePath("snapshots")),
            SnapshotContents{.settingsFile = desk.dir.filePath("config.json"), .pluginsFolder = desk.dir.filePath("plugins")},
            [&desk] { return desk.backend->libraryPath(); }, SnapshotDesk::health());
        QVERIFY(entry(*desk.backend->library(), "Moss") > 0);
        QVERIFY(setUpWithOne(desk));
        QVERIFY(QDir().mkpath(desk.dir.filePath("second-disk")));
        desk.snapshotDesk->addDestination("second-disk", desk.dir.filePath("second-disk/snapshots"), {}, {});
        QVERIFY(idle(desk));
        QVERIFY2(!desk.snapshotDesk->failed(), qPrintable(desk.snapshotDesk->status()));
        const QString away = Failure(Failure::Code::NoCopyAway).text();
        QVERIFY2(desk.snapshotDesk->warnings().contains(away), qPrintable(desk.snapshotDesk->warnings().join('\n')));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(open(desk));
        QTRY_VERIFY(shown(desk, "snapshotWarning"));
    }

    // The library's menu and its key open it.
    void theMenuAndTheKeyOpenIt() {
        Desk desk("en", {}, false);
        QVERIFY(desk.show());
        QVERIFY(desk.run("library.snapshots"));
        QTRY_VERIFY(value(desk, "snapshots", "opened").toBool());
        QVERIFY(desk.closePopups());
        desk.menu("library");
        QVERIFY(desk.waitForPopup());
        QObject *menu = desk.find("commandMenu");
        const QVariantList entries = menu->property("entries").toList();
        const auto entry =
            std::ranges::find_if(entries, [](const QVariant &each) { return each.toMap().value("id") == "library.snapshots"; });
        QVERIFY2(entry != entries.end(), "the library's menu has no Snapshots");
        QVERIFY(QMetaObject::invokeMethod(menu, "take", Q_ARG(QVariant, *entry)));
        QTRY_VERIFY(value(desk, "snapshots", "opened").toBool());
        QVERIFY(idle(desk));
        QVERIFY(shown(desk, "snapshotSetUp"));
    }

    // Each page at every size, in a long language, under fingers and right to left.
    void thePanelAtEverySize() {
        for (const auto &[language, touch] : {std::pair{QByteArray("de"), false}, {QByteArray("de"), true}, {QByteArray("ar"), false}}) {
            Desk desk(language, touch ? QByteArray() : lightTheme, touch);
            Library &library = *desk.backend->library();
            const QList<qint64> books = stock(desk);
            QVERIFY(setUpWithOne(desk));
            QVERIFY(rename(library, books.at(1), "Moss, a Second Title Rather Longer Than the First"));
            QVERIFY(QDir().mkpath(desk.dir.filePath("second-disk")));
            desk.snapshotDesk->addDestination("second-disk", desk.dir.filePath("second-disk/snapshots"), {}, {});
            QVERIFY(idle(desk));
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            QVERIFY(open(desk));
            const QVariantMap snapshot = desk.snapshotDesk->snapshots().first().toMap();
            const auto every = [&](const QString &state) {
                for (const QSize &size : dialogSizes()) {
                    QVERIFY(desk.resize(size));
                    desk.quicken();
                    desk.settle();
                    look(desk, state);
                }
            };
            QTRY_VERIFY(shown(desk, "snapshotList"));
            every("list");
            QVERIFY(QMetaObject::invokeMethod(desk.find("snapshots"), "compare", Q_ARG(QVariant, snapshot), Q_ARG(QVariant, "")));
            QVERIFY(idle(desk));
            QTRY_VERIFY(shown(desk, "snapshotPutBack"));
            press(desk, "snapshotPutBack");
            QVERIFY(idle(desk));
            QTRY_VERIFY(shown(desk, "snapshotPutBackAsk"));
            every("diff");
            QVERIFY(QMetaObject::invokeMethod(desk.find("snapshots"), "restoreWhole", Q_ARG(QVariant, snapshot)));
            QTRY_VERIFY(shown(desk, "snapshotWholeFolder"));
            every("whole");
            press(desk, "snapshotsBack");
            press(desk, "openDestinations");
            QTRY_VERIFY(shown(desk, "snapshotDestination"));
            press(desk, "snapshotCredentialAdd");
            QTRY_VERIFY(shown(desk, "snapshotCredentialValue"));
            every("destinations");
            QVERIFY(desk.closePopups());
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = std::exchange(found, {});
        QVERIFY2(problems.isEmpty(), qPrintable(problems.mid(0, 40).join('\n')));
    }
};

QTEST_MAIN(SnapshotsPanelTests)
#include "snapshotspanel_tests.moc"
