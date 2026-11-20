// Highlights (#81): passages marked in one of five named colours, readable
// on every theme and told apart without colour too; what a stroke of the pen
// over lines of text means (a line under words, a loop round them, a bar
// beside them, a scribble); and highlights made, coloured anew and listed by
// colour, by the library and the command line.

#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <numbers>
#include <set>
#include <vector>

#include "annotationanchors.h"
#include "booktext.h"
#include "fixtures.h"
#include "highlightcolors.h"
#include "highlights.h"
#include "inkgesture.h"
#include "library.h"
#include "settings.h"

namespace {

using Ink::GestureKind;

// Lines of text as a page sets them: `count` lines 20 pixels high, from x 40 to 440.
std::vector<Ink::TextLine> page(int count = 10) {
    std::vector<Ink::TextLine> lines;
    for (int i = 0; i < count; ++i)
        lines.push_back({.box = QRectF(40, 100 + (i * 24), 400, 20)});
    return lines;
}

// A stroke from one point to another, a point every few pixels, wavering as a hand does.
std::vector<QPointF> line(QPointF from, QPointF to, double waver = 1.0) {
    std::vector<QPointF> points;
    const int steps = std::max(8, static_cast<int>(QLineF(from, to).length() / 4));
    for (int i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        points.push_back(from + ((to - from) * t) + QPointF(0, waver * std::sin(i * 1.3)));
    }
    return points;
}

// A loop round a box, as a pen draws it: starting at its left, a little past where it started.
std::vector<QPointF> loop(const QRectF &around) {
    std::vector<QPointF> points;
    for (int i = 0; i <= 64; ++i) {
        const double angle = std::numbers::pi + (i * 2.1 * std::numbers::pi / 64);
        points.emplace_back(around.center().x() + (around.width() / 2 * std::cos(angle)),
                            around.center().y() + (around.height() / 2 * std::sin(angle)));
    }
    return points;
}

// Back and forth over a box, `turns` times.
std::vector<QPointF> scribble(const QRectF &over, int turns) {
    std::vector<QPointF> points;
    for (int i = 0; i <= turns; ++i) {
        const QPointF at(i % 2 == 0 ? over.left() : over.right(), over.top() + (over.height() * i / turns));
        if (!points.empty()) {
            const std::vector<QPointF> part = line(points.back(), at, 0);
            points.insert(points.end(), part.begin() + 1, part.end());
        } else {
            points.push_back(at);
        }
    }
    return points;
}

Ink::Gesture gesture(const std::vector<QPointF> &points, const std::vector<Ink::TextLine> &lines = page()) {
    return Ink::gestureOf(points, lines);
}

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did, and wrote the numbers in a book that nobody read.";

[[nodiscard]] Passage passageOf(const QString &text, const QString &words) {
    const qsizetype at = text.indexOf(words);
    return Passage::of(text, 0, at, at + words.size());
}

} // namespace

class HighlightsTests : public QObject {
    Q_OBJECT

private slots:
    // Five, each its own name and its own line under the words, so a reader
    // who doesn't tell two of the colours apart still tells the highlights apart.
    void fiveColoursEachWithANameAndALineOfItsOwn() {
        const QList<HighlightColors::Color> &all = HighlightColors::all();
        QCOMPARE(all.size(), 5);
        std::set<QString> keys;
        std::set<QVector<qreal>> lines;
        for (const HighlightColors::Color &color : all) {
            keys.insert(color.key);
            lines.insert(color.dashes);
        }
        QCOMPARE(keys.size(), size_t{5});
        QCOMPARE(lines.size(), size_t{5});
        QCOMPARE(all.first().key, QString("yellow"));
        // The first is kept as none, so a highlight from before colours is it.
        QCOMPARE(HighlightColors::stored(u"yellow"), QString());
        QCOMPARE(HighlightColors::stored(u"blue"), QString("blue"));
        QCOMPARE(HighlightColors::stored(u"mauve"), QString());
        QCOMPARE(HighlightColors::of(u"").key, QString("yellow"));
        QVERIFY(!HighlightColors::known(u"mauve"));
    }

