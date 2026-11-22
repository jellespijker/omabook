// Free ink on a flowing book (#152) as the text lays it out and the library
// keeps it: a stroke in a side margin is anchored to the line beside it, and
// one among the lines to the words under or over it; laid out again in
// another size of type and another width, each is drawn beside its line
// again, at that line's height, the margin's in the margin, kept within a
// margin narrower than it (#157). The strokes of a line are an annotation on
// its passage, its strokes a set in the change log, gone with the last; found
// again by its words on a page that shows the chapter otherwise; and between
// two of one's own computers over the stand-in ssh, what each wrote all kept,
// an erasing on either going to the other.

#include <QFont>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QtTest>

#include <cmath>

#include "fixtures.h"
#include "inklines.h"
#include "library.h"
#include "lineink.h"
#include "remote.h"
#include "sync.h"
#include "textgeometry.h"

namespace {

const QString chapter = "The harbour master counted the crates twice before the ferry left, and twice more when it came back "
                        "empty. Mira watched the lantern swing over the black water while the gulls slept on the roof of the "
                        "ticket office. Nobody spoke of the storm, though everyone had seen the sky go green at noon, and the "
                        "old captain had tied his boat to the pier with three ropes instead of one.";

// A chapter laid out in type `size` pixels high, `width` wide.
struct Page {
    Page(int size, double width) {
        QFont font;
        font.setPixelSize(size);
        document.setDefaultFont(font);
        document.setDocumentMargin(0);
        document.setPlainText(chapter);
        document.setTextWidth(width);
        column = {.left = 0, .right = width, .leftRoom = 80, .rightRoom = 80};
    }
    [[nodiscard]] TextGeometry::Line lineOf(int position) const {
        for (const TextGeometry::Line &line : TextGeometry::linesIn(document, QRectF(QPointF(0, 0), document.size())))
            if (position >= line.start && position < line.end)
                return line;
        return {};
    }
    QTextDocument document;
    InkLines::Column column;
};

// A stroke from one point to another, in pixels of a page.
Ink::Stroke line(QPointF from, QPointF to, const QString &id = Ink::newId()) {
    Ink::Stroke stroke{
        .id = id, .color = QColor("#1a1a1a"), .width = 2.4, .pressured = false, .tilted = false, .began = 1000, .points = {}};
    for (int i = 0; i <= 12; ++i)
        stroke.points.push_back({.at = from + ((to - from) * (i / 12.0)), .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = 0});
    return stroke;
}

// A stroke drawn on a page, kept on the line it is beside, and drawn again on another page.
struct Drawn {
    InkLines::Anchor anchor;
    Ink::Stroke kept;
};
Drawn drawOn(const Page &page, const Ink::Stroke &stroke) {
    const auto anchor = InkLines::anchorOf(page.document, page.column, Ink::bounds(stroke));
    if (!anchor)
        return {};
    const auto placing = InkLines::Placing::of(page.document, page.column, *anchor, {});
    return placing ? Drawn{.anchor = *anchor, .kept = placing->kept(stroke)} : Drawn{};
}
std::optional<Ink::Stroke> shownOn(const Page &page, const Drawn &drawn) {
    const std::vector<Ink::Stroke> kept{drawn.kept};
    const auto placing = InkLines::Placing::of(page.document, page.column, drawn.anchor, kept);
    return placing ? std::optional(placing->shown(drawn.kept)) : std::nullopt;
}

QString ssh(const QTemporaryDir &dir) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    return Fixtures::script(dir.filePath("ssh-log"), "exec '" + standIn + "' \"$@\"\n");
}

QStringList idsOf(const QList<LineInk::Line> &lines) {
    QStringList ids;
    for (const LineInk::Line &line : lines)
        for (const Ink::Stroke &stroke : line.strokes)
            ids << stroke.id;
    ids.sort();
    return ids;
}

// The passage of the words from one place of the chapter to another, as the book has them.
FoundPassage passageOf(const QString &words) {
    const auto at = chapter.indexOf(words);
    return {.passage = Passage::of(chapter, 0, at, at + words.size()), .confidence = 1};
}

} // namespace

