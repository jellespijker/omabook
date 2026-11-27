// A book's details written into its EPUB, the library's own copy (#74): of
// EPUB 2 and EPUB 3, read back as they were written, the rest of the archive
// as it was, valid as epubcheck sees it, the original put back, a write cut
// short at any point, and the books' files still known for the same between
// two computers once one of them has written into its copy.

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QUrl>
#include <QXmlStreamReader>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>

#include "bookarchive.h"
#include "changelog.h"
#include "fixtures.h"
#include "formats.h"
#include "library.h"
#include "opfdetails.h"
#include "remote.h"
#include "settings.h"
#include "spaces.h"
#include "sync.h"
#include "zipedit.h"

using namespace Qt::StringLiterals;

namespace {

// An EPUB as the specification asks for one: its mimetype first and stored,
// the rest deflated.
bool epubZip(const QString &path, const Fixtures::Entries &entries) {
    archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    bool written = archive_write_open_filename(writer, QFile::encodeName(path).constData()) == ARCHIVE_OK;
    for (const auto &[name, data] : entries) {
        if (name == u"mimetype")
            archive_write_zip_set_compression_store(writer);
        else
            archive_write_zip_set_compression_deflate(writer);
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        written = written && archive_write_header(writer, entry) == ARCHIVE_OK
                  && archive_write_data(writer, data.constData(), size_t(data.size())) == data.size();
        archive_entry_free(entry);
        if (!written)
            qWarning() << "the test's EPUB can't be written:" << name << archive_error_string(writer);
    }
    // What libarchive warns of on closing is no fault of the zip.
    archive_write_close(writer);
    archive_write_free(writer);
    return written && QFileInfo(path).size() > 0;
}

// An EPUB 3 with a navigation document, a series as a collection, refined
// titles and creators, and what the library doesn't write (a contributor,
// rights); with a cover or without one.
Fixtures::Entries epub3Entries(bool cover) {
    const QByteArray opf = QByteArray(R"(<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">urn:uuid:12345678-1234-1234-1234-123456789abc</dc:identifier>
    <dc:title id="t1">Ferry Lines</dc:title>
    <meta refines="#t1" property="title-type">main</meta>
    <dc:creator id="c1">Ida Old</dc:creator>
    <meta refines="#c1" property="role" scheme="marc:relators">aut</meta>
    <meta refines="#c1" property="file-as">Old, Ida</meta>
    <dc:contributor>An Editor</dc:contributor>
    <dc:language>nl</dc:language>
    <dc:rights>Public domain</dc:rights>
    <meta property="belongs-to-collection" id="s1">Harbours</meta>
    <meta refines="#s1" property="collection-type">series</meta>
    <meta refines="#s1" property="group-position">1</meta>
    <meta property="dcterms:modified">2020-01-01T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="ch1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>)")
                           + (cover ? R"(
    <item id="art" href="images/art.jpg" media-type="image/jpeg" properties="cover-image"/>)"
                                    : "")
                           + R"(
  </manifest>
  <spine><itemref idref="ch1"/></spine>
</package>)";
    Fixtures::Entries entries{
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml",
         R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
        {"OEBPS/content.opf", opf},
        {"OEBPS/nav.xhtml", R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Contents</title></head>
<body><nav epub:type="toc" id="toc"><ol><li><a href="text/ch1.xhtml">One</a></li></ol></nav></body></html>)"},
        {"OEBPS/text/ch1.xhtml", R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title></head><body><h1>One</h1><p>The ferry came back.</p></body></html>)"},
    };
    if (cover)
        entries.append({"OEBPS/images/art.jpg", Fixtures::picture(80, 120, Qt::darkGreen, "JPG")});
    return entries;
}

