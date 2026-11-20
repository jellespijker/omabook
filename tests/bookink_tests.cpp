// Book ink (#83) as the library keeps it: strokes on a page of a PDF or a
// comic, an annotation of the page made with the first and gone with the last,
// the strokes a set of it in the change log, a batch an operation, never
// written over; and between two of one's own computers over the stand-in ssh,
// what each drew on a page all kept, an erasing on either going to the other.

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <cmath>

#include "annotationanchors.h"
#include "bookink.h"
#include "changelog.h"
#include "fixtures.h"
#include "library.h"
#include "remote.h"
#include "spaces.h"
#include "sync.h"

namespace {

using Fixtures::whyNot;

// A line of handwriting from one point to another, in ink units, with its pressure.
Ink::Stroke line(QPointF from, QPointF to, qint64 began, const QString &id = Ink::newId()) {
    Ink::Stroke stroke{
        .id = id, .color = QColor("#1a1a1a"), .width = 2.4, .pressured = true, .tilted = false, .began = began, .points = {}};
    for (int i = 0; i <= 20; ++i)
        stroke.points.push_back({.at = from + ((to - from) * (i / 20.0)),
                                 .pressure = 0.3F + (static_cast<float>(i) * 0.02F),
                                 .xTilt = 0,
                                 .yTilt = 0,
                                 .ms = static_cast<quint32>(i * 8)});
    return stroke;
}

QStringList idsOf(const std::vector<Ink::Stroke> &strokes) {
    QStringList ids;
    for (const Ink::Stroke &stroke : strokes)
        ids << stroke.id;
    ids.sort();
    return ids;
}

qint64 answer(const Library &library, const QString &sql) {
    QSqlQuery query = ChangeLog::query(library, sql);
    return query.next() ? query.value(0).toLongLong() : -1;
}

qint64 inkOps(const Library &library) { return answer(library, "SELECT COUNT(*) FROM live_ops WHERE field = 'set:strokes'"); }

QString ssh(const QTemporaryDir &dir) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    return Fixtures::script(dir.filePath("ssh-log"), "exec '" + standIn + "' \"$@\"\n");
}

// Two libraries with the same PDF, synced once so they are paired.
struct TwoComputers {
    TwoComputers() : here(dir.filePath("Here")), there(dir.filePath("There")) {
        QVERIFY(Fixtures::pdf(dir.filePath("Ferry.pdf"), "The Ferry", {"The harbour master counted the crates.", "Mira watched."}));
        QVERIFY(here.open() && there.open());
        mine = here.add(dir.filePath("Ferry.pdf")).id;
        theirs = there.add(dir.filePath("Ferry.pdf")).id;
    }
    [[nodiscard]] Remote remote() const { return Remote("laptop:" + dir.filePath("There"), {ssh(dir)}); }

    QTemporaryDir dir;
    Library here, there;
    qint64 mine = 0, theirs = 0;
};

} // namespace

