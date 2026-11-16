
// The fixtures live here rather than in the header: moc reads headers and
// loses its place in raw string literals.
#include "fixtures.h"

#include <QBuffer>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QPageSize>
#include <QPainter>
#include <QPair>
#include <QPdfWriter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QtEndian>
#include <archive.h>
#include <archive_entry.h>

namespace Fixtures {

bool write(const QString &path, const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QByteArray picture(int width, int height, const QColor &color, const char *format) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, format);
    return bytes;
}


bool zip(const QString &path, const Entries &entries) {
    archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    if (archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
        archive_write_free(writer);
        return false;
    }
    for (const auto &[name, data] : entries) {
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.toUtf8().constData());
        archive_entry_set_size(entry, data.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_write_header(writer, entry);
        archive_write_data(writer, data.constData(), data.size());
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    return true;
}

Entries epubEntries(const QString &title) {
    const QByteArray opf = QString(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>%1</dc:title>
    <dc:creator opf:role="aut">Ann Author</dc:creator>
    <dc:creator>Bo Writer</dc:creator>
    <dc:language>en</dc:language>
    <dc:publisher>Fixture Press</dc:publisher>
    <dc:identifier id="id" opf:scheme="ISBN">978-0-00-000000-2</dc:identifier>
    <dc:description>&lt;p&gt;A book made for tests.&lt;/p&gt;</dc:description>
    <dc:subject>Testing</dc:subject>
    <dc:subject>Fixtures</dc:subject>
    <dc:date>2020-05-17T00:00:00Z</dc:date>
    <meta name="calibre:series" content="The Fixtures"/>
    <meta name="calibre:series_index" content="2"/>
    <meta name="cover" content="cover-image"/>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="cover-image" href="images/cover.png" media-type="image/png"/>
    <item id="wide" href="images/wide%20map.png" media-type="image/png"/>
    <item id="ch1" href="text/ch1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="text/ch2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx"><itemref idref="ch1"/><itemref idref="ch2"/></spine>
</package>)").arg(title).toUtf8();
    const QByteArray ncx = R"(<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <navMap>
    <navPoint id="a"><navLabel><text>One</text></navLabel><content src="text/ch1.xhtml"/>
      <navPoint id="b"><navLabel><text>A note</text></navLabel><content src="text/ch2.xhtml#note"/></navPoint>
    </navPoint>
    <navPoint id="c"><navLabel><text>Two</text></navLabel><content src="text/ch2.xhtml"/></navPoint>
  </navMap>
</ncx>)";
    const QByteArray ch1 = R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>One</title>
<link rel="stylesheet" href="../style.css"/><style>p { color: red }</style></head>
<body><h1 id="top">Chapter One</h1>
<p class="first">It was a <span class="it">dark</span>&nbsp;night, and 1 &lt; 2.</p>
<p class="quote">Centered words.</p>
<script>alert("no")</script>
<p><img src="../images/wide%20map.png" alt="map"/> See <a href="ch2.xhtml#note">the note</a>
and <a href="https://example.com/">the web</a> and <a href="missing.xhtml">nowhere</a>.</p>
</body></html>)";
    const QByteArray ch2 = R"(<html><body><h1>Chapter Two</h1><p>The end.<p id="note">A note, unclosed.</body></html>)";
    return {
        {"mimetype", "application/epub+zip"},
        {"META-INF/container.xml",
         R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
        {"OEBPS/content.opf", opf},
        {"OEBPS/toc.ncx", ncx},
        {"OEBPS/style.css", ".it { font-style: italic } p.quote, .other { text-align: center; font-weight: bold }"},
        {"OEBPS/images/cover.png", picture(60, 90, Qt::red)},
        {"OEBPS/images/wide map.png", picture(1200, 600, Qt::blue)},
        {"OEBPS/text/ch1.xhtml", ch1},
        {"OEBPS/text/ch2.xhtml", ch2},
    };
}

bool epub(const QString &path, const QString &title) { return zip(path, epubEntries(title)); }