    // Laid over the page, light or dark, the words on it read as well as
    // normal text should (4.5), and its line stands out from the page (3).
    void everyColourKeepsTheTextReadableInLightAndDark_data() {
        QTest::addColumn<QColor>("background");
        QTest::addColumn<QColor>("text");
        QTest::newRow("light") << QColor("#fafafa") << QColor("#202020");
        QTest::newRow("paper") << QColor(Qt::white) << QColor(Qt::black);
        QTest::newRow("dark") << QColor("#1e1e2e") << QColor("#cdd6f4");
        QTest::newRow("black") << QColor("#000000") << QColor("#e0e0e0");
        QTest::newRow("warm") << QColor("#fbf1c7") << QColor("#3c3836");
        QTest::newRow("dim") << QColor("#282828") << QColor("#a89984");
    }
    void everyColourKeepsTheTextReadableInLightAndDark() {
        QFETCH(QColor, background);
        QFETCH(QColor, text);
        const double page = HighlightColors::contrast(background, text);
        for (const HighlightColors::Color &color : HighlightColors::all()) {
            const QColor tint = HighlightColors::tint(color.key, background, text);
            QVERIFY2(HighlightColors::contrast(tint, text) >= std::min(4.5, page) - 0.01,
                     qPrintable(QString("%1: %2").arg(color.key).arg(HighlightColors::contrast(tint, text))));
            // And it is there to see: not the page itself.
            QVERIFY2(tint != background, qPrintable(color.key));
            QVERIFY2(HighlightColors::contrast(HighlightColors::line(color.key, background), background) >= 3,
                     qPrintable(color.key + " line"));
        }
    }

    // A line under words highlights the words above it, as far as it goes.
    void aLineUnderWordsMarksThem() {
        const Ink::Gesture under = gesture(line({120, 121}, {300, 120}));
        QCOMPARE(under.kind, GestureKind::Underline);
        QCOMPARE(under.spans.size(), 1);
        QCOMPARE(under.spans.first().line, 0);
        QVERIFY(qAbs(under.spans.first().from - 120) < 1 && qAbs(under.spans.first().to - 300) < 1);
        // Under the third line, and a line struck through the fifth.
        QCOMPARE(gesture(line({60, 170}, {200, 169})).spans.first().line, 2);
        const Ink::Gesture through = gesture(line({60, 206}, {420, 207}));
        QCOMPARE(through.kind, GestureKind::Underline);
        QCOMPARE(through.spans.first().line, 4);
        // Past the end of the line it stops at the line's end.
        QCOMPARE(gesture(line({300, 121}, {600, 121})).spans.first().to, 440.0);
    }

    void aLoopRoundWordsMarksWhatItEncloses() {
        // Round words of the second line.
        const Ink::Gesture round = gesture(loop(QRectF(150, 118, 160, 32)));
        QCOMPARE(round.kind, GestureKind::Circle);
        QCOMPARE(round.spans.size(), 1);
        QCOMPARE(round.spans.first().line, 1);
        QVERIFY(round.spans.first().from > 145 && round.spans.first().to < 315);
        // Round three lines: from where it crosses the first to where it crosses the last.
        const Ink::Gesture three = gesture(loop(QRectF(100, 120, 300, 80)));
        QCOMPARE(three.kind, GestureKind::Circle);
        QCOMPARE(three.spans.size(), 3);
        QCOMPARE(three.spans.first().line, 1);
        QCOMPARE(three.spans.last().line, 3);
    }

    // A bar in the margin, beside lines, marks those lines whole; on either side.
    void aBarInTheMarginMarksTheLinesBesideIt() {
        for (const double x : {28.0, 452.0}) {
            const Ink::Gesture bar = gesture(line({x, 124}, {x + 2, 196}));
            QCOMPARE(bar.kind, GestureKind::Bracket);
            QCOMPARE(bar.spans.size(), 3);
            QCOMPARE(bar.spans.first(), (Ink::LineSpan{.line = 1, .from = 40, .to = 440}));
            QCOMPARE(bar.spans.last().line, 3);
        }
        // The same stroke in the middle of the text is writing, not a bar.
        QCOMPARE(gesture(line({240, 124}, {242, 196})).kind, GestureKind::None);
    }

    void aScribbleOverWordsTakesHighlightsAway() {
        const Ink::Gesture rubbed = gesture(scribble(QRectF(150, 122, 80, 12), 8));
        QCOMPARE(rubbed.kind, GestureKind::Scribble);
        QCOMPARE(rubbed.spans.first().line, 1);
        QVERIFY(rubbed.spans.first().from <= 150 && rubbed.spans.first().to >= 230);
    }

    // What isn't one of them means nothing here: a word written over the
    // text, a dot, a slanting line, a stroke far from any line.
    void anythingElseMeansNothing() {
        QCOMPARE(gesture(line({200, 150}, {201, 151})).kind, GestureKind::None);
        QCOMPARE(gesture(line({100, 110}, {300, 260})).kind, GestureKind::None);
        QCOMPARE(gesture(line({100, 600}, {300, 600})).kind, GestureKind::None);
        QCOMPARE(gesture(line({100, 600}, {300, 600}), {}).kind, GestureKind::None);
        QCOMPARE(gesture({QPointF(5, 5)}).kind, GestureKind::None);
        // A short loop of handwriting, smaller than a line: no circle round words.
        QCOMPARE(gesture(loop(QRectF(200, 126, 8, 8))).kind, GestureKind::None);
    }

