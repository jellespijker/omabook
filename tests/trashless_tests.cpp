// Where no trash folder can be made, nothing is deleted: removing a book, a
// tidy's fold, a removal a sync brings and a library thrown away are refused,
// and everything stays where it was (#285).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtTest>

#include "backend.h"
#include "fixtures.h"
#include "library.h"
#include "mcpharness.h"
#include "remote.h"
#include "settings.h"
#include "sync.h"
#include "trash.h"

using namespace Qt::StringLiterals;

namespace {

// A data folder of this test's own, where the home's trash is looked for.
void useDataFolder(const QString &folder) {
    QDir().mkpath(folder);
    qputenv("XDG_DATA_HOME", QFile::encodeName(folder));
}

// A trash that can't be made, nor used: a file stands where its folder goes.
[[nodiscard]] bool blockTrash(const QString &dataFolder) {
    return QDir(dataFolder + "/Trash").removeRecursively() && QDir().mkpath(dataFolder)
           && Fixtures::write(dataFolder + "/Trash", "in the way");
}

[[nodiscard]] qint64 addText(Library &library, const QString &folder, const QString &title, const QByteArray &words) {
    const QString file = folder + u'/' + title + ".txt";
    return Fixtures::write(file, words) ? library.add(file, BookMeta{.title = title}).id : 0;
}

[[nodiscard]] SyncSummary syncFrom(Library &here, const QString &there, const QTemporaryDir &dir, const SyncOptions &options = {}) {
    const QString ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
    return syncLibrary(here, Remote("laptop:" + there, {ssh}), {}, options);
}

// A removal that was refused is said, once, and the two are never called in step.
void saysItWasRefused(const SyncSummary &summary) {
    const QString said = summary.problems.join(u'\n');
    QCOMPARE(summary.problems.filter(u"could not be removed"_s).size(), 1);
    QVERIFY2(said.contains(u"trash"_s), qPrintable(said));
    QCOMPARE(summary.removedHere + summary.removedThere, 0);
    QVERIFY(!Backend::syncLines(summary).contains(Backend::tr("Both libraries were already in step.")));
}

} // namespace