QByteArray readAll(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString packageOf(const Archive &archive) {
    QXmlStreamReader container(archive.read(u"META-INF/container.xml"_s));
    while (!container.atEnd())
        if (container.readNext() == QXmlStreamReader::StartElement && container.name() == u"rootfile")
            return container.attributes().value(u"full-path"_s).toString();
    return {};
}

QByteArray opfOf(const QString &path) {
    const Archive archive(path);
    return archive.read(packageOf(archive));
}

// Each file's record as it lies in the zip, by name.
QHash<QString, QByteArray> records(const QString &path) {
    const QByteArray bytes = readAll(path);
    QHash<QString, QByteArray> found;
    for (const ZipEdit::Entry &entry : ZipEdit::entries(bytes).value_or(QList<ZipEdit::Entry>()))
        found.insert(entry.name, bytes.mid(entry.offset, entry.length));
    return found;
}

// What the package document says, as epubcheck reads it.
struct Seen {
    QSet<QString> ids, refined, manifest, spine, metadata, hrefs;
    QString unique, uniqueElement, version;
    int modified = 0, covers = 0;
    QStringList found;
};

void seeItem(const QXmlStreamAttributes &attributes, const QString &folder, Seen &seen) {
    seen.manifest.insert(attributes.value(u"id"_s).toString());
    seen.hrefs.insert(QDir::cleanPath(folder + QUrl::fromPercentEncoding(attributes.value(u"href"_s).toUtf8())));
    seen.covers += attributes.value(u"properties"_s).split(u' ').contains(u"cover-image") ? 1 : 0;
}

void seeModified(QXmlStreamReader &xml, Seen &seen) {
    ++seen.modified;
    static const QRegularExpression stamp(u"^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ$"_s);
    if (!stamp.match(xml.readElementText()).hasMatch())
        seen.found << u"dcterms:modified is not a time in UTC"_s;
}

void seeElement(QXmlStreamReader &xml, const QString &folder, Seen &seen) {
    const QXmlStreamAttributes attributes = xml.attributes();
    const QString id = attributes.value(u"id"_s).toString();
    if (!id.isEmpty() && seen.ids.contains(id))
        seen.found << u"the id %1 is given twice"_s.arg(id);
    seen.ids.insert(id);
    if (attributes.hasAttribute(u"refines"_s))
        seen.refined.insert(attributes.value(u"refines"_s).toString().mid(1));
    if (xml.name() == u"package") {
        seen.unique = attributes.value(u"unique-identifier"_s).toString();
        seen.version = attributes.value(u"version"_s).toString();
    } else if (xml.namespaceUri() == u"http://purl.org/dc/elements/1.1/") {
        seen.metadata.insert(xml.name().toString());
        if (!id.isEmpty() && id == seen.unique)
            seen.uniqueElement = xml.name().toString();
    } else if (xml.name() == u"meta" && attributes.value(u"property"_s) == u"dcterms:modified") {
        seeModified(xml, seen);
    } else if (xml.name() == u"item") {
        seeItem(attributes, folder, seen);
    } else if (xml.name() == u"itemref") {
        seen.spine.insert(attributes.value(u"idref"_s).toString());
    }
}

// Whether each set's members are all in `in`, said for those that aren't.
void allIn(const QSet<QString> &each, const QSet<QString> &in, const QString &saying, QStringList &found) {
    for (const QString &one : each)
        if (!in.contains(one))
            found << saying.arg(one);
}

// What is wrong with the package document, as epubcheck finds it.
void packageProblems(const QByteArray &opf, const QSet<QString> &files, const QString &folder, QStringList &found) {
    QXmlStreamReader xml(opf);
    Seen seen;
    while (!xml.atEnd())
        if (xml.readNext() == QXmlStreamReader::StartElement)
            seeElement(xml, folder, seen);
    found << seen.found;
    if (xml.hasError())
        found << u"the package is not well-formed: "_s + xml.errorString();
    if (seen.uniqueElement != u"identifier")
        found << u"the unique identifier is not a dc:identifier"_s;
    for (const char *needed : {"title", "language", "identifier"})
        if (!seen.metadata.contains(QLatin1StringView(needed)))
            found << u"no dc:%1"_s.arg(QLatin1StringView(needed));
    if (seen.version.startsWith(u'3') && seen.modified != 1)
        found << u"%1 dcterms:modified, not one"_s.arg(seen.modified);
    if (seen.covers > 1)
        found << u"more than one cover-image"_s;
    allIn(seen.refined, seen.ids, u"refines #%1, which is not there"_s, found);
    allIn(seen.hrefs, files, u"the manifest names %1, which is not in the zip"_s, found);
    allIn(seen.spine, seen.manifest, u"the spine names %1, which is not in the manifest"_s, found);
}

// What epubcheck would say of the commonest faults: the zip, its mimetype,
// every file's checksum, and the package document.
QStringList problems(const QString &path) {
    QStringList found;
    const QByteArray bytes = readAll(path);
    const auto entries = ZipEdit::entries(bytes);
    if (!entries || entries->isEmpty())
        return {u"not a zip"_s};
    const ZipEdit::Entry &first = entries->first();
    const Archive archive(path);
    if (first.name != u"mimetype" || first.method != 0 || first.localExtra != 0 || archive.read(u"mimetype"_s) != "application/epub+zip")
        found << u"the mimetype is not first, stored and plain"_s;
    QSet<QString> files;
    for (const ZipEdit::Entry &entry : *entries) {
        if (files.contains(entry.name))
            found << entry.name + u" is in the zip twice"_s;
        files.insert(entry.name);
        if (const QByteArray data = archive.read(entry.name); data.isNull() || ZipEdit::crc32(data) != entry.crc)
            found << entry.name + u" doesn't match its checksum"_s;
    }
    const QString package = packageOf(archive);
    if (!files.contains(package))
        found << u"no package document"_s;
    packageProblems(archive.read(package), files, package.contains(u'/') ? package.section(u'/', 0, -2) + u'/' : QString(), found);
    return found;
}

struct Shelf {
    QTemporaryDir dir;
    Library library{dir.filePath(u"Library"_s)};
    QString added; // the file the book was added from

    qint64 add(const Fixtures::Entries &entries, const QString &name = u"Added.epub"_s) {
        added = dir.filePath(name);
        if (!epubZip(added, entries) || !library.open())
            return 0;
        return library.add(added).id;
    }
    [[nodiscard]] QString file(qint64 id) const { return library.formatPath(id, u"epub"_s); }
    [[nodiscard]] FormatFile row(qint64 id) const { return library.formatFiles(id).value(0); }
};

BookMeta changedMeta(const BookMeta &before) {
    BookMeta meta = before;
    meta.title = u"The Harbour & the Sea"_s;
    meta.authors = {u"Ann Author"_s, u"Cy Second"_s};
    meta.series = u"Sea Stories"_s;
    meta.seriesIndex = 3.5;
    meta.publisher = u"Quay Books"_s;
    meta.language = u"en"_s;
    meta.isbn = u"9781234567897"_s;
    meta.published = u"2021-04-02"_s;
    meta.description = u"Boats & the people on them."_s;
    meta.tags = {u"Sea"_s, u"Ferries"_s};
    return meta;
}

void expectSaid(const BookMeta &read, const BookMeta &meta) {
    QCOMPARE(read.title, meta.title);
    QCOMPARE(read.authors, meta.authors);
    QCOMPARE(read.series, meta.series);
    QCOMPARE(read.seriesIndex, meta.seriesIndex);
    QCOMPARE(read.publisher, meta.publisher);
    QCOMPARE(read.language, meta.language);
    QCOMPARE(read.isbn, meta.isbn);
    QCOMPARE(read.published, meta.published);
    QCOMPARE(read.description, meta.description);
    QCOMPARE(read.tags, meta.tags);
}

struct Run {
    int code = -1;
    QByteArray output;
};

Run omabook(const QStringList &arguments) {
    QProcess process;
    process.start(QStringLiteral(OMABOOK_BINARY), arguments);
    if (!process.waitForFinished(60000))
        return {};
    return {.code = process.exitCode(), .output = process.readAllStandardOutput()};
}

} // namespace

