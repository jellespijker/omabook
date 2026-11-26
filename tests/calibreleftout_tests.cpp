#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include "backend.h"
#include "filepicker.h"
#include "fixtures.h"
#include "settings.h"

// Nothing of a Calibre library is passed over in silence (#224): whatever
// doesn't come across is named, with why, by the command line, the log and the
// window. Every library here is made up, in a temporary folder.

namespace {

// A Calibre library, and an Omabook of its own to bring it into.
struct World {
    QTemporaryDir dir;
    QString shelf = dir.filePath("Calibre Library");
    QString library = dir.filePath("Library");
    bool ok = Fixtures::calibreShelf(shelf);

    [[nodiscard]] QString file(const QString &inside) const { return shelf + "/" + inside; }
    [[nodiscard]] bool change(const QStringList &statements) const { return Fixtures::calibreChange(shelf, statements); }
    // The shelf's book in a format Omabook doesn't take, gone: what is left all comes across.
    [[nodiscard]] bool tidy() const { return change({"DELETE FROM books WHERE id = 2", "DELETE FROM data WHERE book = 2"}); }
    // A book as `calibredb add --empty` makes one: no file at all.
    [[nodiscard]] bool emptyBook() const {
        return change({"INSERT INTO books VALUES (7, 'Empty Paper', 1.0, '0101-01-01 00:00:00+00:00', 'N. Obody/Empty Paper (7)', 0, "
                       "'11111111-aaaa-4bbb-8ccc-000000000007')",
                       "INSERT INTO authors VALUES (7, 'N. Obody')", "INSERT INTO books_authors_link VALUES (7, 7, 7)"});
    }

    struct Ran {
        int code = -1;
        QString output, errors;
        [[nodiscard]] QJsonObject json() const { return QJsonDocument::fromJson(output.toUtf8()).object(); }
        [[nodiscard]] QString leftOut() const {
            QStringList lines;
            for (const QJsonValue &line : json().value("left_out").toArray())
                lines << line.toString();
            return lines.join('\n');
        }
        [[nodiscard]] QString problems() const {
            QStringList lines;
            for (const QJsonValue &line : json().value("problems").toArray())
                lines << line.toString();
            return lines.join('\n');
        }
    };
    [[nodiscard]] Ran omabook(const QStringList &arguments) const {
        QProcess process;
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", dir.filePath("home"));
        for (const char *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME"})
            environment.insert(name, dir.filePath(QString("home/") + name));
        process.setProcessEnvironment(environment);
        process.start(OMABOOK_BINARY, QStringList(arguments) << "--library" << library);
        if (!process.waitForFinished(60000))
            return {};
        return {.code = process.exitCode(),
                .output = QString::fromUtf8(process.readAllStandardOutput()),
                .errors = QString::fromUtf8(process.readAllStandardError())};
    }
    [[nodiscard]] Ran import() const { return omabook({"calibre", "import", shelf, "--json"}); }
};

// Takes every right to a file or folder away for as long as it lives.
class Shut {
public:
    explicit Shut(const QString &path) : m_path(path), m_before(QFile::permissions(path)) { QFile::setPermissions(path, {}); }
    ~Shut() { QFile::setPermissions(m_path, m_before); }
    Shut(const Shut &) = delete;
    Shut &operator=(const Shut &) = delete;
    // Permissions don't hold the superuser back.
    [[nodiscard]] bool holds() const { return !QFileInfo(m_path).isReadable(); }

private:
    QString m_path;
    QFile::Permissions m_before;
};

class SilentPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

} // namespace

class CalibreLeftOutTests : public QObject {
    Q_OBJECT

private slots:
    void booksWithoutAFileToTakeAreNamedAndCounted() {
        const World world;
        const QString gone = world.file("Reader, Ann/Long Done (5)/Long Done - Reader, Ann.txt");
        QVERIFY(world.ok && world.emptyBook() && QFile::remove(gone));

        const World::Ran ran = world.import();
        QCOMPARE(ran.code, 1);
        // Seven books: four came, three are counted and named with why.
        QCOMPARE(ran.json().value("added").toInt(), 4);
        QCOMPARE(ran.json().value("skipped").toInt(), 3);
        const QString left = ran.leftOut();
        QVERIFY2(left.contains("Empty Paper - N. Obody was not imported: Calibre has no file for it."), qPrintable(left));
        QVERIFY2(left.contains("Long Done was not imported: none of its files can be taken."), qPrintable(left));
        QVERIFY2(left.contains("Long Done: there is no file at " + gone), qPrintable(left));
        QVERIFY2(left.contains("Old Format was not imported: none of its files can be taken."), qPrintable(left));
        QVERIFY2(left.contains("Old Format - Nobody.original_epub is in a format Omabook doesn't take."), qPrintable(left));
        QVERIFY2(ran.errors.contains("Empty Paper - N. Obody was not imported"), qPrintable(ran.errors));

        // The log names them too.
        const World::Ran log = world.omabook({"logs"});
        QVERIFY2(log.output.contains("Empty Paper - N. Obody was not imported"), qPrintable(log.output));
        QVERIFY2(log.output.contains("there is no file at " + gone), qPrintable(log.output));

        // And so does adding the library as a folder, in words as in JSON.
        const World::Ran added = world.omabook({"add", world.shelf});
        QCOMPARE(added.code, 1);
        QVERIFY2(added.errors.contains("Empty Paper - N. Obody was not imported"), qPrintable(added.errors));
        const World::Ran words = world.omabook({"calibre", "import", world.shelf});
        QVERIFY2(words.output.contains("3 not imported"), qPrintable(words.output));
    }

