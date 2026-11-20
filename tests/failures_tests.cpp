// Failures from the layers below the front ends (failure.h, ADR 0005): each
// module says why by a code, and the sentence is worded where it is shown, in
// the desktop's language in the window and in English on the command line.

#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QtTest>

#include "desk.h"
#include "failure.h"
#include "formats.h"
#include "importer.h"
#include "library.h"
#include "metadatasource.h"

namespace {

// The languages the window speaks besides English.
const QStringList languages{"nl", "de", "fr", "es-MX"};

// A translator for one language, installed while it lives, as the window's is.
struct Speaking {
    QTemporaryDir dir;
    std::optional<Translator> translator;

    explicit Speaking(const QString &language) {
        Fixtures::write(dir.filePath("language"), language.toUtf8() + "\n");
        translator.emplace(dir.filePath("language"));
        QCoreApplication::installTranslator(&*translator);
    }
    ~Speaking() { QCoreApplication::removeTranslator(&*translator); }
    Speaking(const Speaking &) = delete;
    Speaking &operator=(const Speaking &) = delete;
};

[[nodiscard]] QJsonObject catalog(const QString &language) {
    QFile file(QFINDTESTDATA("../src/i18n/" + language + ".json"));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// The sentences failure.cpp words failures in, as bin/i18n reads them.
[[nodiscard]] QStringList sentences() {
    QFile file(QFINDTESTDATA("../src/failure.cpp"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    static const QRegularExpression marked(R"(\btr\("((?:[^"\\]|\\.)*)\")");
    QStringList found;
    // Comments are skipped, as bin/i18n skips them.
    static const QRegularExpression comment("//[^\n]*");
    const QString code = QString::fromUtf8(file.readAll()).remove(comment);
    for (const QRegularExpressionMatch &match : marked.globalMatch(code))
        found << match.captured(1).replace("\\\"", "\"");
    return found;
}

// A catalog at an address of this computer where nothing listens.
class ClosedCatalog : public WebMetadataSource {
public:
    using WebMetadataSource::WebMetadataSource;
    QString name() const override { return "Closed Catalog"; }
    QStringList fields() const override { return {"title"}; }

protected:
    QUrl searchUrl(const BookMeta &) const override { return QUrl("http://127.0.0.1:1/search"); }
    QList<MetaCandidate> parse(const QByteArray &) const override { return {}; }
};

// What a source says when it is asked for a book's details and can't answer.
[[nodiscard]] Failure lookUpFails(MetadataSource &source) {
    QSignalSpy failed(&source, &MetadataSource::failed);
    BookMeta book;
    book.title = "Moss";
    source.search(book);
    return failed.wait(10000) || !failed.isEmpty() ? failed.first().first().value<Failure>() : Failure();
}

[[nodiscard]] Failure openingFails(const QString &path) {
    const auto document = Formats::opened(path);
    return document ? Failure() : document.error();
}

} // namespace

class FailuresTests : public QObject {
    Q_OBJECT

private slots:
    void aFailureIsWordedInEnglishWithoutATranslator() {
        // The command line and the MCP server install no translator.
        QCOMPARE(Failure(Failure::Code::NoSuchFile, {"/books/Moss.epub"}).text(), QString("There is no such file: /books/Moss.epub"));
        // An argument is put in as it is, even one that looks like a placeholder.
        QCOMPARE(Failure(Failure::Code::NoChaptersFound, {"Lines %2", "a%1.lines"}).text(),
                 QString("Lines %2 found no chapters in a%1.lines."));
        QCOMPARE(Failure::said("the catalog is closed").text(), QString("the catalog is closed"));
    }

    void everySentenceIsInEveryCatalog() {
        const QStringList wanted = sentences();
        QVERIFY(wanted.size() > 10);
        QStringList missing;
        for (const QString &language : QStringList{"en"} + languages) {
            const QJsonObject translated = catalog(language);
            for (const QString &sentence : wanted)
                if (!translated.contains(sentence))
                    missing << language + ": " + sentence;
        }
        QVERIFY2(missing.isEmpty(), qPrintable(missing.join('\n')));
    }

    void aFailureReadsInTheDesktopsLanguage() {
        const Failure missing(Failure::Code::NoSuchFile, {"/books/Moss.epub"});
        for (const QString &language : languages) {
            const Speaking speaking(language);
            const QString expected = catalog(language).value("There is no such file: %1").toString().arg("/books/Moss.epub");
            QCOMPARE(missing.text(), expected);
            QVERIFY2(!missing.text().startsWith("There is"), qPrintable(language));
        }
        // Words from elsewhere stay as they were said.
        const Speaking dutch("nl");
        QCOMPARE(Failure::said("the catalog is closed").text(), QString("the catalog is closed"));
    }

    void formatsSayWhyByCode() {
        QTemporaryDir dir;
        QCOMPARE(openingFails(dir.filePath("missing.epub")), Failure(Failure::Code::NoSuchFile, {dir.filePath("missing.epub")}));
        QVERIFY(Fixtures::write(dir.filePath("broken.epub"), "not a zip"));
        QCOMPARE(openingFails(dir.filePath("broken.epub")).code(), Failure::Code::EpubNotAnArchive);
        Fixtures::Entries locked = Fixtures::epubEntries();
        locked.append(qMakePair(QString("META-INF/rights.xml"), QByteArray("<rights/>")));
        QVERIFY(Fixtures::zip(dir.filePath("locked.epub"), locked));
        QCOMPARE(openingFails(dir.filePath("locked.epub")).code(), Failure::Code::EpubProtected);
        QVERIFY(Fixtures::write(dir.filePath("broken.pdf"), "not a pdf"));
        QCOMPARE(openingFails(dir.filePath("broken.pdf")).code(), Failure::Code::PdfUnreadable);
        QVERIFY(Fixtures::write(dir.filePath("empty.fb2"), ""));
        QCOMPARE(openingFails(dir.filePath("empty.fb2")).code(), Failure::Code::FictionBookEmpty);
        QVERIFY(Fixtures::write(dir.filePath("empty.txt"), ""));
        QCOMPARE(openingFails(dir.filePath("empty.txt")).code(), Failure::Code::FileWithoutText);
        QVERIFY(Fixtures::write(dir.filePath("notes.xyz"), "?"));
        QCOMPARE(openingFails(dir.filePath("notes.xyz")), Failure(Failure::Code::UnknownFormat, {"notes.xyz"}));
        QVERIFY(Fixtures::write(dir.filePath("book.docx"), "?"));
        QCOMPARE(openingFails(dir.filePath("book.docx")), Failure(Failure::Code::ReadAfterConverting, {"Word"}));
    }

    void catalogsSayWhyByCodeAndInTheDesktopsLanguage() {
        QNetworkAccessManager network;
        ClosedCatalog closed(&network);
        const Failure unreachable = lookUpFails(closed);
        QCOMPARE(unreachable.code(), Failure::Code::CatalogUnreachable);
        QCOMPARE(unreachable.arguments().value(0), QString("Closed Catalog"));
        {
            const Speaking french("fr");
            QVERIFY2(unreachable.text().startsWith("Closed Catalog est injoignable : "), qPrintable(unreachable.text()));
        }

        ProcessMetadataSource missing("Shelf Helper", {"title"}, "/nonexistent/shelf-helper", {}, {});
        const Failure unstarted = lookUpFails(missing);
        QCOMPARE(unstarted, Failure(Failure::Code::ProgramWontStart, {"Shelf Helper", "/nonexistent/shelf-helper"}));
        const Speaking german("de");
        QCOMPARE(unstarted.text(), QString("Shelf Helper kann nicht gestartet werden: /nonexistent/shelf-helper"));
    }

    void addingBooksSaysWhyInTheDesktopsLanguage() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("photo.jpg"), "x"));
        QVERIFY(Fixtures::zip(dir.filePath("empty.zip"), {{"notes.nfo", "x"}}));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        QCOMPARE(library.add(dir.filePath("photo.jpg")).error, QString("photo.jpg is not a book format Omabook knows."));
        const Speaking spanish("es-MX");
        QCOMPARE(library.add(dir.filePath("photo.jpg")).error, QString("photo.jpg no es un formato de libro que Omabook conozca."));
        QCOMPARE(library.add(dir.filePath("missing.epub")).error,
                 QString("No hay ningún archivo legible en " + dir.filePath("missing.epub")));
        QCOMPARE(importBooks(library, {dir.filePath("empty.zip")}).problems, QStringList{"No hay libros dentro de empty.zip."});
    }

    void aCountedFailureTakesTheLanguagesPluralForm() {
        const Failure slow(Failure::Code::SourceTooSlow, {"Shelf Helper"}, 45);
        QCOMPARE(slow.text(), QString("Shelf Helper didn't answer within 45 seconds."));
        {
            // English's own one form is in en.json, which a translator reads.
            const Speaking english("en");
            QCOMPARE(Failure(Failure::Code::SourceTooSlow, {"Shelf Helper"}, 1).text(),
                     QString("Shelf Helper didn't answer within 1 second."));
        }
        const Speaking dutch("nl");
        QCOMPARE(slow.text(), QString("Shelf Helper antwoordde niet binnen 45 seconden."));
        QCOMPARE(Failure(Failure::Code::SourceTooSlow, {"Shelf Helper"}, 1).text(),
                 QString("Shelf Helper antwoordde niet binnen 1 seconde."));
    }

    void theCommandLineSaysWhyInEnglishUnlessAsked() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::write(dir.filePath("broken.epub"), "not a zip"));
        QVERIFY(QDir(dir.path()).mkpath("home/.config/omarchy"));
        QVERIFY(Fixtures::write(dir.filePath("home/.config/omarchy/language"), "de\n"));
        const auto said = [&dir](const QStringList &arguments) {
            QProcess omabook;
            QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
            environment.insert("HOME", dir.filePath("home"));
            omabook.setProcessEnvironment(environment);
            omabook.start(OMABOOK_BINARY, arguments + QStringList{"--library", dir.filePath("Library")});
            omabook.waitForFinished(30000);
            return QString::fromUtf8(omabook.readAllStandardError());
        };
        const QString english = said({"toc", dir.filePath("broken.epub")});
        QVERIFY2(english.contains("Not an EPUB: the file is not a readable zip archive."), qPrintable(english));
        const QString german = said({"toc", dir.filePath("broken.epub"), "--translated"});
        QVERIFY2(german.contains(catalog("de").value("Not an EPUB: the file is not a readable zip archive.").toString()),
                 qPrintable(german));
    }

    void theReaderSaysWhyABookCantBeOpenedInDutch() {
        Desk desk("nl", {}, false);
        QVERIFY(desk.show());
        const QString file = desk.dir.filePath("Moss.epub");
        QVERIFY(Fixtures::write(file, "not a zip"));
        const auto added = desk.backend->library()->add(file);
        QVERIFY2(added.id > 0, qPrintable(added.error));
        desk.backend->reload();
        desk.reader->openBook(added.id);
        const QString dutch = catalog("nl").value("Not an EPUB: the file is not a readable zip archive.").toString();
        QVERIFY(!dutch.isEmpty());
        QTRY_COMPARE(desk.reader->error(), dutch);
        QTRY_COMPARE(desk.find("readerError")->property("text").toString(), dutch);
        desk.reader->close();
    }
};

QTEST_MAIN(FailuresTests)
#include "failures_tests.moc"