class LineInkTests : public QObject {
    Q_OBJECT

private slots:
    // A stroke beside a line in the left margin stays in the left margin,
    // beside the line that starts with the same words, whatever the width and
    // the type; it is as much larger as the line is taller.
    void inkInTheMarginFollowsItsLine() {
        const Page before(16, 420);
        const int at = static_cast<int>(chapter.indexOf("Mira"));
        const TextGeometry::Line mira = before.lineOf(at);
        const double middle = mira.box.center().y();
        const Drawn drawn = drawOn(before, line({-50, middle - 6}, {-20, middle + 6}));
        QCOMPARE(drawn.anchor.where, InkLines::Where::Margin);
        QVERIFY(drawn.anchor.start == mira.start && drawn.anchor.end == mira.end);

        const Page after(24, 300);
        const auto shown = shownOn(after, drawn);
        QVERIFY(shown);
        const QRectF box = Ink::bounds(*shown);
        const TextGeometry::Line now = after.lineOf(mira.start);
        QVERIFY2(box.right() < 0, "the ink stays in the left margin");
        QVERIFY2(box.center().y() > now.box.top() && box.center().y() < now.box.bottom(), "beside the line it was beside");
        const double grown = now.box.height() / mira.box.height();
        QVERIFY(std::abs((box.height() / Ink::bounds(line({-50, middle - 6}, {-20, middle + 6})).height()) - grown) < 0.05);
    }

    // On the right, a stroke stays right of the column as it narrows, beside its line.
    void inkInTheRightMarginStaysThere() {
        const Page before(16, 420);
        const TextGeometry::Line storm = before.lineOf(static_cast<int>(chapter.indexOf("storm")));
        const double middle = storm.box.center().y();
        const Drawn drawn = drawOn(before, line({440, middle}, {470, middle + 4}));
        QCOMPARE(drawn.anchor.where, InkLines::Where::Margin);
        const Page after(16, 260);
        const auto shown = shownOn(after, drawn);
        QVERIFY(shown);
        const QRectF box = Ink::bounds(*shown);
        QVERIFY(box.left() > 260);
        const TextGeometry::Line now = after.lineOf(storm.start);
        QVERIFY(box.center().y() > now.box.top() && box.center().y() < now.box.bottom() + 4);
    }

    // Ink among the lines (an underline) is anchored to the words over it and
    // goes where they go when the lines break elsewhere.
    void inkBetweenTheLinesFollowsItsWords() {
        const Page before(16, 420);
        const int at = static_cast<int>(chapter.indexOf("lantern"));
        const QRectF word = TextGeometry::rects(before.document, at, at + 7).first();
        const Drawn drawn = drawOn(before, line({word.left() + 1, word.bottom() + 1}, {word.right() - 1, word.bottom() + 1}));
        QCOMPARE(drawn.anchor.where, InkLines::Where::Lines);
        QCOMPARE(chapter.mid(drawn.anchor.start, drawn.anchor.end - drawn.anchor.start), QString("lantern"));

        const Page after(20, 280);
        const QRectF moved = TextGeometry::rects(after.document, at, at + 7).first();
        QVERIFY(std::abs(moved.left() - word.left()) > 20);
        const auto shown = shownOn(after, drawn);
        QVERIFY(shown);
        const QRectF box = Ink::bounds(*shown);
        QVERIFY2(std::abs(box.left() - moved.left()) < 4, "under the word where it went");
        QVERIFY(box.top() > moved.top() + (moved.height() / 2) && box.top() < moved.bottom() + 6);
    }

    // A margin narrower than its ink (large type, a narrow window, #157):
    // the ink is drawn smaller, within the page; with the room back, at its size.
    void inkInANarrowMarginIsKeptWithinIt() {
        const Page before(16, 420);
        const TextGeometry::Line first = before.lineOf(0);
        const Drawn drawn = drawOn(before, line({-70, first.box.center().y()}, {-10, first.box.center().y()}));
        Page narrow(40, 300);
        narrow.column.leftRoom = 16;
        const auto small = shownOn(narrow, drawn);
        QVERIFY(small);
        QVERIFY(Ink::bounds(*small).left() >= -16.5 && Ink::bounds(*small).right() <= 0);
        const Page roomy(16, 420);
        const auto again = shownOn(roomy, drawn);
        QVERIFY(again);
        QVERIFY(std::abs(Ink::bounds(*again).left() - Ink::bounds(line({-70, 0}, {-10, 0})).left()) < 0.5);
    }