    // A highlight made on words of a page that shows them otherwise is
    // placed in the book's own text, in a colour, and found by it.
    void aHighlightIsMadeInAColourAndColouredAnew() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("Book.epub")));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("Book.epub")).id;
        const BookText text(1, [](int) { return story; });
        // The reader's page has the words with hyphens put in, and another place.
        const QString shown = "Before it all: " + QString(story).replace("harbour", "har­bour");
        const auto made = Highlights::make(library, book, passageOf(shown, "The har­bour master counted"), u"blue", &text, "hash");
        QVERIFY2(made, qPrintable(Fixtures::whyNot(made)));
        const QList<Annotation> all = Highlights::of(library, book);
        QCOMPARE(all.size(), 1);
        QCOMPARE(all.first().kind, QString("highlight"));
        QCOMPARE(all.first().color, QString("blue"));
        QCOMPARE(all.first().anchored, QString("hash"));
        QCOMPARE(all.first().quote, QString("The harbour master counted"));
        QCOMPARE(text.offsetOf(all.first().place), story.indexOf("The harbour"));
        QCOMPARE(all.first().toJson().value("color").toString(), QString("blue"));

        // A second, the first colour; one of each.
        QVERIFY(Highlights::make(library, book, passageOf(story, "the ferry came back"), u"yellow", &text, "hash"));
        QCOMPARE(Highlights::of(library, book, u"blue").size(), 1);
        QCOMPARE(Highlights::of(library, book, u"yellow").size(), 1);
        QCOMPARE(Highlights::of(library, book).first().toJson().value("color").toString(), QString("yellow"));
        // Coloured anew; a bookmark has no colour to change, nor a number that isn't there.
        QVERIFY(Highlights::recolor(library, *made, u"purple"));
        QCOMPARE(Highlights::of(library, book, u"purple").size(), 1);
        QVERIFY(Highlights::of(library, book, u"blue").isEmpty());
        QVERIFY(!Highlights::recolor(library, *made, u"mauve"));
        Annotation bookmark;
        bookmark.kind = "bookmark";
        bookmark.text = "Here";
        const qint64 mark = library.addAnnotation(book, bookmark);
        QVERIFY(!Highlights::recolor(library, mark, u"blue"));
        QVERIFY(!Highlights::recolor(library, 9999, u"blue"));
        QCOMPARE(Highlights::of(library, book).size(), 2);
        // No words, no colour of that name: none made.
        QVERIFY(!Highlights::make(library, book, Passage{}, u"blue"));
        QVERIFY(!Highlights::make(library, book, passageOf(story, "ferry"), u"mauve"));
        // Kept in the trash and back with its colour.
        QCOMPARE(Annotation::fromJson(Highlights::of(library, book, u"purple").first().toJson()).color, QString("purple"));
    }

    // `omabook bookmarks <book> --color` lists one colour's highlights, by
    // its name or the one the reader gave it, each line saying its colour.
    void theCommandLineListsOneColour() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("Book.epub")));
        qint64 book = 0;
        {
            Library library(dir.filePath("Library"));
            QVERIFY(library.open());
            book = library.add(dir.filePath("Book.epub")).id;
            QVERIFY(Highlights::make(library, book, passageOf(story, "the ferry came back"), u"green"));
            QVERIFY(Highlights::make(library, book, passageOf(story, "nobody read"), u"orange"));
        }
        Settings().setHighlightName("orange", "Questions");
        const auto run = [&](const QStringList &more) {
            QProcess omabook;
            omabook.start(OMABOOK_BINARY, QStringList{"bookmarks", QString::number(book), "--library", dir.filePath("Library")} + more);
            omabook.waitForFinished(30000);
            return std::pair{omabook.exitCode(), QString::fromUtf8(omabook.readAllStandardOutput() + omabook.readAllStandardError())};
        };
        const auto [all, listed] = run({});
        QCOMPARE(all, 0);
        QVERIFY2(listed.contains("the ferry came back\tgreen") && listed.contains("nobody read\torange"), qPrintable(listed));
        const auto [named, questions] = run({"--color", "questions"});
        QCOMPARE(named, 0);
        QVERIFY2(questions.contains("nobody read") && !questions.contains("ferry"), qPrintable(questions));
        const auto [json, asJson] = run({"--color", "green", "--json"});
        QCOMPARE(json, 0);
        QVERIFY2(asJson.contains("\"color\": \"green\"") && !asJson.contains("orange"), qPrintable(asJson));
        const auto [unknown, why] = run({"--color", "mauve"});
        QCOMPARE(unknown, 1);
        QVERIFY2(why.contains("no highlight colour called mauve"), qPrintable(why));
        Settings().setHighlightName("orange", "");
    }
};

QTEST_MAIN(HighlightsTests)
#include "highlights_tests.moc"
