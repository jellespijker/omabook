// Another file for a book there is (#220): nothing of the user's gives way to
// it unless that was asked for, and then what gave way is in the trash. An
// empty file takes no file's place, and a conversion made again keeps the one
// before.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "bookops.h"
#include "filepicker.h"
#include "fixtures.h"
#include "library.h"
#include "reader.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

// Why the file wasn't taken; empty when it was.
[[nodiscard]] QString refusedWith(Library &library, qint64 id, const QString &file,
                                  Library::SameFormat same = Library::SameFormat::Refuse) {
    return Fixtures::whyNot(library.addFormat(id, file, same));
}

[[nodiscard]] QByteArray bytesOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A library with one book, a text file, and the trash of this run.
struct Shelf {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    qint64 book = 0;

    Shelf() {
        qputenv("XDG_DATA_HOME", QFile::encodeName(dir.filePath("data")));
        if (QDir().mkpath(dir.filePath("data")) && library.open() && Fixtures::write(file("first.txt"), "The first words.\n"))
            book = library.add(file("first.txt")).id;
    }
    [[nodiscard]] QString file(const QString &name) const { return dir.filePath(name); }
    [[nodiscard]] QByteArray held(const QString &format = u"txt"_s) const { return bytesOf(library.formatPath(book, format)); }
    // What the files in this run's trash hold.
    [[nodiscard]] QList<QByteArray> trash() const {
        QList<QByteArray> gone;
        const QDir files(dir.filePath("data/Trash/files"));
        for (const QString &name : files.entryList(QDir::Files))
            gone << bytesOf(files.filePath(name));
        return gone;
    }
};

} // namespace

class BookFormatsTests : public QObject {
    Q_OBJECT

private slots:
    void aSecondFileOfAFormatIsRefused() {
        Shelf shelf;
        QVERIFY(shelf.book > 0);
        QVERIFY(Fixtures::write(shelf.file("second.txt"), "Another book.\n"));
        const QString why = refusedWith(shelf.library, shelf.book, shelf.file("second.txt"));
        QVERIFY2(why.contains("--replace"), qPrintable(why));
        QCOMPARE(shelf.held(), QByteArray("The first words.\n"));
        // The very file it has is no other one, and another format joins it.
        QVERIFY(shelf.library.addFormat(shelf.book, shelf.file("first.txt")));
        QVERIFY(Fixtures::write(shelf.file("beside.md"), "# Beside\n"));
        QVERIFY(shelf.library.addFormat(shelf.book, shelf.file("beside.md")));
        QCOMPARE(shelf.library.book(shelf.book)->formats.size(), 2);
        QVERIFY(shelf.trash().isEmpty());
    }

    void aFileReplacedWhenAskedIsInTheTrash() {
        Shelf shelf;
        QVERIFY(Fixtures::write(shelf.file("second.txt"), "Another book.\n"));
        QCOMPARE(refusedWith(shelf.library, shelf.book, shelf.file("second.txt"), Library::SameFormat::Trash), QString());
        QCOMPARE(shelf.held(), QByteArray("Another book.\n"));
        QCOMPARE(shelf.trash(), QList<QByteArray>{"The first words.\n"});
    }

    void nothingIsReplacedWithoutATrashToKeepIt() {
        Shelf shelf;
        // A trash that can't be made: a file stands where its folder goes.
        QVERIFY(Fixtures::write(shelf.file("data/Trash"), "in the way"));
        QVERIFY(Fixtures::write(shelf.file("second.txt"), "Another book.\n"));
        const QString why = refusedWith(shelf.library, shelf.book, shelf.file("second.txt"), Library::SameFormat::Trash);
        QVERIFY2(why.contains("trash"), qPrintable(why));
        QCOMPARE(shelf.held(), QByteArray("The first words.\n"));
        QCOMPARE(QFileInfo(shelf.library.formatPath(shelf.book)).dir().entryList(QDir::Files).size(), 1);
    }