    void aLibraryThatAllComesAcrossLeavesNothingOut() {
        const World world;
        QVERIFY(world.ok && world.tidy());
        for (const int added : {5, 0}) {
            const World::Ran ran = world.import();
            QCOMPARE(ran.code, 0);
            QCOMPARE(ran.json().value("added").toInt(), added);
            QCOMPARE(ran.json().value("skipped").toInt(-1), 0);
            QVERIFY(ran.json().value("left_out").isArray());
            QVERIFY2(ran.leftOut().isEmpty(), qPrintable(ran.leftOut()));
        }
    }

    void whatABookThatComesAcrossLacksIsNamed() {
        const World world;
        const QString folder = "Writer, Bo/Shelved Wrong (1)/";
        const QString cover = world.file(folder + "cover.jpg");
        QVERIFY(world.ok && world.tidy() && QFile::remove(cover));
        QVERIFY(world.change({"INSERT INTO data VALUES (40, 1, 'PDF', 'Lost')", "INSERT INTO data VALUES (41, 1, 'ZIP', 'Packed')"}));
        QVERIFY(Fixtures::write(world.file(folder + "Packed.zip"), "x"));

        const World::Ran ran = world.import();
        // Every book came, so nothing failed; what of one didn't is still said.
        QCOMPARE(ran.code, 0);
        QCOMPARE(ran.json().value("added").toInt(), 5);
        QCOMPARE(ran.json().value("skipped").toInt(-1), 0);
        const QString left = ran.leftOut();
        QVERIFY2(left.contains("there is no file at " + world.file(folder + "Lost.pdf")), qPrintable(left));
        QVERIFY2(left.contains("Packed.zip is in a format Omabook doesn't take."), qPrintable(left));
        QVERIFY2(left.contains("its cover can't be read from " + cover), qPrintable(left));
    }

    void aCoverThatIsNoPictureIsNamed() {
        const World world;
        const QString cover = world.file("Writer, Bo/Shelved Wrong (1)/cover.jpg");
        QVERIFY(world.ok && world.tidy() && Fixtures::write(cover, "no picture"));
        const World::Ran ran = world.import();
        QVERIFY2(ran.leftOut().contains("its cover can't be read from " + cover), qPrintable(ran.leftOut()));
    }

    void anUnreadableFileIsNamedAndTheBooksOtherFilesComeAcross() {
        const World world;
        QVERIFY(world.ok && world.tidy());
        // The first of one book's two formats, the only file of another, and a whole folder.
        const QString first = world.file("Writer, Bo/Shelved Wrong (1)/Shelved Wrong - Writer, Bo.epub");
        const QString only = world.file("Reader, Ann/Half Way (3)/Half Way - Reader, Ann.txt");
        const QString folder = world.file("Reader, Ann/Some Day (4)");
        const Shut shutFirst(first), shutOnly(only), shutFolder(folder);
        if (!shutFirst.holds())
            QSKIP("Permissions don't hold this user back.");

        const World::Ran ran = world.import();
        QCOMPARE(ran.code, 1);
        QCOMPARE(ran.json().value("added").toInt(), 3);
        QCOMPARE(ran.json().value("skipped").toInt(), 2);
        const QString left = ran.leftOut();
        QVERIFY2(left.contains("the file " + first + " can't be read."), qPrintable(left));
        QVERIFY2(left.contains("Half Way was not imported"), qPrintable(left));
        QVERIFY2(left.contains("the file " + only + " can't be read."), qPrintable(left));
        QVERIFY2(left.contains("Some Day was not imported"), qPrintable(left));
        QVERIFY2(left.contains("the file " + folder + "/Some Day - Reader, Ann.txt can't be read."), qPrintable(left));
    }

