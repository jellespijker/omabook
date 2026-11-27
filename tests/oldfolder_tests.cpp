// A library started in a folder that holds an older library's books writes over
// none of their files, and an empty database where there was a library is said
// to be that, instead of becoming a new one (#281).

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QtTest>

#include "library.h"
#include "mcpharness.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

// The files below `folder`, but not the database's own, each with its hash.
[[nodiscard]] QMap<QString, QString> filesIn(const QString &folder) {
    QMap<QString, QString> files;
    QDirIterator walk(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QString path = walk.next();
        if (!walk.fileName().startsWith("omabook.db"_L1))
            files.insert(path, Library::hashFile(path));
    }
    return files;
}

// Whether every file of `before` is where it was, holding what it held.
[[nodiscard]] bool untouched(const QMap<QString, QString> &before) {
    return std::ranges::all_of(before.asKeyValueRange(), [](const auto &file) { return Library::hashFile(file.first) == file.second; });
}

// An edition of one title: the same title and format, other bytes.
[[nodiscard]] QString edition(const McpHarness::Machine &machine, const QString &which) {
    const QString folder = machine.dir.filePath(which);
    const QString file = folder + "/The Salt Road.txt";
    return QDir().mkpath(folder) && Fixtures::write(file, which.toUtf8() + " edition.\n") ? file : QString();
}

[[nodiscard]] bool removeDatabase(const QString &library) {
    for (const QString &suffix : {u"-wal"_s, u"-shm"_s})
        QFile::remove(Library::databaseIn(library) + suffix);
    return QFile::remove(Library::databaseIn(library));
}

} // namespace

class OldFolderTests : public QObject {
    Q_OBJECT

private slots:
    void init() { QFile::remove(Settings::defaultFile()); }

    void anEmptyDatabaseBesideBooksIsNotANewLibrary() {
        const McpHarness::Machine machine;
        const QString library = machine.library;
        QCOMPARE(machine.omabook({"add", edition(machine, "first"), "--library", library}).code, 0);
        QVERIFY(removeDatabase(library));
        QVERIFY(Fixtures::write(Library::databaseIn(library), {}));
        const auto before = filesIn(library);
        QVERIFY(!before.isEmpty());

        for (const QStringList &arguments : {QStringList{"list", "--library", library}, QStringList{"libraries", "--use", library},
                                             QStringList{"libraries", "--use", library, "--create"},
                                             QStringList{"add", edition(machine, "second"), "--library", library}}) {
            const auto ran = machine.omabook(arguments);
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains("is empty") && ran.errors.contains("snapshot restore"), qPrintable(ran.errors));
        }
        QCOMPARE(QFileInfo(Library::databaseIn(library)).size(), 0);
        QCOMPARE(filesIn(library), before);