QByteArray fb2() {
    return QByteArray(R"(<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0" xmlns:l="http://www.w3.org/1999/xlink">
<description><title-info>
  <genre>sf_history</genre>
  <author><first-name>Lev</first-name><last-name>Tester</last-name></author>
  <book-title>Fiction Fixture</book-title>
  <annotation><p>About <emphasis>this</emphasis> book.</p></annotation>
  <coverpage><image l:href="#cover.png"/></coverpage>
  <lang>ru</lang>
  <sequence name="Cycle" number="3"/>
</title-info>
<document-info><author><nickname>scanner</nickname></author></document-info>
<publish-info><publisher>Samizdat</publisher><year>1999</year><isbn>5-00-000000-0</isbn></publish-info>
</description>
<body>
  <section><title><p>First</p></title><p>Opening <emphasis>words</emphasis><a l:href="#n1" type="note">[1]</a>.</p>
    <poem><stanza><v>A line</v><v>Another</v></stanza></poem></section>
  <section><title><p>Second</p></title><p>Closing words.</p><image l:href="#cover.png"/></section>
</body>
<body name="notes"><section id="n1"><title><p>1</p></title><p>The footnote.</p></section></body>
<binary id="cover.png" content-type="image/png">)")
           + picture(40, 60, Qt::green).toBase64() + "</binary>\n</FictionBook>";
}

bool pdf(const QString &path, const QString &title, const QStringList &pages) {
    QPdfWriter writer(path);
    writer.setTitle(title);
    writer.setCreator("Fixture Author");
    writer.setPageSize(QPageSize(QPageSize::A5));
    QPainter painter(&writer);
    for (int i = 0; i < pages.size(); ++i) {
        if (i > 0)
            writer.newPage();
        painter.drawText(QRect(200, 200, 4000, 1000), pages.at(i));
    }
    return painter.end();
}