    void twoEntriesWithAFileInCommonAreSaidToBeOne() {
        const World world;
        const QString folder = "Writer, Bo/Second Entry (8)";
        QVERIFY(world.ok && world.tidy() && QDir().mkpath(world.file(folder)));
        // Its text is the first book's, its EPUB another.
        QVERIFY(Fixtures::epub(world.file(folder + "/Second.epub"), "Another"));
        QVERIFY(QFile::copy(world.file("Writer, Bo/Shelved Wrong (1)/Shelved Wrong - Writer, Bo.txt"), world.file(folder + "/Second.txt")));
        QVERIFY(world.change({"INSERT INTO books VALUES (8, 'Second Entry', 1.0, '0101-01-01', '" + folder + "', 0, 'uuid-8')",
                              "INSERT INTO data VALUES (80, 8, 'EPUB', 'Second'), (81, 8, 'TXT', 'Second')"}));

        const World::Ran ran = world.import();
        QCOMPARE(ran.json().value("already_here").toInt(), 1);
        const QString left = ran.leftOut();
        QVERIFY2(left.contains("Second Entry has a file in common with Calibre's Title - Bo Writer"), qPrintable(left));
        QVERIFY2(left.contains("Second.epub was not copied"), qPrintable(left));
    }

    void aBookTheLibraryCantTakeIsNamedAndCounted() {
        const World world;
        QVERIFY(world.ok && world.tidy());
        // A file where the first book's author folder would go.
        QVERIFY(QDir().mkpath(world.library) && Fixtures::write(world.library + "/Bo Writer", "in the way"));
        const World::Ran ran = world.import();
        QCOMPARE(ran.code, 1);
        QCOMPARE(ran.json().value("added").toInt(), 4);
        QCOMPARE(ran.json().value("skipped").toInt(), 1);
        QVERIFY2(ran.problems().contains("Calibre's Title - Bo Writer was not imported: "), qPrintable(ran.problems()));
    }

    void aLibraryWithoutItsListOfFilesIsSaidToBeUnreadable() {
        const World world;
        QVERIFY(world.ok && world.change({"DROP TABLE data"}));
        const World::Ran ran = world.import();
        QCOMPARE(ran.code, 1);
        QVERIFY2(ran.problems().contains("files of the Calibre library can't be read"), qPrintable(ran.problems()));
    }

    void theWindowSaysHowManyAndNamesThem() {
        const World world;
        const QString gone = world.file("Reader, Ann/Long Done (5)/Long Done - Reader, Ann.txt");
        QVERIFY(world.ok && world.emptyBook() && QFile::remove(gone));
        Settings settings(world.dir.filePath("omabook.json"));
        settings.setLibraryPath(world.library);
        SilentPicker picker;
        Backend backend(
            &settings, &picker, nullptr, {}, [](QObject *) { return QList<MetadataSource *>(); }, world.dir.filePath("plugins"), "-");
        backend.importCalibreLibrary(world.shelf);
        QTRY_VERIFY_WITH_TIMEOUT(!backend.busy(), 20000);
        const QString said = backend.message();
        QVERIFY2(said.contains("4 books added"), qPrintable(said));
        QVERIFY2(said.contains("3 books of the Calibre library were not imported"), qPrintable(said));
        // The books left behind come first, by name.
        for (const char *name : {"Empty Paper - N. Obody", "Long Done", "Old Format"})
            QVERIFY2(said.contains(QString(name) + " was not imported"), qPrintable(said));
        QVERIFY2(said.contains("2 more are named in the log"), qPrintable(said));
    }
};

QTEST_MAIN(CalibreLeftOutTests)
#include "calibreleftout_tests.moc"