    void anEmptyFileNeverJoinsABookThatHasOne() {
        Shelf shelf;
        QVERIFY(Fixtures::write(shelf.file("empty.txt"), ""));
        QVERIFY(Fixtures::write(shelf.file("empty.epub"), ""));
        for (const auto same : {Library::SameFormat::Refuse, Library::SameFormat::Trash, Library::SameFormat::Overwrite}) {
            const QString why = refusedWith(shelf.library, shelf.book, shelf.file("empty.txt"), same);
            QVERIFY2(why.contains("is empty"), qPrintable(why));
        }
        QVERIFY(!shelf.library.addFormat(shelf.book, shelf.file("empty.epub")));
        QCOMPARE(shelf.held(), QByteArray("The first words.\n"));
        QCOMPARE(shelf.library.book(shelf.book)->formats, QStringList{"txt"});
        QVERIFY(shelf.trash().isEmpty());
    }

    // An empty file a book was left with doesn't keep it from being merged.
    void aMergeLeavesAnEmptyFileBehind() {
        Shelf shelf;
        // Such a file still makes a book of its own: that is no other file's place.
        QVERIFY(Fixtures::write(shelf.file("emptied.md"), ""));
        const qint64 other = shelf.library.add(shelf.file("emptied.md")).id;
        QVERIFY(other > 0);
        QString error;
        QVERIFY2(shelf.library.merge(shelf.book, other, &error), qPrintable(error));
        QCOMPARE(shelf.library.book(shelf.book)->formats, QStringList{"txt"});
    }

    // The reader converts a book to open it. An EPUB given to the book while
    // it did stays, and is the one that opens.
    void theReaderKeepsAnEpubThatCameWhileItConverted() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("old.azw3"), Fixtures::mobi()));
        QVERIFY(Fixtures::epub(dir.filePath("mine.epub"), u"Mine"_s));
        QVERIFY(Fixtures::epub(dir.filePath("converted.epub"), u"Converted"_s));
        Fixtures::script(dir.filePath("ebook-convert"), "cp '" + QFile::encodeName(dir.filePath("converted.epub")) + "' \"$2\"\n");
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        NoPicker picker;
        Backend backend(&settings, &picker, nullptr, {}, {}, dir.filePath("plugins"), "-");
        Library &library = *backend.library();
        const qint64 id = library.add(dir.filePath("old.azw3")).id;
        Reader reader(&backend, dir.filePath("ebook-convert"));
        reader.openBook(id);
        // Before the converter is heard of again.
        QVERIFY(library.addFormat(id, dir.filePath("mine.epub")));
        QTRY_VERIFY(reader.isOpen());
        QCOMPARE(reader.error(), QString());
        QCOMPARE(bytesOf(library.formatPath(id, u"epub"_s)), bytesOf(dir.filePath("mine.epub")));
    }

    void aConversionKeepsTheFileItTakesThePlaceOf() {
        Shelf shelf;
        QVERIFY(Fixtures::write(shelf.file("mine.md"), "# Written by hand\n"));
        QVERIFY(shelf.library.addFormat(shelf.book, shelf.file("mine.md")));
        // A stand-in for Calibre's converter, the only one on the path.
        QVERIFY(QDir().mkpath(shelf.file("bin")));
        Fixtures::script(shelf.file("bin/ebook-convert"), "echo '# Converted' > \"$2\"\n");
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", QFile::encodeName(shelf.file("bin")) + ':' + path);
        QString error;
        const QString made = BookOps::convert(shelf.library, shelf.book, u"md"_s, u"txt"_s, &error);
        qputenv("PATH", path);
        QVERIFY2(!made.isEmpty(), qPrintable(error));
        QCOMPARE(bytesOf(made), QByteArray("# Converted\n"));
        QCOMPARE(shelf.trash(), QList<QByteArray>{"# Written by hand\n"});
    }
};

QTEST_MAIN(BookFormatsTests)
#include "bookformats_tests.moc"
