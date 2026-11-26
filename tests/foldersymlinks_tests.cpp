// Adding a folder with symbolic links in it (#225): links to folders and to
// files are followed, and each real folder is looked into once and each book
// added once, even where links lead back up. Through the real binary, so that
// a walk that never ends fails when its time is up.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "library.h"
#include "mcpharness.h"

namespace {

using McpHarness::Machine;

// A folder of the test's own to add from, with links in it.
struct Shelf {
    Machine machine;

    [[nodiscard]] QString path(const QString &name) const { return machine.dir.filePath(name); }
    [[nodiscard]] bool folder(const QString &name) const { return QDir().mkpath(path(name)); }
    [[nodiscard]] bool book(const QString &name, const QString &title) const { return Fixtures::epub(path(name), title); }
    // `target` is what the link says: relative to the folder the link is in.
    [[nodiscard]] bool link(const QString &target, const QString &name) const { return QFile::link(target, path(name)); }

    [[nodiscard]] Machine::Ran add(const QStringList &names) const {
        QStringList arguments{"add"};
        for (const QString &name : names)
            arguments << path(name);
        return machine.omabook(arguments + QStringList{"--library", machine.library, "--json"});
    }

    [[nodiscard]] QStringList titles() const {
        Library library(machine.library);
        QStringList titles;
        if (library.open())
            for (const Book &book : library.books())
                titles << book.meta.title;
        titles.sort();
        return titles;
    }
};

// How often `add` said it brought in, or found, a book.
[[nodiscard]] qsizetype reported(const Machine::Ran &ran) { return QJsonDocument::fromJson(ran.output.toUtf8()).array().size(); }

} // namespace

class FolderSymlinksTests : public QObject {
    Q_OBJECT

private slots:
    void aLinkBackUpAddsTheBookOnce() {
        const Shelf shelf;
        QVERIFY(shelf.folder("lib/sub"));
        QVERIFY(shelf.book("lib/sub/book.epub", "The Salt Lighthouse"));
        QVERIFY(shelf.link("..", "lib/sub/up"));
        const Machine::Ran ran = shelf.add({"lib"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(reported(ran), 1);
        QCOMPARE(shelf.titles(), QStringList{"The Salt Lighthouse"});
    }

    void twoLinksBackUpEnd() {
        const Shelf shelf;
        QVERIFY(shelf.folder("two/a") && shelf.folder("two/b"));
        QVERIFY(shelf.book("two/book.epub", "The Salt Lighthouse"));
        QVERIFY(shelf.link("..", "two/a/up") && shelf.link("..", "two/b/up"));
        const Machine::Ran ran = shelf.add({"two"});
        QVERIFY2(ran.code == 0, qPrintable("it did not end, or failed: " + ran.errors));
        QCOMPARE(reported(ran), 1);
        QCOMPARE(shelf.titles(), QStringList{"The Salt Lighthouse"});
    }

    void linksToFoldersAndFilesElsewhereAreFollowedOnce() {
        const Shelf shelf;
        QVERIFY(shelf.folder("lib/own") && shelf.folder("elsewhere/series") && shelf.folder("elsewhere/notes"));
        QVERIFY(shelf.book("lib/own/own.epub", "Own"));
        QVERIFY(shelf.book("elsewhere/series/linked.epub", "In A Linked Folder"));
        QVERIFY(shelf.book("elsewhere/single.epub", "Linked File"));
        // A folder that is one book.
        QVERIFY(Fixtures::write(shelf.path("elsewhere/notes/one.md"), "# Notes\n\nA chapter.\n"));
        // Each reached by two names, and what leads nowhere.
        QVERIFY(shelf.link("../elsewhere/series", "lib/series") && shelf.link("../elsewhere/series", "lib/series again"));
        QVERIFY(shelf.link("../elsewhere/single.epub", "lib/single.epub")
                && shelf.link("../../elsewhere/single.epub", "lib/own/same.epub"));
        QVERIFY(shelf.link("../elsewhere/notes", "lib/notes") && shelf.link("../elsewhere/notes", "lib/notes again"));
        QVERIFY(shelf.link("own.epub", "lib/own/second name.epub"));
        QVERIFY(shelf.link("itself", "lib/itself") && shelf.link("nothing", "lib/nowhere.epub"));
        // A folder named twice, once inside the other.
        const Machine::Ran ran = shelf.add({"lib", "lib/own", "lib"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(reported(ran), 4);
        const QStringList titles = shelf.titles();
        QCOMPARE(titles.size(), 4);
        for (const char *title : {"Own", "In A Linked Folder", "Linked File"})
            QVERIFY2(titles.contains(QLatin1StringView(title)), qPrintable(titles.join(", ")));
    }

    void aLinkNamedAloneIsFollowed() {
        const Shelf shelf;
        QVERIFY(shelf.folder("real"));
        QVERIFY(shelf.book("real/book.epub", "Behind A Link"));
        QVERIFY(shelf.link("real", "alias"));
        const Machine::Ran ran = shelf.add({"alias"});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        QCOMPARE(shelf.titles(), QStringList{"Behind A Link"});
    }
};

QTEST_MAIN(FolderSymlinksTests)
#include "foldersymlinks_tests.moc"