        // Moved aside, as it says: a new library, which writes over nothing.
        QVERIFY(QFile::remove(Library::databaseIn(library)));
        QCOMPARE(machine.omabook({"libraries", "--use", library, "--create"}).code, 0);
        QCOMPARE(machine.omabook({"add", edition(machine, "second"), "--library", library}).code, 0);
        QVERIFY(untouched(before));
    }

    // SQLite throws a log away beside an empty database, and the changes in it.
    void anEmptyDatabaseBesideItsLogKeepsTheLog() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(Fixtures::write(Library::databaseIn(root), {}));
        QVERIFY(Fixtures::write(Library::databaseIn(root) + "-wal", "changes"));
        Library library(root);
        QString why;
        QVERIFY(!library.open(&why));
        QVERIFY2(why.contains("is empty"), qPrintable(why));
        QCOMPARE(QFileInfo(Library::databaseIn(root) + "-wal").size(), 7);
    }

    // With nothing there to lose, as a first start cut short leaves it.
    void anEmptyDatabaseAloneBecomesALibrary() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        QVERIFY(QDir().mkpath(root));
        QVERIFY(Fixtures::write(Library::databaseIn(root), {}));
        Library library(root);
        QString why;
        QVERIFY2(library.open(&why), qPrintable(why));
    }

    void aNewLibraryWritesOverNoBook() {
        const McpHarness::Machine machine;
        const QString library = machine.library;
        QCOMPARE(machine.omabook({"add", edition(machine, "first"), "--library", library}).code, 0);
        QVERIFY(removeDatabase(library));
        const auto before = filesIn(library);
        QCOMPARE(before.size(), 1);
        QCOMPARE(machine.omabook({"libraries", "--use", library, "--create"}).code, 0);
        QCOMPARE(machine.omabook({"add", edition(machine, "second")}).code, 0);
        QVERIFY(untouched(before));
        QCOMPARE(filesIn(library).size(), 2);
    }

    // The natural recovery: every folder added again, "(10)" read before "(2)".
    void addingTheOldFolderAgainKeepsEveryEdition() {
        const McpHarness::Machine machine;
        const QString library = machine.library;
        {
            Library old(library);
            QVERIFY(old.open());
            QCOMPARE(old.add(edition(machine, "first")).id, 1);
            QCOMPARE(old.add(edition(machine, "second")).id, 2);
            for (int filler = 3; filler < 10; ++filler)
                QCOMPARE(old.addEntry({.title = u"Filler %1"_s.arg(filler)}, true).id, filler);
            QCOMPARE(old.add(edition(machine, "tenth")).id, 10);
        }
        const auto before = filesIn(library);
        QCOMPARE(before.size(), 3);
        QVERIFY(removeDatabase(library));
        QCOMPARE(machine.omabook({"libraries", "--use", library, "--create"}).code, 0);
        QCOMPARE(machine.omabook({"add", library}).code, 0);
        QVERIFY(untouched(before));
        Library recovered(library);
        QVERIFY(recovered.open());
        QStringList held;
        for (const Book &book : recovered.books())
            for (const FormatFile &file : recovered.formatFiles(book.id)) {
                QCOMPARE(Library::hashFile(file.path), file.hash);
                held << file.hash;
            }
        QStringList expected = before.values();
        held.sort();
        expected.sort();
        QCOMPARE(held, expected);
    }

    void aFailedAddAndAPaperBookLeaveAnOldFolderAlone() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library");
        const QString old = root + "/Unknown/On paper (1)";
        QVERIFY(QDir().mkpath(old));
        QVERIFY(Fixtures::write(old + "/cover.jpg", "old cover"));
        Library library(root);
        QVERIFY(library.open());
        QImage cover(10, 20, QImage::Format_RGB32);
        cover.fill(Qt::red);
        const qint64 id = library.addEntry({.title = "On paper", .cover = cover}, true).id;
        QCOMPARE(id, 1);
        QCOMPARE(library.book(id)->path, u"Unknown/On paper (1) 2"_s);
        QVERIFY(!library.coverPath(id).isEmpty());
        QFile kept(old + "/cover.jpg");
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("old cover"));
    }

    void aFileTheLibraryDoesNotKnowIsNotWrittenOver() {
        QTemporaryDir dir;
        const QString root = dir.filePath("Library"), given = dir.filePath("book.txt");
        QVERIFY(Fixtures::write(given, "The new one.\n"));
        Library library(root);
        QVERIFY(library.open());
        const qint64 id = library.addEntry({.title = "On paper"}, true).id;
        const QString folder = root + u'/' + library.book(id)->path;
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(Fixtures::write(folder + "/On paper.txt", "Someone's own.\n"));
        const auto added = library.addFormat(id, given, Library::SameFormat::Trash);
        QVERIFY(!added);
        QCOMPARE(added.error().code(), Failure::Code::BookFileInTheWay);
        QFile kept(folder + "/On paper.txt");
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("Someone's own.\n"));
        QVERIFY(library.formatFiles(id).isEmpty());
    }
};

QTEST_MAIN(OldFolderTests)
#include "oldfolder_tests.moc"
