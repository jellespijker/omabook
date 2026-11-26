// A line of preformatted text too long for the column wraps inside it, in the
// layout that scrolls and in the one with pages, so no part of a listing is cut
// off (#231). Its own lines and indents stay as the book has them.

#include <QQuickStyle>
#include <QtTest>

#include "desk.h"
#include "fixtures.h"
#include "library.h"

namespace {

// A listing as a technical book has one, and a line with no space to break at.
const QString listing =
    QStringLiteral("<h1>Listing</h1><pre>def greet(name):\n"
                   "    return \"Hello, \" + name + \", welcome to the lighthouse; the keeper is out on the shore today\"  # END_OF_LINE\n"
                   "\n"
                   "# %1END_OF_WORD</pre><p>Text after the listing.</p>");

QString shownText(QQuickItem *text) {
    QString words;
    QMetaObject::invokeMethod(text, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0), Q_ARG(int, text->property("length").toInt()));
    return words;
}

QRectF placeOf(QQuickItem *text, qsizetype position) {
    QRectF place;
    QMetaObject::invokeMethod(text, "positionToRectangle", Q_RETURN_ARG(QRectF, place), Q_ARG(int, static_cast<int>(position)));
    return place;
}

} // namespace

class PreformattedTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    void aLongLineWrapsInItsColumn_data() {
        QTest::addColumn<QString>("layout");
        QTest::addColumn<QString>("align");
        QTest::newRow("automatic") << QString("auto") << QString("start");
        QTest::newRow("pages, justified") << QString("pages") << QString("justify");
        QTest::newRow("scroll") << QString("scroll") << QString("start");
    }
    void aLongLineWrapsInItsColumn() {
        QFETCH(QString, layout);
        QFETCH(QString, align);
        Desk desk("en", {}, false);
        const QString file = desk.dir.filePath("code.epub");
        QVERIFY(Fixtures::chapteredEpub(file, "Long Code Lines", {{"Listing", listing.arg(QString(200, 'x'))}}));
        const qint64 book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        desk.backend->reload();
        QVERIFY(desk.show());
        QVERIFY(desk.resize(QSize(1600, 1000)));
        desk.settings->setOption("layout", layout);
        desk.settings->setOption("align", align);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        QTRY_VERIFY(desk.item("flowText") && shownText(desk.item("flowText")).contains("END_OF_WORD"));
        desk.settle(4);
        QQuickItem *text = desk.item("flowText");
        const QString words = shownText(text);
        desk.shot("preformatted-" + layout);

        // Every character that shows stands inside the column.
        for (qsizetype at = 0; at < words.size(); ++at) {
            if (words.at(at).isSpace())
                continue; // the space a line breaks at hangs past its end
            const QRectF place = placeOf(text, at);
            QVERIFY2(place.left() >= -1 && place.left() <= text->width() + 1,
                     qPrintable(QString("character %1 of %2 at %3, in a column %4 wide")
                                    .arg(at)
                                    .arg(words.size())
                                    .arg(place.left())
                                    .arg(text->width())));
        }
        // The long line goes on below where it starts.
        const qsizetype start = words.indexOf("return");
        QVERIFY(start > 0);
        QVERIFY(placeOf(text, words.indexOf("END_OF_LINE")).top() > placeOf(text, start).top());
        // The listing's own lines and indents are as written.
        QStringList lines = words.split(QRegularExpression("[\\n\\x{2028}\\x{2029}]"));
        QVERIFY2(lines.contains("def greet(name):"), qPrintable(words));
        QVERIFY2(std::ranges::any_of(
                     lines, [](const QString &line) { return line.startsWith("    return ") && line.endsWith("\"  # END_OF_LINE"); }),
                 qPrintable(words));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }
};

QTEST_MAIN(PreformattedTests)
#include "preformatted_tests.moc"