    // A line's strokes are an annotation on its passage, made with the first
    // and gone with the last; found again by its words in a text that shows
    // the chapter otherwise (a heading above it).
    void aLinesStrokesAreKeptOnItsPassage() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("Ferry.epub"), "The Ferry"));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("Ferry.epub")).id;
        const Ink::Stroke one = line({-40, 10}, {-10, 30}, "one");
        const auto made =
            LineInk::make(library, book, QString::fromLatin1(LineInk::marginKind), passageOf("Mira watched the lantern"), {}, {one});
        QVERIFY(made);
        QVERIFY(LineInk::write(library, *made, {.added = {line({-30, 0}, {-12, 4}, "two")}, .removed = {}}));
        const QList<LineInk::Line> lines = LineInk::lines(library, book);
        QCOMPARE(lines.size(), 1);
        QCOMPARE(lines.first().kind, QString("margin-ink"));
        QCOMPARE(idsOf(lines), (QStringList{"one", "two"}));

        Page page(16, 420);
        page.document.setPlainText("Chapter One\n" + chapter);
        const QList<LineInk::Placed> placed = LineInk::place(page.document, page.document.toPlainText(), page.column, lines);
        QCOMPARE(placed.size(), 1);
        QCOMPARE(page.document.toPlainText().mid(placed.first().anchor.start, 24), QString("Mira watched the lantern"));

        QVERIFY(LineInk::write(library, *made, {.added = {}, .removed = {"one", "two"}}));
        QVERIFY(LineInk::lines(library, book).isEmpty());
        QVERIFY(library.annotations(book, "margin-ink").isEmpty());
        QVERIFY(!LineInk::write(library, *made, {.added = {one}, .removed = {}}));
    }

    // Both computers write beside lines before they sync: each line comes to
    // the other, its strokes where they were drawn. An erasing on either goes
    // to the other, the last stroke of a line taking it away there too.
    void twoComputersWriteBesideLinesAndEraseOnEither() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("Ferry.epub"), "The Ferry"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("Ferry.epub")).id;
        const qint64 theirs = there.add(dir.filePath("Ferry.epub")).id;
        const Remote remote("laptop:" + dir.filePath("There"), {ssh(dir)});
        const SyncSummary first = syncLibrary(here, remote);
        QVERIFY2(first.problems.isEmpty() && first.error.isEmpty(), qPrintable(first.error + first.problems.join('\n')));

        QVERIFY(LineInk::make(here, mine, QString::fromLatin1(LineInk::marginKind), passageOf("Mira watched the lantern"), {},
                              {line({-40, 10}, {-10, 30}, "mine")}));
        QVERIFY(LineInk::make(there, theirs, QString::fromLatin1(LineInk::linesKind), passageOf("storm"), {},
                              {line({0, 104}, {48, 104}, "theirs")}));
        const SyncSummary met = syncLibrary(here, remote);
        QVERIFY2(met.problems.isEmpty() && met.error.isEmpty(), qPrintable(met.error + met.problems.join('\n')));
        QCOMPARE(idsOf(LineInk::lines(here, mine)), (QStringList{"mine", "theirs"}));
        QCOMPARE(idsOf(LineInk::lines(there, theirs)), (QStringList{"mine", "theirs"}));
        const QList<LineInk::Line> came = LineInk::lines(here, mine);
        const auto storm = std::ranges::find(came, QString("line-ink"), &LineInk::Line::kind);
        QVERIFY(storm != came.cend());
        QCOMPARE(storm->passage.quote, QString("storm"));
        QVERIFY(std::abs(storm->strokes.front().points.back().at.x() - 48) <= 0.125);

        // Erased there: gone here, and the line with it.
        const QList<LineInk::Line> thereNow = LineInk::lines(there, theirs);
        const qint64 ofMine = std::ranges::find(thereNow, QString("margin-ink"), &LineInk::Line::kind)->id;
        QVERIFY(LineInk::write(there, ofMine, {.added = {}, .removed = {"mine"}}));
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());
        QCOMPARE(idsOf(LineInk::lines(here, mine)), QStringList{"theirs"});
        QVERIFY(here.annotations(mine, "margin-ink").isEmpty());
    }
};

QTEST_MAIN(LineInkTests)
#include "lineink_tests.moc"
