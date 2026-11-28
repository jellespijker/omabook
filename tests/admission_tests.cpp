// What comes into a library from outside (#226): a file that is empty, damaged
// or cut short is no book and is refused with the reason, and one that is
// protected comes in with a note that it can't be read here; whether a person,
// an agent or a plugin adds it.

#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#include "fixtures.h"
#include "formats.h"
#include "importer.h"
#include "library.h"
#include "mcpharness.h"
#include "scriptplugin.h"
#include "scriptplugins.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QByteArray bytesOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// A one page PDF that asks for a password: the standard handler, whose check
// of the empty password fails.
[[nodiscard]] QByteArray lockedPdf() {
    const QList<QByteArray> objects{
        "<</Type/Catalog/Pages 2 0 R>>",
        "<</Type/Pages/Kids[3 0 R]/Count 1>>",
        "<</Type/Page/Parent 2 0 R/MediaBox[0 0 200 200]>>",
        "<</Filter/Standard/V 1/R 2/P -4/O<" + QByteArray(64, '1') + ">/U<" + QByteArray(64, '2') + ">>>",
    };
    QByteArray pdf = "%PDF-1.4\n";
    QByteArray table = "xref\n0 5\n0000000000 65535 f \n";
    for (qsizetype at = 0; at < objects.size(); ++at) {
        table += QByteArray::number(pdf.size()).rightJustified(10, '0') + " 00000 n \n";
        pdf += QByteArray::number(at + 1) + " 0 obj\n" + objects.at(at) + "\nendobj\n";
    }
    const QByteArray start = QByteArray::number(pdf.size());
    const QByteArray id = '<' + QByteArray(32, '3') + '>';
    return pdf + table + "trailer\n<</Size 5/Root 1 0 R/Encrypt 4 0 R/ID[" + id + id + "]>>\nstartxref\n" + start + "\n%%EOF\n";
}

// An EPUB whose chapters are locked, as a shop's DRM leaves it.
[[nodiscard]] bool lockedEpub(const QString &path) {
    auto entries = Fixtures::epubEntries(u"Locked Away"_s);
    entries.emplace_back(u"META-INF/encryption.xml"_s,
                         R"(<encryption><EncryptedData><EncryptionMethod Algorithm="http://www.w3.org/2001/04/xmlenc#aes128-cbc"/>)"
                         R"(</EncryptedData></encryption>)");
    return Fixtures::zip(path, entries);
}

// Bytes of no format at all, the same every time.
[[nodiscard]] QByteArray noise() {
    QByteArray bytes(20000, '\0');
    quint32 state = 12345;
    for (char &byte : bytes) {
        state = state * 1103515245U + 12345U;
        byte = static_cast<char>(state >> 16);
    }
    return bytes;
}

// The file of each kind that is no book, made in `dir`; its path.
[[nodiscard]] QString broken(const QTemporaryDir &dir, const QString &name) {
    const QString path = dir.filePath(name);
    bool made = false;
    if (name.startsWith("empty"))
        made = Fixtures::write(path, {});
    else if (name.startsWith("noise"))
        made = Fixtures::write(path, noise());
    else if (name == "zeros.txt")
        made = Fixtures::write(path, QByteArray(4096, '\0'));
    else if (name == "short.rtf")
        made = Fixtures::write(
            path, QByteArray(R"({\rtf1\ansi{\fonttbl{\f0 Serif;}}\f0 The lantern keeper walked down to the harbour.})").first(70));
    else if (name == "half.docx" && Fixtures::zip(dir.filePath("whole.zip"), {{"word/document.xml", QByteArray("<w/>").repeated(500)}}))
        made = Fixtures::write(path, bytesOf(dir.filePath("whole.zip")).first(QFileInfo(dir.filePath("whole.zip")).size() / 2));
    else if (name == "nopictures.cbz")
        made = Fixtures::zip(path, {{"notes.txt", "No pictures here."}});
    else if (name == "truncated.epub" && Fixtures::epub(dir.filePath("whole.epub")))
        made = Fixtures::write(path, bytesOf(dir.filePath("whole.epub")).chopped(200));
    else if (name == "half.epub" && Fixtures::epub(dir.filePath("whole.epub")))
        made = Fixtures::write(path, bytesOf(dir.filePath("whole.epub")).first(QFileInfo(dir.filePath("whole.epub")).size() / 2));
    else
        made = Fixtures::write(path, QByteArray("These are words, not what the name says they are.\n").repeated(20));
    return made ? path : QString();
}

} // namespace