class EmbeddingTests : public QObject {
    Q_OBJECT

private slots:
    void anEpub2CarriesTheLibrarysDetails() {
        Shelf shelf;
        const qint64 id = shelf.add(Fixtures::epubEntries());
        QVERIFY(id > 0);
        const QByteArray added = readAll(shelf.added);
        const FormatFile before = shelf.row(id);
        const QHash<QString, QByteArray> recordsBefore = records(shelf.file(id));
        QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
        QVERIFY(shelf.library.setIdentifier(id, u"goodreads"_s, u"4671"_s));

        const auto written = shelf.library.writeDetails(id);
        QVERIFY2(written, qPrintable(Fixtures::whyNot(written)));
        QVERIFY(written->changed);
        const QString file = shelf.file(id);
        expectSaid(Formats::readMeta(file), changedMeta(shelf.library.book(id)->meta));
        const QString opf = QString::fromUtf8(opfOf(file));
        QVERIFY2(opf.contains(u"<meta name=\"calibre:series\" content=\"Sea Stories\"/>"_s), qPrintable(opf));
        QVERIFY(opf.contains(u"<meta name=\"calibre:series_index\" content=\"3.5\"/>"_s));
        QVERIFY(opf.contains(u"<dc:identifier opf:scheme=\"GOODREADS\">4671</dc:identifier>"_s));
        QVERIFY(opf.contains(u"<dc:creator opf:role=\"aut\">Cy Second</dc:creator>"_s));
        // The identifier the package is known by stays, and so does the rest of the document.
        QVERIFY(opf.contains(u"<dc:identifier id=\"id\" opf:scheme=\"ISBN\">978-0-00-000000-2</dc:identifier>"_s));
        QVERIFY(opf.contains(u"<spine toc=\"ncx\"><itemref idref=\"ch1\"/><itemref idref=\"ch2\"/></spine>"_s));
        QCOMPARE(opf.count(u"<dc:title>"_s), 1);
        QVERIFY2(problems(file).isEmpty(), qPrintable(problems(file).join(u"; "_s)));

        // Every other file of the archive as it was, byte for byte.
        const QHash<QString, QByteArray> recordsAfter = records(file);
        QCOMPARE(recordsAfter.size(), recordsBefore.size());
        for (auto it = recordsBefore.cbegin(); it != recordsBefore.cend(); ++it)
            if (it.key() != u"OEBPS/content.opf")
                QVERIFY2(recordsAfter.value(it.key()) == it.value(), qPrintable(it.key()));

        // The library knows the file by its new hash and by the one it came in with.
        const FormatFile after = shelf.row(id);
        QCOMPARE(after.hash, Library::hashFile(file));
        QCOMPARE(after.hash, written->hash);
        QCOMPARE(after.size, QFileInfo(file).size());
        QCOMPARE(after.original, before.hash);
        QCOMPARE(shelf.library.bookWithHash(before.hash), id);
        QCOMPARE(shelf.library.bookWithHash(after.hash), id);
        QCOMPARE(Library::hashFile(shelf.library.originalFile(id)), before.hash);
        // The file the book was added from is never touched.
        QCOMPARE(readAll(shelf.added), added);
        // The same file added again is the same book.
        QVERIFY(shelf.library.add(shelf.added).duplicate);
    }