class BookInkTests : public QObject {
    Q_OBJECT

private slots:
    // Strokes go on their page, the page's annotation made with the first
    // and placed in the file they were drawn on; each batch is one
    // operation, so the log only grows. Erasing part of a stroke is its
    // removal and its pieces' addition; the last stroke erased takes the
    // page's annotation away, and new ink on it later is the same one again.
    void strokesAreKeptOnTheirPageAsBatchesOfTheLog() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::pdf(dir.filePath("Ferry.pdf"), "The Ferry", {"One.", "Two.", "Three."}));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("Ferry.pdf")).id;
        QVERIFY(book > 0);
        const QString hash = AnnotationAnchors::readableFile(library, book)->hash;

        const Ink::Stroke a = line({100, 200}, {600, 220}, 1000, "a");
        const Ink::Stroke b = line({120, 400}, {500, 380}, 2000, "b");
        auto written = BookInk::write(library, book, 1, {.added = {a, b}, .removed = {}}, hash);
        QVERIFY2(written, qPrintable(whyNot(written)));
        QCOMPARE(idsOf(BookInk::strokes(library, book, 1)), (QStringList{"a", "b"}));
        QVERIFY(BookInk::strokes(library, book, 0).empty());
        QCOMPARE(inkOps(library), 1);
        const QList<Annotation> ink = library.annotations(book, "ink");
        QCOMPARE(ink.size(), 1);
        QCOMPARE(ink.first().place.section, 1);
        QCOMPARE(ink.first().anchored, hash);
        QVERIFY(!ink.first().hasPassage());
        const QString gid = Spaces::annotationId(library, ink.first().id);
        // The points come back as they were drawn, to an eighth of an ink unit.
        const Ink::Stroke back = BookInk::strokes(library, book, 1).front();
        QCOMPARE(back.points.size(), a.points.size());
        for (std::size_t i = 0; i < a.points.size(); ++i)
            QVERIFY(std::hypot(back.points[i].at.x() - a.points[i].at.x(), back.points[i].at.y() - a.points[i].at.y()) <= 0.125);

        // Part of a erased: two pieces in its place.
        const Ink::Stroke left = line({100, 200}, {300, 208}, 1000, "a1"), right = line({400, 212}, {600, 220}, 1000, "a2");
        written = BookInk::write(library, book, 1, {.added = {left, right}, .removed = {"a"}}, hash);
        QVERIFY2(written, qPrintable(whyNot(written)));
        QCOMPARE(idsOf(BookInk::strokes(library, book, 1)), (QStringList{"a1", "a2", "b"}));
        QCOMPARE(inkOps(library), 2);
        const QList<BookInk::Page> pages = BookInk::pages(library, book);
        QCOMPARE(pages.size(), 1);
        QCOMPARE(pages.first().page, 1);
        QCOMPARE(pages.first().strokes.size(), std::size_t{3});

        // All of it erased: the page has no ink, and lists none.
        written = BookInk::write(library, book, 1, {.added = {}, .removed = {"a1", "a2", "b"}}, hash);
        QVERIFY2(written, qPrintable(whyNot(written)));
        QVERIFY(BookInk::strokes(library, book, 1).empty());
        QVERIFY(library.annotations(book, "ink").isEmpty());
        QVERIFY(BookInk::pages(library, book).isEmpty());
        QCOMPARE(inkOps(library), 3);
        // Ink on it again is the page's ink as it was, by the same id.
        written = BookInk::write(library, book, 1, {.added = {b}, .removed = {}}, hash);
        QVERIFY2(written, qPrintable(whyNot(written)));
        QCOMPARE(library.annotations(book, "ink").size(), 1);
        QCOMPARE(Spaces::annotationId(library, library.annotations(book, "ink").first().id), gid);
        QCOMPARE(idsOf(BookInk::strokes(library, book, 1)), QStringList{"b"});
        // Nothing to erase on a page without ink is no write.
        written = BookInk::write(library, book, 2, {.added = {}, .removed = {"b"}}, hash);
        QVERIFY(written);
        QVERIFY(library.annotations(book, "ink").size() == 1 && inkOps(library) == 4);
    }

    // A change too large for one write goes as several, all of it kept.
    void manyStrokesGoAsSeveralBatches() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::pdf(dir.filePath("Ferry.pdf"), "The Ferry", {"One."}));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("Ferry.pdf")).id;
        Ink::Change many;
        for (int i = 0; i < 1500; ++i)
            many.added.push_back(line(QPointF(10 + (i % 20) * 49, 10 + (i / 20) * 18), QPointF(50 + (i % 20) * 49, 14 + (i / 20) * 18), i));
        const auto written = BookInk::write(library, book, 0, many);
        QVERIFY2(written, qPrintable(whyNot(written)));
        QVERIFY(inkOps(library) >= 2);
        QCOMPARE(BookInk::strokes(library, book, 0).size(), std::size_t{1500});
    }

    // The part of the page ink covers, measured in the page's width, and
    // where it starts down a page of that shape.
    void theRegionOfAPagesInk() {
        const std::vector<Ink::Stroke> strokes{line({100, 200}, {600, 220}, 1), line({120, 700}, {500, 680}, 2)};
        const PageRegion region = BookInk::regionOf(3, strokes);
        QCOMPARE(region.page, 3);
        QVERIFY(std::abs(region.area.left() - 0.1) < 0.005 && std::abs(region.area.right() - 0.6) < 0.005);
        QVERIFY(std::abs(region.area.top() - 0.2) < 0.005 && std::abs(region.area.bottom() - 0.7) < 0.005);
        QVERIFY(std::abs(region.place(1.4).fraction - (region.area.top() / 1.4)) < 1e-9);
        QCOMPARE(PageRegion::fromJson(region.toJson()), region);
        // Within the page across, whatever the margin.
        const PageRegion edge = BookInk::regionOf(0, std::vector<Ink::Stroke>{line({2, 5}, {990, 30}, 1)}, 40);
        QVERIFY(edge.area.left() >= 0 && edge.area.right() <= 1 && edge.area.top() >= 0);
        QVERIFY(BookInk::regionOf(0, std::vector<Ink::Stroke>{}).area.isNull());
    }

    // Both computers write on the same page before they sync: both made the
    // page's ink, and it is one, holding both their strokes. An erasing on
    // either goes to the other, and the last stroke erased takes the page's
    // ink away there too; a sync after that changes nothing.
    void twoComputersWriteOnOnePageAndEraseOnEither() {
        TwoComputers two;
        const Remote remote = two.remote();
        const SyncSummary first = syncLibrary(two.here, remote);
        QVERIFY2(first.problems.isEmpty() && first.error.isEmpty(), qPrintable(first.error + first.problems.join('\n')));
        const Ink::Stroke mine = line({100, 200}, {600, 220}, 1000, "mine");
        const Ink::Stroke theirs = line({100, 500}, {600, 520}, 2000, "theirs");
        QVERIFY(BookInk::write(two.here, two.mine, 0, {.added = {mine}, .removed = {}}));
        QVERIFY(BookInk::write(two.there, two.theirs, 0, {.added = {theirs}, .removed = {}}));
        QCOMPARE(Spaces::annotationId(two.here, two.here.annotations(two.mine, "ink").first().id),
                 Spaces::annotationId(two.there, two.there.annotations(two.theirs, "ink").first().id));

        const SyncSummary met = syncLibrary(two.here, remote);
        QVERIFY2(met.problems.isEmpty() && met.error.isEmpty(), qPrintable(met.error + met.problems.join('\n')));
        QCOMPARE(idsOf(BookInk::strokes(two.here, two.mine, 0)), (QStringList{"mine", "theirs"}));
        QCOMPARE(idsOf(BookInk::strokes(two.there, two.theirs, 0)), (QStringList{"mine", "theirs"}));
        QCOMPARE(two.here.annotations(two.mine, "ink").size(), 1);
        QCOMPARE(two.there.annotations(two.theirs, "ink").size(), 1);
        // Their strokes are drawn where they were drawn.
        const std::vector<Ink::Stroke> came = BookInk::strokes(two.here, two.mine, 0);
        const auto found = std::ranges::find(came, QString("theirs"), &Ink::Stroke::id);
        QVERIFY(found != came.cend());
        QVERIFY(std::abs(found->points.front().at.y() - 500) <= 0.125 && std::abs(found->points.back().at.x() - 600) <= 0.125);

        // Erased there: gone here.
        QVERIFY(BookInk::write(two.there, two.theirs, 0, {.added = {}, .removed = {"mine"}}));
        QVERIFY(syncLibrary(two.here, remote).problems.isEmpty());
        QCOMPARE(idsOf(BookInk::strokes(two.here, two.mine, 0)), QStringList{"theirs"});
        // The last erased here: the page has no ink there either.
        QVERIFY(BookInk::write(two.here, two.mine, 0, {.added = {}, .removed = {"theirs"}}));
        QVERIFY(two.here.annotations(two.mine, "ink").isEmpty());
        const SyncSummary last = syncLibrary(two.here, remote);
        QVERIFY(last.problems.isEmpty());
        QCOMPARE(last.annotationsSent, 1);
        QVERIFY(two.there.annotations(two.theirs, "ink").isEmpty());
        QVERIFY(BookInk::strokes(two.there, two.theirs, 0).empty());
        const SyncSummary again = syncLibrary(two.here, remote);
        QCOMPARE(again.annotationsSent + again.annotationsTaken, 0);
    }
};

QTEST_MAIN(BookInkTests)
#include "bookink_tests.moc"
