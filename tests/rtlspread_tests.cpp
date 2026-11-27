// The pages of a spread follow each other the way the book reads, whatever the
// window's language (#232): a book whose pages progress right to left, or that
// is written in such a language, has its first page on the right, goes forward
// leftwards by the keys and a finger, and keeps its margin notes at the left.

#include <QBuffer>
#include <QPointingDevice>
#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "fixtures.h"
#include "formats.h"
#include "library.h"

namespace {

// One long chapter of numbered paragraphs in `language`, whose spine says
// `progression` of its pages, or nothing. Long enough for several spreads in
// any font: sixty paragraphs of Arabic are only four pages in DejaVu.
bool book(const QString &path, const QString &language, const QString &progression) {
    const bool arabic = language == "ar";
    QString body = arabic ? QString::fromUtf8("<div dir=\"rtl\"><h1>الأول</h1>") : QString("<div><h1>The First</h1>");
    for (int paragraph = 1; paragraph <= 150; ++paragraph)
        body += (arabic ? QString::fromUtf8("<p>فقرة رقم %1: كان يا ما كان في قديم الزمان ملك عظيم يحكم بلادا واسعة.</p>")
                        : QString("<p>Paragraph %1: once upon a time a great king ruled over wide lands.</p>"))
                    .arg(paragraph);
    body += "</div>";
    const QString spine = progression.isEmpty() ? QString() : QString(" page-progression-direction=\"%1\"").arg(progression);
    return Fixtures::zip(path,
                         {{"mimetype", "application/epub+zip"},
                          {"META-INF/container.xml",
                           R"(<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="c.opf" media-type="application/oebps-package+xml"/></rootfiles></container>)"},
                          {"c.opf", QString(R"(<?xml version="1.0" encoding="utf-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="i">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Spread %1</dc:title><dc:language>%1</dc:language>
<dc:identifier id="i">urn:uuid:spread</dc:identifier></metadata>
<manifest><item id="c" href="c.xhtml" media-type="application/xhtml+xml"/></manifest>
<spine%2><itemref idref="c"/></spine></package>)")
                                        .arg(language, spine)
                                        .toUtf8()},
                          {"c.xhtml", QString("<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>%1</body></html>").arg(body).toUtf8()}});
}

double across(const QQuickItem *item) { return item->mapToScene(QPointF(0, 0)).x(); }

} // namespace

class RtlSpreadTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // The language the library says goes before the file's (#265): a text
    // file, which says none, and an EPUB tagged English, both set to Arabic.
    void theLibrarysLanguageTurnsTheSpread_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("a text file") << "spread.txt";
        QTest::newRow("an EPUB tagged English") << "spread.epub";
    }
    void theLibrarysLanguageTurnsTheSpread() {
        QFETCH(const QString, name);
        Desk desk("en", {}, false);
        const QString file = desk.dir.filePath(name);
        if (name.endsWith(".epub")) {
            QVERIFY(book(file, "en", {}));
        } else {
            const QString line = QString::fromUtf8("كان يا ما كان في قديم الزمان ملك عظيم يحكم بلادا واسعة.\n\n");
            QVERIFY(Fixtures::write(file, line.repeated(150).toUtf8()));
        }
        const qint64 id = desk.backend->library()->add(file).id;
        QVERIFY(id > 0);
        desk.backend->reload();
        QVERIFY(desk.show());
        QVERIFY(desk.resize(QSize(1800, 900)));
        desk.settings->setOption("layout", "pages");
        desk.settings->setPages(2);
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("spreadReader"));
        QQuickItem *spread = desk.item("spreadReader");
        QTRY_VERIFY(spread->property("pages").toInt() > 2);
        QTRY_COMPARE(spread->property("columns").toInt(), 2);
        const auto leftwards = [&desk] { return across(desk.item("page0")) > across(desk.item("page1")); };
        QTRY_VERIFY(!leftwards());

        // Corrected while the book is open.
        BookMeta meta = desk.backend->library()->book(id)->meta;
        meta.language = "ar";
        QVERIFY(desk.backend->library()->setMeta(id, meta));
        desk.backend->reload();
        QTRY_VERIFY(leftwards());
        // And when it is opened anew.
        desk.reader->close();
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen() && desk.find("spreadReader"));
        QTRY_VERIFY(desk.item("page1") && leftwards());
        QCOMPARE(desk.warnings, QStringList());
    }

    // A manga says it is read right to left (#265): its pages stand and turn that way.
    void aMangaReadsRightToLeft_data() {
        QTest::addColumn<QByteArray>("manga");
        QTest::addColumn<bool>("leftwards");
        QTest::newRow("right to left") << QByteArray("YesAndRightToLeft") << true;
        QTest::newRow("not said") << QByteArray("Yes") << false;
    }
    void aMangaReadsRightToLeft() {
        QFETCH(const QByteArray, manga);
        QFETCH(const bool, leftwards);
        const QTemporaryDir dir;
        QImage page(60, 90, QImage::Format_RGB32);
        page.fill(Qt::white);
        QByteArray png;
        QBuffer buffer(&png);
        QVERIFY(buffer.open(QIODevice::WriteOnly) && page.save(&buffer, "PNG"));
        const QString file = dir.filePath("manga.cbz");
        QVERIFY(Fixtures::zip(file,
                              {{"1.png", png}, {"2.png", png}, {"ComicInfo.xml", "<ComicInfo><Manga>" + manga + "</Manga></ComicInfo>"}}));
        const auto document = Formats::opened(file);
        QVERIFY(document);
        QCOMPARE((*document)->pageDirection() == Qt::RightToLeft, leftwards);
    }

    void theSpreadReadsAsTheBookDoes_data() {
        QTest::addColumn<QByteArray>("window");
        QTest::addColumn<QString>("language");
        QTest::addColumn<QString>("progression");
        QTest::addColumn<bool>("leftwards");
        QTest::newRow("Arabic, progression rtl") << QByteArray("en") << "ar" << "rtl" << true;
        QTest::newRow("Arabic, nothing said") << QByteArray("en") << "ar" << "" << true;
        QTest::newRow("English, progression rtl") << QByteArray("en") << "en" << "rtl" << true;
        QTest::newRow("English") << QByteArray("en") << "en" << "" << false;
        QTest::newRow("Arabic window and book") << QByteArray("ar") << "ar" << "rtl" << true;
        QTest::newRow("Arabic window, progression ltr") << QByteArray("ar") << "en" << "ltr" << false;
        QTest::newRow("Arabic window, nothing said") << QByteArray("ar") << "en" << "" << true;
    }
    void theSpreadReadsAsTheBookDoes() {
        QFETCH(QByteArray, window);
        QFETCH(QString, language);
        QFETCH(QString, progression);
        QFETCH(bool, leftwards);
        Desk desk(window, {}, false);
        const QString file = desk.dir.filePath("spread.epub");
        QVERIFY(book(file, language, progression));
        const qint64 id = desk.backend->library()->add(file).id;
        QVERIFY(id > 0);
        desk.backend->reload();
        QVERIFY(desk.show());
        QVERIFY(desk.resize(QSize(1800, 900)));
        desk.settings->setOption("layout", "pages");
        desk.settings->setPages(2);
        desk.reader->openBook(id);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.find("spreadReader"));
        QQuickItem *spread = desk.item("spreadReader");
        QTRY_VERIFY(spread->property("pages").toInt() > 4);
        QTRY_COMPARE(spread->property("columns").toInt(), 2);
        desk.settle(4);
        const QString name = QString("rtlspread-%1-%2-%3").arg(QString::fromLatin1(window), language, progression);
        desk.shot(name);

        QQuickItem *first = desk.item("page0");
        QQuickItem *second = desk.item("page1");
        QTRY_VERIFY2((across(first) > across(second)) == leftwards,
                     qPrintable(QString("first at %1, second at %2").arg(across(first)).arg(across(second))));

        // Forward and back by the arrows.
        const Qt::Key forward = leftwards ? Qt::Key_Left : Qt::Key_Right;
        const Qt::Key back = leftwards ? Qt::Key_Right : Qt::Key_Left;
        const auto page = [spread] { return spread->property("page").toInt(); };
        QCOMPARE(page(), 0);
        QTest::keyClick(desk.window, forward);
        QTRY_COMPARE(page(), 2);
        QTest::keyClick(desk.window, back);
        QTRY_COMPARE(page(), 0);

        // A finger's tap near the edge reading goes on towards.
        const std::unique_ptr<QPointingDevice> finger(QTest::createTouchDevice());
        QQuickItem *area = desk.item("touchArea");
        const QPoint edge = area->mapToScene(QPointF(area->width() * (leftwards ? 0.04 : 0.96), area->height() / 2)).toPoint();
        QTest::touchEvent(desk.window, finger.get()).press(0, edge);
        QTest::touchEvent(desk.window, finger.get()).release(0, edge);
        QTRY_COMPARE(page(), 2);
        QTRY_VERIFY(!desk.item("readerView")->property("turningPage").toBool());
        QTest::keyClick(desk.window, back);
        QTRY_COMPARE(page(), 0);

        // Selecting words by the keyboard, forward is the same way.
        QVERIFY(desk.run("reader.note"));
        QTRY_VERIFY(!first->property("text").value<QQuickItem *>()->property("selectedText").toString().isEmpty());
        const auto chosenFrom = [first] { return first->property("text").value<QQuickItem *>()->property("selectionStart").toInt(); };
        const int word = chosenFrom();
        QTest::keyClick(desk.window, forward);
        QTRY_VERIFY(chosenFrom() > word);

        // A note on them: its margin column stands beyond the spread's last page.
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTRY_VERIFY(spread->property("noteRoom").toDouble() > 0);
        QQuickItem *margin = spread->property("marginColumn").value<QQuickItem *>();
        QVERIFY(margin);
        QTRY_VERIFY(margin->isVisible());
        desk.settle(4);
        QTRY_VERIFY(leftwards ? across(margin) + margin->width() <= across(second) + 1
                              : across(margin) >= across(second) + second->width() - 1);
        QVERIFY(leftwards ? across(second) < across(first) : across(first) < across(second));
        desk.shot(name + "-note");

        // The same side in the layout that scrolls, beside its one column.
        desk.settings->setOption("layout", "scroll");
        QTRY_VERIFY(desk.find("flowReader") && !desk.find("spreadReader"));
        QQuickItem *flow = desk.item("flowReader");
        QTRY_VERIFY((margin = flow->property("marginColumn").value<QQuickItem *>()) && margin->isVisible());
        QQuickItem *column = desk.item("flowText");
        QTRY_VERIFY(leftwards ? across(margin) + margin->width() <= across(column) + 1
                              : across(margin) >= across(column) + column->width() - 1);
        desk.shot(name + "-scroll");
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(RtlSpreadTests)
#include "rtlspread_tests.moc"