    void anEpub3CarriesTheLibrarysDetails() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(true));
        QVERIFY(id > 0);
        QCOMPARE(shelf.library.book(id)->meta.series, u"Harbours"_s);
        QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
        QVERIFY(shelf.library.setIdentifier(id, u"goodreads"_s, u"4671"_s));
        QVERIFY(shelf.library.setIdentifier(id, u"calibre"_s, u"12"_s));
        const auto written = shelf.library.writeDetails(id);
        QVERIFY2(written, qPrintable(Fixtures::whyNot(written)));

        const QString file = shelf.file(id);
        expectSaid(Formats::readMeta(file), changedMeta(shelf.library.book(id)->meta));
        QVERIFY2(problems(file).isEmpty(), qPrintable(problems(file).join(u"; "_s)));
        const QString opf = QString::fromUtf8(opfOf(file));
        QVERIFY2(opf.contains(u"<meta property=\"belongs-to-collection\" id=\"omabook-series\">Sea Stories</meta>"_s), qPrintable(opf));
        QVERIFY(opf.contains(u"<meta refines=\"#omabook-series\" property=\"group-position\">3.5</meta>"_s));
        QVERIFY(opf.contains(u"<meta name=\"calibre:series\" content=\"Sea Stories\"/>"_s));
        QVERIFY(opf.contains(u"<dc:identifier>urn:isbn:9781234567897</dc:identifier>"_s));
        QVERIFY(opf.contains(u"<dc:identifier>goodreads:4671</dc:identifier>"_s));
        // Calibre's number for the book means nothing outside this library.
        QVERIFY(!opf.contains(u"calibre:12"_s));
        // What the library doesn't write stays, and so does the package's identifier.
        QVERIFY(opf.contains(u"<dc:contributor>An Editor</dc:contributor>"_s));
        QVERIFY(opf.contains(u"<dc:rights>Public domain</dc:rights>"_s));
        QVERIFY(opf.contains(u"<dc:identifier id=\"uid\">urn:uuid:12345678-1234-1234-1234-123456789abc</dc:identifier>"_s));
        // The old title, creator and series went with what refined them.
        for (const QString &gone : {u"Ferry Lines"_s, u"Ida Old"_s, u"Harbours"_s, u"#t1"_s, u"#c1"_s, u"#s1"_s, u"2020-01-01T00:00:00Z"_s})
            QVERIFY2(!opf.contains(gone), qPrintable(gone));
        const QString stamp =
            QDateTime::fromSecsSinceEpoch(shelf.library.book(id)->detailsAt, QTimeZone::UTC).toString(u"yyyy-MM-dd'T'HH:mm:ss'Z'"_s);
        QVERIFY2(opf.contains(u"<meta property=\"dcterms:modified\">"_s + stamp + u"</meta>"_s), qPrintable(opf));
    }

    // epubcheck itself, where the computer has it.
    void epubcheckFindsNothingWrong() {
        const QString epubcheck = QStandardPaths::findExecutable(u"epubcheck"_s);
        if (epubcheck.isEmpty())
            QSKIP("epubcheck is not installed here; problems() checks what it would check most");
        for (const bool three : {false, true}) {
            Shelf shelf;
            const qint64 id = shelf.add(three ? epub3Entries(false) : Fixtures::epubEntries());
            QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
            QVERIFY(shelf.library.writeDetails(id));
            QProcess check;
            check.start(epubcheck, {shelf.file(id)});
            QVERIFY(check.waitForFinished(120000));
            QVERIFY2(check.exitCode() == 0, check.readAllStandardOutput() + check.readAllStandardError());
        }
    }

    void writingAgainChangesNothing() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(true));
        QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
        const auto first = shelf.library.writeDetails(id);
        QVERIFY(first && first->changed);
        const QByteArray once = readAll(shelf.file(id));
        const auto second = shelf.library.writeDetails(id);
        QVERIFY2(second, qPrintable(Fixtures::whyNot(second)));
        QVERIFY(!second->changed);
        QCOMPARE(readAll(shelf.file(id)), once);
        // Nothing left beside it but the file kept as it came in.
        QCOMPARE(QDir(QFileInfo(shelf.file(id)).absolutePath()).entryList(QDir::Files | QDir::Hidden).filter(u".part"_s), QStringList());
    }

    // A cover of the library's own replaces the file's, in its own format,
    // or is added when the file has none; the file's own is left as it is.
    void theCoverGoesIn() {
        Shelf png;
        const qint64 id = png.add(Fixtures::epubEntries());
        const QByteArray ownCover = Archive(png.file(id)).read(u"OEBPS/images/cover.png"_s);
        QVERIFY(png.library.setMeta(id, changedMeta(png.library.book(id)->meta)));
        QVERIFY(png.library.writeDetails(id));
        QCOMPARE(Archive(png.file(id)).read(u"OEBPS/images/cover.png"_s), ownCover);

        QVERIFY(png.library.setCover(id, QImage::fromData(Fixtures::picture(300, 450, Qt::yellow))));
        QVERIFY(png.library.writeDetails(id));
        const QImage now = QImage::fromData(Archive(png.file(id)).read(u"OEBPS/images/cover.png"_s), "PNG");
        QCOMPARE(now.size(), QSize(300, 450));
        QCOMPARE(now.convertToFormat(QImage::Format_RGB32), QImage(png.library.coverPath(id)).convertToFormat(QImage::Format_RGB32));
        QVERIFY2(problems(png.file(id)).isEmpty(), qPrintable(problems(png.file(id)).join(u"; "_s)));

        Shelf none;
        const qint64 bare = none.add(epub3Entries(false));
        QVERIFY(none.library.setCover(bare, QImage::fromData(Fixtures::picture(200, 300, Qt::cyan))));
        QVERIFY(none.library.writeDetails(bare));
        const QString file = none.file(bare);
        QCOMPARE(readAll(none.library.coverPath(bare)), Archive(file).read(u"OEBPS/omabook-cover.jpg"_s));
        const QString opf = QString::fromUtf8(opfOf(file));
        QVERIFY2(
            opf.contains(u"<item id=\"omabook-cover\" href=\"omabook-cover.jpg\" media-type=\"image/jpeg\" properties=\"cover-image\"/>"_s),
            qPrintable(opf));
        QVERIFY(!Formats::readMeta(file).cover.isNull());
        QVERIFY2(problems(file).isEmpty(), qPrintable(problems(file).join(u"; "_s)));
    }

    void theOriginalComesBack() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(true));
        const QByteArray original = readAll(shelf.file(id));
        const QString hash = shelf.row(id).hash;
        QCOMPARE(shelf.library.restoreFile(id).error().code(), Failure::Code::NoOriginalKept);
        QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
        QVERIFY(shelf.library.writeDetails(id));
        // A second write keeps the first original, not the file the first wrote.
        BookMeta again = shelf.library.book(id)->meta;
        again.title = u"Third Title"_s;
        QVERIFY(shelf.library.setMeta(id, again));
        QVERIFY(shelf.library.writeDetails(id));
        QCOMPARE(shelf.row(id).original, hash);

        const auto restored = shelf.library.restoreFile(id);
        QVERIFY2(restored, qPrintable(Fixtures::whyNot(restored)));
        QCOMPARE(readAll(shelf.file(id)), original);
        QCOMPARE(shelf.row(id).hash, hash);
        QCOMPARE(shelf.row(id).original, QString());
        QCOMPARE(shelf.library.originalFile(id), QString());
        QVERIFY(!QFileInfo::exists(QFileInfo(shelf.file(id)).absolutePath() + u"/.original"_s));
        QCOMPARE(Formats::readMeta(shelf.file(id)).title, u"Ferry Lines"_s);
        // The library's details are its own still.
        QCOMPARE(shelf.library.book(id)->meta.title, u"Third Title"_s);
    }

    // Stopped at any point, a write leaves the book's file whole: the old one
    // or the new one, and the library told of the one that is there.
    void aWriteCutShortLeavesTheFileWhole() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(true));
        const QString hash = shelf.row(id).hash;
        // Its folder follows its title, so the file is where it is after that.
        QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
        const QString file = shelf.file(id);

        // Its new file half written, by a write that stopped long ago.
        const QString partial = file + u".details-99999-1.part"_s;
        QVERIFY(Fixtures::write(partial, readAll(file).left(100)));
        QFile left(partial);
        QVERIFY(left.open(QIODevice::ReadWrite));
        QVERIFY(left.setFileTime(QDateTime::currentDateTime().addDays(-1), QFileDevice::FileModificationTime));
        left.close();
        QCOMPARE(Library::hashFile(file), hash);
        QVERIFY(shelf.library.writeDetails(id));
        QVERIFY(!QFileInfo::exists(partial));

        // Put in place, and stopped before the library was told.
        QVERIFY(ChangeLog::exec(shelf.library, u"UPDATE formats SET hash = ?, original = '' WHERE book_id = ?"_s, {hash, id}));
        const auto again = shelf.library.writeDetails(id);
        QVERIFY2(again, qPrintable(Fixtures::whyNot(again)));
        QCOMPARE(shelf.row(id).hash, Library::hashFile(file));
        QCOMPARE(shelf.row(id).original, hash);
        QVERIFY(shelf.library.restoreFile(id));
        QCOMPARE(Library::hashFile(file), hash);

        // A folder it can't write in: the file stays as it was.
        const QString folder = QFileInfo(file).absolutePath();
        QVERIFY(QFile::setPermissions(folder, QFile::ReadOwner | QFile::ExeOwner));
        const auto writable =
            qScopeGuard([&folder] { QFile::setPermissions(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner); });
        if (QFile probe(folder + u"/probe"_s); probe.open(QIODevice::WriteOnly)) {
            probe.remove();
            QSKIP("this user writes where permissions say it can't (root)");
        }
        const auto refused = shelf.library.writeDetails(id);
        QVERIFY(!refused);
        QCOMPARE(Library::hashFile(file), hash);
        QCOMPARE(shelf.row(id).hash, hash);
        QCOMPARE(shelf.row(id).original, QString());
    }

    // What can't be changed in place is left as it is.
    void aDamagedOrOtherBookIsLeftAlone() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(false));
        const QString file = shelf.file(id);
        const QByteArray whole = readAll(file);
        QCOMPARE(ZipEdit::edited(whole.left(whole.size() - 10), {}).error().code(), Failure::Code::ZipUnchangeable);
        QVERIFY(QFile::setPermissions(file, QFile::ReadOwner | QFile::WriteOwner));
        QVERIFY(Fixtures::write(file, whole.left(whole.size() / 2)));
        QVERIFY(ChangeLog::exec(shelf.library, u"UPDATE formats SET hash = ? WHERE book_id = ?"_s, {Library::hashFile(file), id}));
        QVERIFY(!shelf.library.writeDetails(id));
        QCOMPARE(readAll(file), whole.left(whole.size() / 2));
        QVERIFY(shelf.library.originalFile(id).isEmpty());

        // A book with no EPUB has nothing to write into.
        QVERIFY(Fixtures::write(shelf.dir.filePath(u"Notes.txt"_s), "Some notes.\n"));
        const qint64 notes = shelf.library.add(shelf.dir.filePath(u"Notes.txt"_s)).id;
        QVERIFY(notes > 0);
        QCOMPARE(shelf.library.writeDetails(notes).error().code(), Failure::Code::DetailsNeedEpub);
    }

    // Removed, a book is remembered by both its hashes, so no sync brings
    // either back.
    void aRemovedBookIsRememberedByBothItsHashes() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(false));
        const QString before = shelf.row(id).hash;
        QVERIFY(shelf.library.setMeta(id, changedMeta(shelf.library.book(id)->meta)));
        QVERIFY(shelf.library.writeDetails(id));
        const QString after = shelf.row(id).hash;
        QVERIFY(shelf.library.remove(id));
        QVERIFY(shelf.library.wasRemoved(before));
        QVERIFY(shelf.library.wasRemoved(after));
    }

    // Between two computers, a book whose details one wrote into its copy
    // stays one book: each knows the other's file by the hash it came in
    // with, and a third that gets the written file knows that hash too.
    void aSyncKnowsTheFileByTheHashItCameInWith() {
        QTemporaryDir dir;
        const QString book = dir.filePath(u"Ferry.epub"_s);
        QVERIFY(epubZip(book, epub3Entries(true)));
        Library a(dir.filePath(u"A"_s)), b(dir.filePath(u"B"_s)), c(dir.filePath(u"C"_s));
        QVERIFY(a.open() && b.open() && c.open());
        const qint64 mine = a.add(book).id;
        QVERIFY(mine > 0 && b.add(book).id > 0);
        const QString ssh = Fixtures::standInSsh(dir.path(), QStringLiteral(OMABOOK_BINARY));
        const auto sync = [&](Library &here, const QString &there) {
            const SyncSummary summary = syncLibrary(here, Remote(u"laptop:"_s + dir.filePath(there), {ssh}));
            QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join(u"; "_s)));
        };
        sync(a, u"B"_s);
        QVERIFY(a.setMeta(mine, changedMeta(a.book(mine)->meta)));
        QVERIFY(a.writeDetails(mine));
        const QJsonArray files = libraryManifest(a).value("books").toArray().first().toObject().value("files").toArray();
        QCOMPARE(files.first().toObject().value("original").toString(), Library::hashFile(book));

        sync(a, u"B"_s);
        QCOMPARE(a.books().size(), 1);
        QCOMPARE(b.books().size(), 1);
        sync(c, u"A"_s);
        QCOMPARE(c.books().size(), 1);
        QCOMPARE(c.formatFiles(c.books().first().id).first().hash, a.formatFiles(mine).first().hash);
        QCOMPARE(c.formatFiles(c.books().first().id).first().original, Library::hashFile(book));
        sync(c, u"B"_s);
        QCOMPARE(c.books().size(), 1);
        QCOMPARE(b.books().size(), 1);
    }

    // A book whose details were written into its file keeps its id and the hash it came in with
    // where it goes, sent or fetched, so another computer of one's own that has the file as it
    // came in meets it there as the one book, never as a second (#255).
    void aBookKeepsItsIdAndOriginalWhereItGoes() {
        QTemporaryDir dir;
        const QString book = dir.filePath(u"Ferry.epub"_s);
        QVERIFY(epubZip(book, epub3Entries(true)));
        const QString came = Library::hashFile(book);
        Library a(dir.filePath(u"A"_s)), other(dir.filePath(u"Other"_s)), sent(dir.filePath(u"Sent"_s)),
            fetched(dir.filePath(u"Fetched"_s));
        QVERIFY(a.open() && other.open() && sent.open() && fetched.open());
        const qint64 mine = a.add(book).id;
        QVERIFY(mine > 0 && other.add(book).id > 0);
        const QString ssh = Fixtures::standInSsh(dir.path(), QStringLiteral(OMABOOK_BINARY));
        const auto sync = [&](Library &here, const QString &there) {
            const SyncSummary summary = syncLibrary(here, Remote(u"laptop:"_s + dir.filePath(there), {ssh}));
            [&] {
                QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(), qPrintable(summary.error + summary.problems.join(u"; "_s)));
            }();
            return summary;
        };
        // The other computer has synced with A, then A writes the book's details into its file.
        sync(other, u"A"_s);
        QVERIFY(a.setMeta(mine, changedMeta(a.book(mine)->meta)));
        QVERIFY(a.writeDetails(mine));
        const QString gid = Spaces::bookId(a, mine);
        QVERIFY(a.formatFiles(mine).first().hash != came && !gid.isEmpty());
        // Sent: A gives its book to an empty library there. Fetched: an empty library here takes it from A.
        sync(a, u"Sent"_s);
        sync(fetched, u"A"_s);
        for (Library *there : {&sent, &fetched}) {
            QCOMPARE(there->books().size(), 1);
            const qint64 theirs = there->books().first().id;
            QCOMPARE(Spaces::bookId(*there, theirs), gid);
            QCOMPARE(there->formatFiles(theirs).first().original, came);
        }
        for (const QString &there : {u"Sent"_s, u"Fetched"_s}) {
            const SyncSummary summary = sync(other, there);
            QVERIFY2(summary.received == 0 && summary.sent == 0,
                     qPrintable(there + u": %1 received, %2 sent"_s.arg(summary.received).arg(summary.sent)));
            QCOMPARE(other.books().size(), 1);
        }
    }

    // `omabook embed`, and the setting that has `omabook set` write too.
    void theCommandWritesAndPutsBack() {
        Shelf shelf;
        const qint64 id = shelf.add(epub3Entries(true));
        const QString library = shelf.library.root();
        const QString hash = shelf.row(id).hash;
        QCOMPARE(omabook({u"set"_s, QString::number(id), u"--title"_s, u"By The Command"_s, u"--library"_s, library}).code, 0);
        QCOMPARE(Formats::readMeta(shelf.file(id)).title, u"Ferry Lines"_s);

        const Run written = omabook({u"embed"_s, QString::number(id), u"--json"_s, u"--library"_s, library});
        QCOMPARE(written.code, 0);
        const QJsonObject said = QJsonDocument::fromJson(written.output).object().value("books").toArray().first().toObject();
        QVERIFY2(said.value("written").toBool(), written.output.constData());
        QCOMPARE(Formats::readMeta(shelf.file(id)).title, u"By The Command"_s);
        QCOMPARE(omabook({u"embed"_s, QString::number(id), u"--restore"_s, u"--library"_s, library}).code, 0);
        QCOMPARE(Library::hashFile(shelf.file(id)), hash);

        Settings settings;
        settings.setWriteDetails(true);
        const auto off = qScopeGuard([] { Settings().setWriteDetails(false); });
        QCOMPARE(omabook({u"set"_s, QString::number(id), u"--title"_s, u"Set And Written"_s, u"--library"_s, library}).code, 0);
        QCOMPARE(Formats::readMeta(shelf.file(id)).title, u"Set And Written"_s);
        QCOMPARE(omabook({u"embed"_s, u"--all"_s, u"--library"_s, library}).code, 0);
    }

    // A library from before keeps its books' files as they were.
    void anOlderLibrarysFilesAreKept() {
        Shelf shelf;
        const qint64 id = shelf.add(Fixtures::epubEntries());
        const FormatFile before = shelf.row(id);
        QVERIFY(ChangeLog::exec(shelf.library, u"PRAGMA user_version = 39"_s));
        Library again(shelf.library.root());
        QVERIFY(again.open());
        const FormatFile after = again.formatFiles(id).value(0);
        QCOMPARE(after.hash, before.hash);
        QCOMPARE(after.path, before.path);
        QCOMPARE(after.original, QString());
        QCOMPARE(again.bookWithHash(before.hash), id);
    }
};

QTEST_GUILESS_MAIN(EmbeddingTests)
#include "embedding_tests.moc"