class AdmissionTests : public QObject {
    Q_OBJECT

private slots:
    void aFileThatIsNoBookIsRefused_data() {
        QTest::addColumn<QString>("name");
        for (const char *name : {"empty.epub", "empty.txt", "empty.mobi", "garbage.epub", "truncated.epub", "half.epub", "fake.pdf",
                                 "fake.cbz", "nopictures.cbz", "garbage.fb2",
                                 // And those of #264.
                                 "noise.mobi", "noise.azw3", "noise.docx", "noise.odt", "noise.djvu", "noise.txt", "noise.rtf", "zeros.txt",
                                 "half.docx", "short.rtf"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void aFileThatIsNoBookIsRefused() {
        QFETCH(const QString, name);
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString file = broken(dir, name);
        QVERIFY(!file.isEmpty());

        const ImportSummary summary = importBooks(library, {file});
        QCOMPARE(summary.added, 0);
        QCOMPARE(summary.lastId, 0);
        QVERIFY(summary.failed());
        QCOMPARE(summary.problems.size(), 1);
        QVERIFY2(summary.problems.first().startsWith(name), qPrintable(summary.problems.first()));
        QCOMPARE(summary.items.size(), 1);
        QCOMPARE(summary.items.first().error, summary.problems.first());
        QVERIFY(library.books().isEmpty());

        // Described by whoever adds it, it is no more of a book.
        BookMeta meta;
        meta.title = u"Described"_s;
        const ImportSummary described = importDescribed(library, file, meta);
        QVERIFY(described.failed());
        QVERIFY(library.books().isEmpty());
    }

    // Pictures none of which can be shown (#264): kept, with a note, and never shown as blank pages.
    void aComicWithoutAReadablePictureComesInWithANote() {
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString comic = dir.filePath("Broken.cbz");
        QVERIFY(Fixtures::zip(comic, {{"1.jpg", "not an image"}, {"2.jpg", QByteArray()}}));

        const ImportSummary summary = importBooks(library, {comic});
        QCOMPARE(summary.added, 1);
        QCOMPARE(summary.unreadable.size(), 1);
        QVERIFY2(summary.unreadable.first().text().contains("pictures"), qPrintable(summary.unreadable.first().text()));
        QString why;
        QVERIFY(!Formats::open(library.formatPath(summary.lastId, "cbz"), &why));
        QVERIFY2(why.contains("pictures"), qPrintable(why));
    }

    // What is what its name says, and text in an older encoding, comes in as before (#264).
    void whatIsWhatItsNameSaysComesIn_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("content");
        QByteArray topaz = "TPZ0";
        topaz.resize(100, '\1');
        QTest::newRow("a Kindle book") << "book.azw3" << Fixtures::mobi();
        QTest::newRow("a Topaz book") << "book.azw" << topaz;
        QTest::newRow("rich text") << "book.rtf" << QByteArray(R"({\rtf1\ansi A \{ brace.{\bin3 {{{}}\par})") + "\n\0"_ba;
        QTest::newRow("a DjVu") << "book.djvu" << QByteArray("AT&TFORM\0\0\0\0DJVU", 16);
        QTest::newRow("a Word document") << "book.docx" << QByteArray("PK\x05\x06", 4).prepend("PK\x03\x04") + QByteArray(18, '\0');
        QTest::newRow("Western text") << "book.txt" << QByteArray("Caf\xe9 on the quay,\twith a form feed\f\r\nand an end mark\x1a");
        QTest::newRow("UTF-16 text") << "wide.txt" << QByteArray("\xff\xfeH\0i\0\n\0", 8);
    }

    void whatIsWhatItsNameSaysComesIn() {
        QFETCH(const QString, name);
        QFETCH(const QByteArray, content);
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::write(dir.filePath(name), content));
        const ImportSummary summary = importBooks(library, {dir.filePath(name)});
        QVERIFY2(summary.added == 1 && !summary.failed(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.unreadable.size(), 0);
    }

    // A folder that is one book comes in by whatever path names it (#267).
    void aFolderComesInByARelativePath_data() {
        QTest::addColumn<QString>("path");
        for (const char *path : {"mybook", "./mybook", "mybook/", "./mybook/", "shelf/../mybook"})
            QTest::newRow(path) << QString::fromLatin1(path);
    }

    void aFolderComesInByARelativePath() {
        QFETCH(const QString, path);
        const QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("mybook") && QDir(dir.path()).mkpath("shelf"));
        QVERIFY(Fixtures::write(dir.filePath("mybook/01.md"), "# One\n\nalpha\n")
                && Fixtures::write(dir.filePath("mybook/02.md"), "# Two\n\nbeta\n"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString before = QDir::currentPath();
        QVERIFY(QDir::setCurrent(dir.path()));
        const auto back = qScopeGuard([&before] { QDir::setCurrent(before); });

        const ImportSummary summary = importBooks(library, {path});
        QVERIFY2(summary.added == 1 && !summary.failed(), qPrintable(summary.problems.join('\n')));
        QString why;
        const auto document = Formats::open(library.formatPath(summary.lastId, "mdz"), &why);
        QVERIFY2(document && document->count() == 2, qPrintable(why));
        // Reading the folder where it lies, as `omabook toc` does.
        const auto lying = Formats::open(path, &why);
        QVERIFY2(lying && lying->count() == 2, qPrintable(why));
    }

    void aProtectedBookComesInWithANote() {
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QVERIFY(lockedEpub(dir.filePath("shop.epub")));
        QVERIFY(Fixtures::write(dir.filePath("secret.pdf"), lockedPdf()));

        const ImportSummary summary = importBooks(library, {dir.filePath("shop.epub"), dir.filePath("secret.pdf")});
        QCOMPARE(summary.added, 2);
        QVERIFY(!summary.failed());
        QCOMPARE(summary.reasons().size(), 2);
        QVERIFY2(summary.reasons().at(0).startsWith("shop.epub was added") && summary.reasons().at(0).contains("DRM"),
                 qPrintable(summary.reasons().at(0)));
        QVERIFY2(summary.reasons().at(1).startsWith("secret.pdf was added") && summary.reasons().at(1).contains("password"),
                 qPrintable(summary.reasons().at(1)));
        // Said when it comes in, not every time it is found here again.
        const ImportSummary again = importBooks(library, {dir.filePath("secret.pdf")});
        QCOMPARE(again.duplicates, 1);
        QVERIFY(again.reasons().isEmpty());
    }

    void aSoundFileOfACalibreBookComesInWithoutTheDamagedOne() {
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString calibre = dir.filePath("Shelf");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        const QString epub = calibre + "/Writer, Bo/Shelved Wrong (1)/Shelved Wrong - Writer, Bo.epub";
        QVERIFY(Fixtures::write(epub, "no zip at all"));

        const ImportSummary summary = importBooks(library, {calibre});
        QCOMPARE(summary.added, 1);
        QCOMPARE(library.books().size(), 1);
        QCOMPARE(library.books().first().formats, QStringList{"txt"});
        QVERIFY2(summary.reasons().join('\n').contains("Shelved Wrong - Writer, Bo.epub was not added"),
                 qPrintable(summary.reasons().join('\n')));
    }

    // What a library took before, or what a sync carried here, stays its own.
    void aFileTheLibraryHoldsIsStillFoundHere() {
        const QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString file = broken(dir, u"garbage.epub"_s);
        const qint64 held = library.add(file).id;
        QVERIFY(held > 0);

        const ImportSummary summary = importBooks(library, {file});
        QCOMPARE(summary.duplicates, 1);
        QCOMPARE(summary.lastId, held);
        QVERIFY(!summary.failed());
    }

    void commandsRefuseWithTheReason() {
        const McpHarness::Machine machine;
        const QStringList at{"--library", machine.library};
        const QString empty = broken(machine.dir, u"empty.epub"_s), fake = broken(machine.dir, u"fake.pdf"_s);
        const QString text = machine.dir.filePath("story.txt");
        QVERIFY(Fixtures::write(text, "It was a dark night.\n"));

        const QString picture = machine.dir.filePath("cover.png");
        QImage blank(8, 8, QImage::Format_RGB32);
        blank.fill(Qt::white);
        QVERIFY(blank.save(picture));

        for (const QStringList &arguments :
             {QStringList{"add", empty}, QStringList{"add", fake}, QStringList{"add", fake, "--cover", picture}}) {
            const auto ran = machine.omabook(arguments + at);
            QCOMPARE(ran.code, 1);
            QVERIFY2(ran.errors.contains(".epub is empty") || ran.errors.contains("fake.pdf was not added"), qPrintable(ran.errors));
            QVERIFY(ran.output.trimmed().isEmpty());
        }
        QCOMPARE(machine.omabook(QStringList{"add", text} + at).code, 0);
        const auto format = machine.omabook(QStringList{"add", fake, "--to", "1"} + at);
        QCOMPARE(format.code, 1);
        QVERIFY2(format.errors.contains("fake.pdf was not added"), qPrintable(format.errors));
        QVERIFY(Fixtures::write(machine.dir.filePath("secret.pdf"), lockedPdf()));
        const auto locked = machine.omabook(QStringList{"add", machine.dir.filePath("secret.pdf"), "--to", "1"} + at);
        QCOMPARE(locked.code, 0);
        QVERIFY2(locked.errors.contains("can't be read here"), qPrintable(locked.errors));
        const QJsonArray books = QJsonDocument::fromJson(machine.omabook(QStringList{"list", "--json"} + at).output.toUtf8()).array();
        QCOMPARE(books.size(), 1);
    }

    void aPluginsEmptyFileIsRefused() {
        const QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const QString folder = dir.filePath("plugins/blank");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(Fixtures::write(folder + "/plugin.json",
                                R"({"name": "blank", "version": "1.0", "script": "main.js", "permissions": ["files"]})"));
        QVERIFY(Fixtures::write(folder + "/main.js", R"js(
            function make() {
                omabook.files.write("data/blank.txt", "");
                return omabook.library.create({title: "Blank"}, "data/blank.txt");
            }
        )js"));
        ScriptServices services;
        services.settings = &settings;
        services.library = [&library] { return &library; };
        ScriptPlugins plugins(services, dir.filePath("plugins"), nullptr, false);
        ScriptPlugin *plugin = plugins.plugin(u"blank"_s);
        QVERIFY(plugin);
        QString problem;
        plugin->callNow(u"make"_s, {}, &problem);
        QVERIFY2(problem.contains("blank.txt is empty"), qPrintable(problem));
        QVERIFY(library.books().isEmpty());
    }
};

QTEST_MAIN(AdmissionTests)
#include "admission_tests.moc"