class TrashlessTests : public QObject {
    Q_OBJECT

private slots:
    void removingIsRefusedAndNothingIsForgotten() {
        const McpHarness::Machine machine;
        const QString data = machine.dir.filePath("data");
        useDataFolder(data);
        QString file;
        {
            Library library(machine.library);
            QVERIFY(library.open());
            QCOMPARE(addText(library, machine.dir.path(), u"Salt"_s, "Salt.\n"), 1);
            file = library.formatPath(1);
        }
        QVERIFY(blockTrash(data));
        const auto ran = machine.omabook({"remove", "1", "--library", machine.library});
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.errors.contains("trash") && ran.errors.contains("nothing was deleted"), qPrintable(ran.errors));
        Library library(machine.library);
        QVERIFY(library.open());
        QVERIFY(library.book(1));
        QVERIFY(QFileInfo::exists(file));
        QVERIFY(library.removals().isEmpty());
        // As it says: the folder deleted by the person, the book can go.
        QVERIFY(QDir(QFileInfo(file).absolutePath()).removeRecursively());
        QVERIFY(library.remove(1));
        QVERIFY(!library.book(1));
    }

    void aBookWithNothingOnDiskNeedsNoTrash() {
        QTemporaryDir dir;
        useDataFolder(dir.filePath("data"));
        QVERIFY(blockTrash(dir.filePath("data")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 paper = library.addEntry({.title = u"On paper"_s}, true).id;
        QVERIFY(library.remove(paper));
        QVERIFY(!library.book(paper));
        // The library's own folder is never taken for a book's.
        QVERIFY(Trash::folder(library.root(), library.root() + "/"));
        QVERIFY(Trash::folder(library.root(), dir.path()));
        QVERIFY(QFileInfo::exists(library.databasePath()));
    }

    void theFirstRemovalOnAFreshHomeGoesToTheTrash() {
        QTemporaryDir dir;
        useDataFolder(dir.filePath("home/.local/share"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = addText(library, dir.path(), u"Salt"_s, "Salt.\n");
        QVERIFY(library.remove(id));
        const QDir trashed(dir.filePath("home/.local/share/Trash/files"));
        QCOMPARE(trashed.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
    }

    void aFoldIsRefusedAndTheFoldedBookStaysWhole() {
        QTemporaryDir dir;
        useDataFolder(dir.filePath("data"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 kept = addText(library, dir.path(), u"Salt"_s, "One edition.\n");
        QDir().mkpath(dir.filePath("other"));
        const qint64 folded = addText(library, dir.filePath("other"), u"Salt"_s, "Another edition.\n");
        QVERIFY(library.setIdentifier(folded, "goodreads", "7"));
        QVERIFY(library.addAnnotation(folded, Annotation{.kind = u"bookmark"_s}) > 0);
        const QString file = library.formatPath(folded);
        QVERIFY(blockTrash(dir.filePath("data")));
        QString why;
        QVERIFY(!library.merge(kept, folded, &why));
        QVERIFY2(why.contains("trash"), qPrintable(why));
        QVERIFY(QFileInfo::exists(file));
        QVERIFY(library.book(folded));
        QCOMPARE(library.annotations(folded).size(), 1);
        QVERIFY(library.annotations(kept).isEmpty());
        QVERIFY(library.removals().isEmpty());
    }

    void aSyncedRemovalIsRefusedOnTheComputerWithNoTrash_data() {
        QTest::addColumn<bool>("refusedHere");
        QTest::newRow("taken here") << true;
        QTest::newRow("sent there") << false;
    }

    void aSyncedRemovalIsRefusedOnTheComputerWithNoTrash() {
        QFETCH(bool, refusedHere);
        QTemporaryDir dir;
        useDataFolder(dir.filePath("data"));
        Library a(dir.filePath("A")), b(dir.filePath("B"));
        QVERIFY(a.open());
        QVERIFY(b.open());
        QVERIFY(addText(a, dir.path(), u"Salt"_s, "Salt.\n") > 0);
        QCOMPARE(syncFrom(a, b.root(), dir).problems, QStringList());
        QCOMPARE(b.books().size(), 1);
        const QString file = b.formatPath(b.books().first().id);
        QTest::qWait(1100); // removed after it came there
        QVERIFY(a.remove(a.books().first().id));
        // A book that crosses in the same sync, which then carries the log twice.
        QVERIFY(addText(a, dir.path(), u"Moss"_s, "Moss.\n") > 0);
        const auto sync = [&](const SyncOptions &options = {}) {
            return refusedHere ? syncFrom(b, a.root(), dir, options) : syncFrom(a, b.root(), dir, options);
        };
        const QString trash = refusedHere ? dir.filePath("data/Trash") : dir.filePath("there-home/.local/share/Trash");
        // B, where the removal arrives, has no trash: here, or the other computer.
        if (refusedHere) {
            QVERIFY(blockTrash(dir.filePath("data")));
        } else {
            std::ignore = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
            QVERIFY(blockTrash(dir.filePath("there-home/.local/share")));
        }
        // Said by the sync that brings it, and by each after it, with nothing lost.
        for (int again = 0; again < 2; ++again) {
            saysItWasRefused(sync());
            QVERIFY(QFileInfo::exists(file));
            QCOMPARE(b.books().size(), 2);
            QCOMPARE(a.books().size(), 1); // not brought back where it was removed
        }
        // A dry run still has it to do.
        const SyncSummary dry = sync(SyncOptions{.dryRun = true});
        QCOMPARE(dry.removedHere + dry.removedThere, 1);
        // With a trash to be had, the next sync removes it, and they are in step.
        QVERIFY(QFile::remove(trash));
        const SyncSummary mended = sync();
        QCOMPARE(mended.problems, QStringList());
        QCOMPARE(refusedHere ? mended.removedHere : mended.removedThere, 1);
        QVERIFY(!QFileInfo::exists(file));
        QCOMPARE(b.books().size(), 1);
        const SyncSummary after = sync();
        QCOMPARE(after.problems, QStringList());
        QCOMPARE(after.removedHere + after.removedThere + after.keptHere + after.keptThere, 0);
    }

    void aLibraryThatCantGoToTheTrashStays() {
        const McpHarness::Machine machine;
        useDataFolder(machine.dir.filePath("data"));
        const QString other = machine.dir.filePath("Other");
        QCOMPARE(machine.omabook({"libraries", "--use", other, "--create"}).code, 0);
        QCOMPARE(machine.omabook({"libraries", "--use", machine.library, "--create"}).code, 0);
        QVERIFY(blockTrash(machine.dir.filePath("data")));
        const auto ran = machine.omabook({"libraries", "--trash", other});
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.errors.contains("trash"), qPrintable(ran.errors));
        QVERIFY(QFileInfo::exists(Library::databaseIn(other)));
        QVERIFY(machine.omabook({"libraries"}).output.contains(other));
    }
};

QTEST_MAIN(TrashlessTests)
#include "trashless_tests.moc"