bool chapteredEpub(const QString &path, const QString &title, const QList<Chapter> &chapters) {
    QString manifest, spine, points;
    Entries entries{{"mimetype", "application/epub+zip"},
                    {"META-INF/container.xml",
                     R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"}};
    for (int i = 0; i < chapters.size(); ++i) {
        const QString id = QString("c%1").arg(i + 1), file = QString("text/%1.xhtml").arg(id);
        manifest += QString(R"(<item id="%1" href="%2" media-type="application/xhtml+xml"/>)").arg(id, file);
        spine += QString(R"(<itemref idref="%1"/>)").arg(id);
        points += QString(R"(<navPoint id="n%1"><navLabel><text>%2</text></navLabel><content src="%3"/></navPoint>)")
                      .arg(i + 1).arg(chapters.at(i).title, file);
        entries.append({"OEBPS/" + file, QString(R"(<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml"><head><title>%1</title></head><body>%2</body></html>)")
                                             .arg(chapters.at(i).title, chapters.at(i).body).toUtf8()});
    }
    entries.append({"OEBPS/content.opf", QString(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>%1</dc:title><dc:creator opf:role="aut">Ann Author</dc:creator><dc:language>en</dc:language>
    <dc:identifier id="id">urn:uuid:%2</dc:identifier>
  </metadata>
  <manifest><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="plate" href="images/plate.png" media-type="image/png"/>%3</manifest>
  <spine toc="ncx">%4</spine>
</package>)").arg(title, QString::number(qHash(title)), manifest, spine).toUtf8()});
    entries.append({"OEBPS/toc.ncx", QString(R"(<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><navMap>%1</navMap></ncx>)").arg(points).toUtf8()});
    entries.append({"OEBPS/images/plate.png", picture(600, 800, QColor("#3a6b8c"))});
    return zip(path, entries);
}

bool outlinedPdf(const QString &path, const QString &title, int pages, const QList<QPair<QString, int>> &chapters) {
    // Objects: 1 the catalog, 2 the pages, 3 the outline, 4 the font, then
    // each page and its contents, then each chapter of the outline.
    QList<QByteArray> objects(4 + 2 * pages + chapters.size());
    const auto pageObject = [](int page) { return 5 + 2 * page; };
    const auto chapterObject = [pages](int chapter) { return 5 + 2 * pages + chapter; };
    QByteArray kids;
    for (int page = 0; page < pages; ++page) {
        kids += QByteArray::number(pageObject(page)) + " 0 R ";
        QByteArray text = "BT /F1 40 Tf 60 500 Td (" + QByteArray::number(page + 1) + ") Tj ET\n";
        for (const auto &[name, start] : chapters)
            if (start == page)
                text += "BT /F1 18 Tf 60 440 Td (" + name.toLatin1() + ") Tj ET\n";
        for (int line = 0; line < 14; ++line)
            text += "BT /F1 10 Tf 60 " + QByteArray::number(400 - line * 22) + " Td (Line " + QByteArray::number(line + 1)
                    + " of page " + QByteArray::number(page + 1) + ", set as a book is.) Tj ET\n";
        objects[pageObject(page) - 1] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 420 595] /Resources << /Font << /F1 4 0 R >> >> "
                                        "/Contents " + QByteArray::number(pageObject(page) + 1) + " 0 R >>";
        objects[pageObject(page)] = "<< /Length " + QByteArray::number(text.size()) + " >>\nstream\n" + text + "endstream";
    }
    objects[0] = "<< /Type /Catalog /Pages 2 0 R /Outlines 3 0 R /PageMode /UseOutlines >>";
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + QByteArray::number(pages) + " >>";
    objects[2] = chapters.isEmpty() ? QByteArray("<< /Type /Outlines /Count 0 >>")
                                    : "<< /Type /Outlines /First " + QByteArray::number(chapterObject(0)) + " 0 R /Last "
                                          + QByteArray::number(chapterObject(chapters.size() - 1)) + " 0 R /Count "
                                          + QByteArray::number(chapters.size()) + " >>";
    objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
    for (int i = 0; i < chapters.size(); ++i) {
        QByteArray item = "<< /Title (" + chapters.at(i).first.toLatin1() + ") /Parent 3 0 R /Dest ["
                          + QByteArray::number(pageObject(chapters.at(i).second)) + " 0 R /XYZ 0 595 0]";
        if (i > 0)
            item += " /Prev " + QByteArray::number(chapterObject(i - 1)) + " 0 R";
        if (i + 1 < chapters.size())
            item += " /Next " + QByteArray::number(chapterObject(i + 1)) + " 0 R";
        objects[chapterObject(i) - 1] = item + " >>";
    }
    QByteArray file = "%PDF-1.4\n";
    QList<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets << file.size();
        file += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const qsizetype table = file.size();
    file += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const qsizetype offset : offsets)
        file += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    file += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R /Info << /Title (" + title.toLatin1()
            + ") >> >>\nstartxref\n" + QByteArray::number(table) + "\n%%EOF\n";
    return write(path, file);
}

QByteArray be32(quint32 value) {
    QByteArray bytes(4, '\0');
    qToBigEndian(value, bytes.data());
    return bytes;
}

// A Kindle file with only what carries the metadata: the Palm database
// header, a first record with MOBI and EXTH headers, and a cover record.
QByteArray mobi() {
    auto exth = [](quint32 type, const QByteArray &value) { return be32(type) + be32(value.size() + 8) + value; };
    const QByteArray name = "Fallback Name";
    const QByteArray records = exth(100, "Kim Kindle") + exth(101, "Fixture Press") + exth(503, "Kindle Fixture")
                               + exth(105, "Testing; Kindle") + exth(524, "en") + exth(201, be32(0));
    const QByteArray extended = "EXTH" + be32(12 + records.size()) + be32(6) + records;

    const int mobiLength = 232;
    QByteArray first(16 + mobiLength, '\0');
    first.replace(16, 4, "MOBI");
    first.replace(20, 4, be32(mobiLength));
    first.replace(28, 4, be32(65001));
    first.replace(84, 4, be32(16 + mobiLength + extended.size()));
    first.replace(88, 4, be32(name.size()));
    first.replace(108, 4, be32(1));
    first.replace(128, 4, be32(0x40));
    first += extended + name;

    const QByteArray cover = picture(30, 45, Qt::yellow, "JPG");
    QByteArray file(78, '\0');
    file.replace(60, 8, "BOOKMOBI");
    file[77] = 2;
    const quint32 firstAt = 78 + 16;
    file += be32(firstAt) + be32(0) + be32(firstAt + first.size()) + be32(0);
    return file + first + cover;
}

bool calibreLibrary(const QString &folder) {
    const QString first = folder + "/Writer, Bo/Shelved Wrong (1)";
    const QString second = folder + "/Nobody/Old Format (2)";
    if (!QDir().mkpath(first) || !QDir().mkpath(second))
        return false;
    bool ok = epub(first + "/Shelved Wrong - Writer, Bo.epub") && write(first + "/Shelved Wrong - Writer, Bo.txt", "Plain words.\n")
              && write(first + "/cover.jpg", picture(80, 120, Qt::magenta, "JPG"))
              && write(second + "/Old Format - Nobody.original_epub", "x");

    const QString name = "calibre-fixture";
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", name);
        database.setDatabaseName(folder + "/metadata.db");
        ok = ok && database.open();
        const QStringList statements{
            "CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, series_index REAL, pubdate TEXT, path TEXT, has_cover BOOL, "
            "uuid TEXT)",
            "CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)",
            "CREATE TABLE books_authors_link (id INTEGER PRIMARY KEY, book INTEGER, author INTEGER)",
            "CREATE TABLE series (id INTEGER PRIMARY KEY, name TEXT)",
            "CREATE TABLE books_series_link (id INTEGER PRIMARY KEY, book INTEGER, series INTEGER)",
            "CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT)",
            "CREATE TABLE books_tags_link (id INTEGER PRIMARY KEY, book INTEGER, tag INTEGER)",
            "CREATE TABLE publishers (id INTEGER PRIMARY KEY, name TEXT)",
            "CREATE TABLE books_publishers_link (id INTEGER PRIMARY KEY, book INTEGER, publisher INTEGER)",
            "CREATE TABLE languages (id INTEGER PRIMARY KEY, lang_code TEXT)",
            "CREATE TABLE books_languages_link (id INTEGER PRIMARY KEY, book INTEGER, lang_code INTEGER, item_order INTEGER)",
            "CREATE TABLE identifiers (id INTEGER PRIMARY KEY, book INTEGER, type TEXT, val TEXT)",
            "CREATE TABLE comments (id INTEGER PRIMARY KEY, book INTEGER, text TEXT)",
            "CREATE TABLE data (id INTEGER PRIMARY KEY, book INTEGER, format TEXT, name TEXT)",
            "INSERT INTO books VALUES (1, 'Calibre''s Title', 4.0, '1999-03-02 00:00:00+00:00', 'Writer, Bo/Shelved Wrong (1)', 1, "
            "'11111111-aaaa-4bbb-8ccc-000000000001')",
            "INSERT INTO books VALUES (2, 'Old Format', 1.0, '0101-01-01 00:00:00+00:00', 'Nobody/Old Format (2)', 0, "
            "'11111111-aaaa-4bbb-8ccc-000000000002')",
            "INSERT INTO authors VALUES (1, 'Bo Writer'), (2, 'Smith| Jr.'), (3, 'Unknown')",
            "INSERT INTO books_authors_link VALUES (1, 1, 1), (2, 1, 2), (3, 2, 3)",
            "INSERT INTO series VALUES (1, 'Curated Series')",
            "INSERT INTO books_series_link VALUES (1, 1, 1)",
            "INSERT INTO tags VALUES (1, 'curated'), (2, 'calibre')",
            "INSERT INTO books_tags_link VALUES (1, 1, 1), (2, 1, 2)",
            "INSERT INTO publishers VALUES (1, 'Calibre House')",
            "INSERT INTO books_publishers_link VALUES (1, 1, 1)",
            "INSERT INTO languages VALUES (1, 'nld')",
            "INSERT INTO books_languages_link VALUES (1, 1, 1, 0)",
            "INSERT INTO identifiers VALUES (1, 1, 'isbn', '9781111111111'), (2, 1, 'google', 'abc'), (3, 1, 'goodreads', '4671')",
            // What Calibre knows of the reading, in the tables it keeps for it.
            "CREATE TABLE ratings (id INTEGER PRIMARY KEY, rating INTEGER)",
            "CREATE TABLE books_ratings_link (id INTEGER PRIMARY KEY, book INTEGER, rating INTEGER)",
            "CREATE TABLE last_read_positions (id INTEGER PRIMARY KEY, book INTEGER, format TEXT, user TEXT, device TEXT, "
            "cfi TEXT, epoch REAL, pos_frac REAL)",
            "CREATE TABLE annotations (id INTEGER PRIMARY KEY, book INTEGER, format TEXT, user_type TEXT, user TEXT, "
            "timestamp REAL, annot_id TEXT, annot_type TEXT, annot_data TEXT, searchable_text TEXT)",
            "CREATE TABLE custom_columns (id INTEGER PRIMARY KEY, label TEXT, name TEXT, datatype TEXT, "
            "mark_for_delete BOOL DEFAULT 0, editable BOOL DEFAULT 1, display TEXT DEFAULT '{}', is_multiple BOOL DEFAULT 0, "
            "normalized BOOL)",
            "INSERT INTO ratings VALUES (1, 8), (2, 10)",
            "INSERT INTO books_ratings_link VALUES (1, 1, 1)",
            "INSERT INTO last_read_positions VALUES (1, 1, 'EPUB', 'local', 'tablet', 'epubcfi(/2/2/4/1:0)', 1700000000.5, 0.5), "
            "(2, 1, 'EPUB', 'local', 'calibre-desktop-viewer', 'epubcfi(/4/2/4/6/1:20)', 1700001000.25, 1.0)",
            "INSERT INTO comments VALUES (1, 1, '<p>Described <b>by hand</b>.</p>')",
            "INSERT INTO data VALUES (1, 1, 'EPUB', 'Shelved Wrong - Writer, Bo'), (2, 1, 'TXT', 'Shelved Wrong - Writer, Bo'), "
            "(3, 2, 'ORIGINAL_EPUB', 'Old Format - Nobody')",
        };
        for (const QString &statement : statements)
            ok = ok && QSqlQuery(database).exec(statement);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool calibreChange(const QString &folder, const QStringList &statements) {
    const QString name = "calibre-fixture-change";
    bool ok = true;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", name);
        database.setDatabaseName(folder + "/metadata.db");
        ok = database.open();
        for (const QString &statement : statements)
            ok = ok && QSqlQuery(database).exec(statement);
        database.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}

bool calibreShelf(const QString &folder) {
    bool ok = calibreLibrary(folder);
    // Four plain text books, each with words of its own so each is a
    // different file.
    const QStringList titles{"Half Way", "Some Day", "Long Done", "By Hand"};
    QStringList statements;
    for (int index = 0; index < titles.size(); ++index) {
        const int id = index + 3;
        const QString title = titles.at(index);
        const QString path = QString("Reader, Ann/%1 (%2)").arg(title).arg(id);
        ok = ok && QDir().mkpath(folder + "/" + path)
             && write(folder + "/" + path + "/" + title + " - Reader, Ann.txt", ("CHAPTER 1\n\n" + title + ".\n").toUtf8());
        statements << QString("INSERT INTO books VALUES (%1, '%2', 1.0, '2001-01-01 00:00:00+00:00', '%3', 0, "
                              "'11111111-aaaa-4bbb-8ccc-00000000000%1')").arg(id).arg(title, path)
                   << QString("INSERT INTO data VALUES (%1, %2, 'TXT', '%3 - Reader, Ann')").arg(id + 10).arg(id).arg(title);
    }
    statements << QStringList{
        // The columns people add, as Calibre keeps them: plain ones hold a
        // value per book, "normalized" ones hold each value once with a
        // link table beside it.
        "INSERT INTO custom_columns (id, label, name, datatype, is_multiple, normalized) VALUES "
        "(1, 'read', 'Read', 'bool', 0, 0), (2, 'shelf', 'Goodreads Shelf', 'text', 1, 1), "
        "(3, 'date_read', 'Date Read', 'datetime', 0, 0), (4, 'myrating', 'My Rating', 'rating', 0, 1), "
        "(5, 'progress', 'Reading Progress', 'int', 0, 0), (6, 'remarks', 'Remarks', 'comments', 0, 0)",
        "CREATE TABLE custom_column_1 (id INTEGER PRIMARY KEY, book INTEGER, value BOOL)",
        "CREATE TABLE custom_column_2 (id INTEGER PRIMARY KEY, value TEXT)",
        "CREATE TABLE books_custom_column_2_link (id INTEGER PRIMARY KEY, book INTEGER, value INTEGER)",
        "CREATE TABLE custom_column_3 (id INTEGER PRIMARY KEY, book INTEGER, value TEXT)",
        "CREATE TABLE custom_column_4 (id INTEGER PRIMARY KEY, value INTEGER)",
        "CREATE TABLE books_custom_column_4_link (id INTEGER PRIMARY KEY, book INTEGER, value INTEGER)",
        "CREATE TABLE custom_column_5 (id INTEGER PRIMARY KEY, book INTEGER, value INTEGER)",
        "CREATE TABLE custom_column_6 (id INTEGER PRIMARY KEY, book INTEGER, value TEXT)",
        "INSERT INTO custom_column_2 VALUES (1, 'currently-reading'), (2, 'to-read'), (3, 'favourites')",
        // Half Way: being read, by its shelf and by its place in the viewer.
        "INSERT INTO books_custom_column_2_link VALUES (1, 3, 1), (2, 3, 3)",
        "INSERT INTO last_read_positions VALUES (3, 3, 'TXT', 'local', 'calibre-desktop-viewer', 'epubcfi(/6/2/4/1:0)', 1700002000.0, 0.4)",
        "INSERT INTO annotations VALUES (1, 3, 'TXT', 'local', 'viewer', 1700002100.0, 'h-1', 'highlight', "
        "'{\"type\": \"highlight\", \"highlighted_text\": \"Half Way.\", \"notes\": \"Remember this\", \"spine_index\": 0, "
        "\"uuid\": \"h-1\"}', ''), "
        "(2, 3, 'TXT', 'local', 'viewer', 1700002200.0, 'Where I stopped', 'bookmark', "
        "'{\"type\": \"bookmark\", \"title\": \"Where I stopped\", \"pos\": \"epubcfi(/2/2/4/1:0)\", \"pos_type\": \"epubcfi\"}', ''), "
        "(3, 3, 'TXT', 'local', 'viewer', 1700002300.0, 'h-2', 'highlight', "
        "'{\"type\": \"highlight\", \"removed\": true, \"uuid\": \"h-2\"}', '')",
        // Some Day: on the to-read shelf.
        "INSERT INTO books_custom_column_2_link VALUES (3, 4, 2)",
        // Long Done: ticked as read, with the day and the reader's own stars.
        "INSERT INTO custom_column_1 VALUES (1, 5, 1), (2, 4, 0)",
        "INSERT INTO custom_column_3 VALUES (1, 5, '2021-06-05 00:00:00+00:00')",
        "INSERT INTO custom_column_4 VALUES (1, 10)",
        "INSERT INTO books_custom_column_4_link VALUES (1, 5, 1)",
        "INSERT INTO identifiers VALUES (10, 5, 'goodreads', '9001'), (11, 5, 'amazon', 'B00LONGDONE')",
        // By Hand: a percentage typed into a column.
        "INSERT INTO custom_column_5 VALUES (1, 6, 30)",
        "INSERT INTO custom_column_6 VALUES (1, 6, 'Lent to a friend.')",
    };
    return ok && calibreChange(folder, statements);
}

bool calibreConfig(const QString &folder, const QStringList &libraries) {
    auto quoted = [](const QString &text) { return QString("\"" + text + "\""); };
    QStringList uses;
    for (int index = 0; index < libraries.size(); ++index)
        uses << quoted(libraries.at(index)) + ": " + QString::number(index + 1);
    // A plugin is a zip whose __init__.py holds a class built on one of
    // Calibre's plugin classes.
    auto plugin = [&](const QString &file, const QByteArray &code) {
        return zip(folder + "/plugins/" + file, {{"plugin-import-name-fixture.txt", ""}, {"__init__.py", code}});
    };
    return QDir().mkpath(folder + "/plugins")
           && write(folder + "/global.py.json", ("{\"library_path\": " + quoted(libraries.value(0)) + ", \"language\": \"en\"}").toUtf8())
           && write(folder + "/gui.json", ("{\"library_usage_stats\": {" + uses.join(", ") + ", \"/gone/Old Library\": 9}}").toUtf8())
           && write(folder + "/customize.py.json",
                    ("{\"disabled_plugins\": {\"__class__\": \"set\", \"__value__\": [\"Tidy Import\"]}, "
                     "\"enabled_plugins\": {\"__class__\": \"set\", \"__value__\": []}, "
                     "\"plugins\": {\"Bookshop\": " + quoted(folder + "/plugins/Bookshop.zip") + ", "
                     "\"Tidy Import\": " + quoted(folder + "/plugins/Tidy Import.zip") + "}}").toUtf8())
           && plugin("Bookshop.zip",
                     "from calibre.ebooks.metadata.sources.base import Source\n\n\n"
                     "class Bookshop(Source):\n"
                     "    name = 'Bookshop'\n"
                     "    description = _('Downloads details from a bookshop')\n"
                     "    author = 'A. Seller'\n"
                     "    version = (1, 2, 3)\n"
                     "    capabilities = frozenset(['identify'])\n")
           && plugin("Tidy Import.zip",
                     "from calibre.customize import FileTypePlugin\n\n"
                     "def helper():\n    name = 'not this'\n\n"
                     "class TidyImport(FileTypePlugin):\n"
                     "    name = \"Tidy Import\"\n"
                     "    description = \"Tidies books as they are added\"\n"
                     "    author = \"T. Idy\"\n"
                     "    version = (0, 4)\n")
           && plugin("reader_driver.zip",
                     "from calibre.devices.usbms.driver import USBMS\n\n"
                     "class ReaderDriver(USBMS):\n"
                     "    name = 'Pocket Reader'\n"
                     "    author = 'D. River'\n"
                     "    version = (2, 0, 0)\n");
}

QString script(const QString &path, const QByteArray &body) {
    write(path, "#!/bin/bash\n" + body);
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QString standInSsh(const QString &folder, const QString &binary) {
    // With a trash, which the system only moves things to when it is there.
    QDir(folder).mkpath("there-home/.local/share/Trash/files");
    QDir(folder).mkpath("there-home/.local/share/Trash/info");
    const QByteArray home = folder.toUtf8() + "/there-home";
    const QByteArray path = QFileInfo(binary).absolutePath().toUtf8();
    return script(folder + "/ssh", "while [ \"$1\" != \"--\" ]; do shift; done\nshift; shift\n"
                                   "export HOME='" + home + "' XDG_DATA_HOME='" + home + "/.local/share' XDG_CONFIG_HOME='" + home
                                       + "/.config' XDG_STATE_HOME='" + home + "/.local/state' XDG_CACHE_HOME='" + home
                                       + "/.cache' QT_QPA_PLATFORM=offscreen\n"
                                   "export PATH=\"" + path + ":$PATH\"\n"
                                   "exec sh -c \"$1\"\n");
}

} // namespace Fixtures
