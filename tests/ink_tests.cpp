// Ink as data (#78): strokes, their bytes, a sheet's edits and undo, which
// touches are a palm, and the triangles a stroke is drawn with. No window:
// the ink surface's own tests (inksurface_tests.cpp) drive it with events.

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>

#include <cmath>
#include <numbers>

#include "fixtures.h"
#include "inkcodec.h"
#include "inkinput.h"
#include "inklasso.h"
#include "inkmesh.h"
#include "inksheet.h"
#include "inkstroke.h"
#include "inkwriting.h"

using namespace Qt::StringLiterals;

namespace {

using Fixtures::whyNot;

// A line of handwriting: `count` points from `from`, a pixel or two apart
// and wavering, at 240 points a second, the pressure rising and falling.
Ink::Stroke handwriting(QPointF from, int count, quint32 seed = 1) {
    Ink::Stroke stroke{.id = Ink::newId(),
                       .color = QColor(0x20, 0x30, 0x40),
                       .width = 2.5,
                       .pressured = true,
                       .tilted = true,
                       .began = 1'795'000'000'000 + seed,
                       .points = {}};
    for (int i = 0; i < count; ++i) {
        const double t = i + seed;
        stroke.points.push_back(Ink::Point{.at = from + QPointF(i * 1.7, 6.0 * std::sin(t / 5.0)),
                                           .pressure = static_cast<float>(0.5 + 0.4 * std::sin(t / 9.0)),
                                           .xTilt = static_cast<float>(20 + i % 3),
                                           .yTilt = -10.0F,
                                           .ms = static_cast<quint32>(i * 4 + i / 3)});
    }
    return stroke;
}

// A straight line along x, at one pressure, without pressure when it has none.
Ink::Stroke line(double fromX, double toX, double y, std::optional<float> pressure = std::nullopt) {
    Ink::Stroke stroke{.id = Ink::newId(),
                       .color = Qt::black,
                       .width = 4.0,
                       .pressured = pressure.has_value(),
                       .tilted = false,
                       .began = 1000,
                       .points = {}};
    for (double x = fromX; x <= toX + 1e-9; x += 2.0) {
        stroke.points.push_back(Ink::Point{.at = QPointF(x, y), .pressure = pressure.value_or(0.5F), .ms = static_cast<quint32>(x)});
    }
    return stroke;
}

// Two strokes alike once kept: positions to an eighth, pressure to 1/255, tilt to a degree.
bool alike(const Ink::Stroke &a, const Ink::Stroke &b) {
    if (a.id != b.id || a.color != b.color || std::abs(a.width - b.width) > 0.07 || a.pressured != b.pressured || a.tilted != b.tilted
        || a.began != b.began || a.points.size() != b.points.size())
        return false;
    for (std::size_t i = 0; i < a.points.size(); ++i) {
        const Ink::Point &p = a.points.at(i);
        const Ink::Point &q = b.points.at(i);
        if (QLineF(p.at, q.at).length() > 0.1 || std::abs(p.pressure - q.pressure) > 0.003F || std::abs(p.xTilt - q.xTilt) > 0.51F
            || std::abs(p.yTilt - q.yTilt) > 0.51F || p.ms != q.ms)
            return false;
    }
    return true;
}

// How far the mesh reaches across a horizontal line, its soft edge too.
double thickness(const Ink::Mesh &mesh, double atX) {
    double top = 1e9;
    double bottom = -1e9;
    for (const Ink::Vertex &v : mesh.vertices) {
        if (std::abs(v.x - atX) < 1.5) {
            top = std::min<double>(top, v.y);
            bottom = std::max<double>(bottom, v.y);
        }
    }
    return bottom - top;
}

} // namespace

class InkTests : public QObject {
    Q_OBJECT

private slots:
    // A stroke read back from its bytes is the stroke written, to the
    // steps kept, and a point of handwriting takes about four bytes.
    void aStrokeRoundTripsInAFewBytesAPoint() {
        const Ink::Stroke stroke = handwriting(QPointF(40.25, 812.5), 300);
        const QByteArray bytes = Ink::encode(stroke);
        const auto back = Ink::decode(bytes, stroke.id);
        QVERIFY2(back.has_value(), qPrintable(whyNot(back)));
        QVERIFY(alike(*back, stroke));
        // Written again, it is the same bytes: nothing drifts at each round.
        QCOMPARE(Ink::encode(*back), bytes);
        const double perPoint = static_cast<double>(bytes.size()) / 300.0;
        qInfo() << "300 points of handwriting with pressure and tilt:" << bytes.size() << "bytes," << perPoint << "a point";
        QVERIFY2(perPoint < 7.0, qPrintable(QString::number(perPoint)));
        // Without pressure or tilt, there is less to say.
        Ink::Stroke plain = stroke;
        plain.pressured = false;
        plain.tilted = false;
        const qsizetype plainBytes = Ink::encode(plain).size();
        QVERIFY2(plainBytes < 300 * 4, qPrintable(QString::number(plainBytes)));
        const auto plainBack = Ink::decode(Ink::encode(plain), plain.id);
        QVERIFY(plainBack.has_value() && !plainBack->pressured && !plainBack->tilted);
        // A dot is a stroke of one point; a colour keeps its alpha.
        Ink::Stroke dot{.id = u"d"_s,
                        .color = QColor(255, 0, 0, 128),
                        .width = 3.0,
                        .pressured = false,
                        .tilted = false,
                        .began = -5,
                        .points = {Ink::Point{.at = QPointF(-3.5, 7.25)}}};
        const auto dotBack = Ink::decode(Ink::encode(dot), u"d"_s);
        QVERIFY(dotBack.has_value());
        QVERIFY(alike(*dotBack, dot));
    }

    // What comes from elsewhere may be anything: cut short, overlong, out
    // of bounds or of a later version, it is an error and no stroke.
    void malformedBytesAreRefused() {
        const QByteArray good = Ink::encode(handwriting(QPointF(0, 0), 20));
        QVERIFY(!Ink::decode(QByteArray(), u"x"_s).has_value());
        for (qsizetype cut = 1; cut < good.size(); ++cut)
            QVERIFY2(!Ink::decode(good.first(cut), u"x"_s).has_value(), qPrintable(QString::number(cut)));
        QVERIFY(!Ink::decode(good + "\x01", u"x"_s).has_value());
        QByteArray later = good;
        later[0] = 2;
        QVERIFY(!Ink::decode(later, u"x"_s).has_value());
        QByteArray flagged = good;
        flagged[1] = static_cast<char>(0x80);
        QVERIFY(!Ink::decode(flagged, u"x"_s).has_value());
        // Ten bytes of continuation are no number.
        QVERIFY(!Ink::decode(QByteArray("\x01\x00\x00\x00\x00\x00", 6) + QByteArray(11, '\xff'), u"x"_s).has_value());
        // A count of points past the most.
        Ink::Stroke huge = line(0, 2.0 * static_cast<double>(Ink::maxPoints) + 100, 0);
        const auto clipped = Ink::decode(Ink::encode(huge), u"h"_s);
        QVERIFY(clipped.has_value());
        QCOMPARE(clipped->points.size(), Ink::maxPoints);
        QVERIFY(!Ink::decodeAll(QByteArray("\x01\xff\xff\xff\x7f", 5)).has_value());
        // A batch with a stroke that can't be read is not taken at all.
        const QJsonObject batch{{u"add"_s, QJsonObject{{u"a"_s, QString::fromLatin1(good.toBase64())}, {u"b"_s, u"AAAA"_s}}},
                                {u"remove"_s, QJsonArray{}}};
        QVERIFY(!Ink::fromBatch(batch).has_value());
    }

    // A sheet's strokes, all at once with their ids: a snapshot.
    void aSnapshotKeepsEveryStrokeAndItsId() {
        std::vector<Ink::Stroke> strokes;
        for (int i = 0; i < 50; ++i)
            strokes.push_back(handwriting(QPointF(i * 3, i * 7), 10 + i, static_cast<quint32>(i)));
        const auto back = Ink::decodeAll(Ink::encodeAll(strokes));
        QVERIFY2(back.has_value(), qPrintable(whyNot(back)));
        QCOMPARE(back->size(), strokes.size());
        for (std::size_t i = 0; i < strokes.size(); ++i)
            QVERIFY(alike(back->at(i), strokes.at(i)));
    }

    // A change as batches the change log takes: one, unless it is over a
    // batch's limits, and read back as the change it was.
    void aChangeGoesAsBatchesWithinTheLogsLimits() {
        Ink::Change change;
        change.removed = {u"old1"_s, u"old2"_s};
        change.added = {handwriting(QPointF(0, 0), 40), handwriting(QPointF(0, 30), 40, 2)};
        const auto one = Ink::batches(change);
        QCOMPARE(one.size(), std::size_t{1});
        const auto back = Ink::fromBatch(one.front());
        QVERIFY2(back.has_value(), qPrintable(whyNot(back)));
        QCOMPARE(back->removed, change.removed);
        QCOMPARE(back->added.size(), std::size_t{2});
        QVERIFY(std::ranges::any_of(back->added, [&](const Ink::Stroke &s) { return alike(s, change.added.front()); }));

        // An erasing of 1500 strokes, and 40 long ones added: more batches, each within the limits.
        Ink::Change large;
        for (int i = 0; i < 1500; ++i)
            large.removed.append(Ink::newId());
        for (int i = 0; i < 40; ++i)
            large.added.push_back(handwriting(QPointF(0, i), 1500, static_cast<quint32>(i)));
        const auto many = Ink::batches(large);
        QVERIFY(many.size() > 2);
        qsizetype removed = 0;
        qsizetype added = 0;
        for (const QJsonObject &batch : many) {
            const qsizetype items = batch.value(u"add"_s).toObject().size() + batch.value(u"remove"_s).toArray().size();
            QVERIFY(items <= Ink::maxBatchItems);
            QVERIFY(QJsonDocument(batch).toJson(QJsonDocument::Compact).size() < 64 * 1024);
            removed += batch.value(u"remove"_s).toArray().size();
            added += batch.value(u"add"_s).toObject().size();
        }
        QCOMPARE(removed, 1500);
        QCOMPARE(added, 40);
    }

    // The pressure makes a line wider, from 0.4 to 1.6 times its width; a
    // line without pressure is its width everywhere. So is what is drawn.
    void pressureWidensTheLine() {
        const Ink::Stroke light = line(0, 100, 50, 0.1F);
        const Ink::Stroke hard = line(0, 100, 50, 0.95F);
        const Ink::Stroke none = line(0, 100, 50);
        QCOMPARE(Ink::widthAt(none, none.points.front()), 4.0);
        QVERIFY(std::abs(Ink::widthAt(light, light.points.front()) - 4.0 * (0.4 + 1.2 * 0.1)) < 1e-6);
        QVERIFY(Ink::widthAt(hard, hard.points.front()) > 2.5 * Ink::widthAt(light, light.points.front()));
        Ink::Mesh mesh;
        Ink::tessellate(light, 2.0, 1.0, mesh);
        const double lightDrawn = thickness(mesh, 200);
        Ink::tessellate(hard, 2.0, 1.0, mesh);
        const double hardDrawn = thickness(mesh, 200);
        Ink::tessellate(none, 2.0, 1.0, mesh);
        const double noneDrawn = thickness(mesh, 200);
        // At twice the scale, with the soft edge a pixel each side.
        QVERIFY2(std::abs(noneDrawn - (8.0 + 1.0)) < 0.2, qPrintable(QString::number(noneDrawn)));
        QVERIFY2(std::abs(hardDrawn - (2.0 * 4.0 * (0.4 + 1.2 * 0.95) + 1.0)) < 0.2, qPrintable(QString::number(hardDrawn)));
        QVERIFY(lightDrawn < noneDrawn && noneDrawn < hardDrawn);
        // Every index is of a vertex there is, and each triangle is whole.
        QCOMPARE(mesh.indices.size() % 3, std::size_t{0});
        QVERIFY(std::ranges::all_of(mesh.indices, [&](quint16 i) { return i < mesh.vertices.size(); }));
        // The longest stroke still fits 16-bit indices.
        Ink::tessellate(handwriting(QPointF(0, 0), static_cast<int>(Ink::maxPoints)), 4.0, 1.0, mesh);
        QVERIFY(mesh.vertices.size() < 65536 && !mesh.indices.empty());
        // A tap is a dot.
        Ink::Stroke dot = line(5, 5, 5);
        Ink::tessellate(dot, 1.0, 1.0, mesh);
        QVERIFY(!mesh.indices.empty());
        QVERIFY2(std::abs(thickness(mesh, 5) - 5.0) < 0.3, qPrintable(QString::number(thickness(mesh, 5))));
    }

    // An eraser over the middle of a line cuts it in two where its edge
    // crosses the line; over its end it shortens it; away from it, nothing.
    void erasingInPartCutsAStroke() {
        const Ink::Stroke stroke = line(0, 200, 100);
        QVERIFY(!Ink::cut(stroke, QPointF(100, 140), 10).has_value());
        const auto pieces = Ink::cut(stroke, QPointF(101, 100), 10);
        QVERIFY(pieces.has_value());
        QCOMPARE(pieces->size(), std::size_t{2});
        // The eraser's radius and half the line's width.
        QVERIFY(std::abs(pieces->at(0).points.back().at.x() - 89.0) < 0.01);
        QVERIFY(std::abs(pieces->at(1).points.front().at.x() - 113.0) < 0.01);
        QCOMPARE(pieces->at(0).points.front().at, QPointF(0, 100));
        QCOMPARE(pieces->at(1).points.back().at, QPointF(200, 100));
        QVERIFY(pieces->at(0).id != stroke.id && pieces->at(1).id != pieces->at(0).id);
        QCOMPARE(pieces->at(1).began, stroke.began);
        const auto shortened = Ink::cut(stroke, QPointF(0, 100), 10);
        QVERIFY(shortened.has_value());
        QCOMPARE(shortened->size(), std::size_t{1});
        QVERIFY(shortened->front().points.front().at.x() > 11.9);
        // Points far apart: the eraser between two of them still cuts.
        Ink::Stroke sparse = line(0, 0, 0);
        sparse.points.push_back(Ink::Point{.at = QPointF(300, 0)});
        const auto split = Ink::cut(sparse, QPointF(150, 3), 5);
        QVERIFY(split.has_value());
        QCOMPARE(split->size(), std::size_t{2});
        // All of it under the eraser: nothing left.
        const auto gone = Ink::cut(line(0, 6, 0), QPointF(3, 0), 20);
        QVERIFY(gone.has_value() && gone->empty());
    }

    // A sheet's edits, each one step of undo; an eraser's sweep is one step,
    // whatever it cut; and undoing says what to undo where they are kept.
    void aSheetUndoesAndRedoesStepByStep() {
        Ink::Sheet sheet;
        const Ink::Stroke first = line(0, 200, 100);
        const Ink::Stroke second = line(0, 200, 300);
        QCOMPARE(sheet.add(first).added.size(), std::size_t{1});
        sheet.add(second);
        QCOMPARE(sheet.strokes().size(), std::size_t{2});

        sheet.beginStep();
        Ink::Change sweep = sheet.erase(QPointF(100, 100), 8, Ink::Erasing::Parts);
        sweep.merge(sheet.erase(QPointF(40, 100), 8, Ink::Erasing::Parts));
        sheet.endStep();
        // The first line in three, as one change: the line removed, three pieces added.
        QCOMPARE(sweep.removed, QStringList{first.id});
        QCOMPARE(sweep.added.size(), std::size_t{3});
        QCOMPARE(sheet.strokes().size(), std::size_t{4});

        const Ink::Change undone = sheet.undo();
        QCOMPARE(undone.added.size(), std::size_t{1});
        QCOMPARE(undone.added.front(), first);
        QCOMPARE(undone.removed.size(), 3);
        QCOMPARE(sheet.strokes().size(), std::size_t{2});
        QVERIFY(sheet.canRedo());
        const Ink::Change redone = sheet.redo();
        QCOMPARE(redone.removed, QStringList{first.id});
        QCOMPARE(sheet.strokes().size(), std::size_t{4});
        sheet.undo();
        const Ink::Change unwritten = sheet.undo();
        QCOMPARE(unwritten.removed, QStringList{second.id});
        QCOMPARE(sheet.strokes().size(), std::size_t{1});
        // A new edit forgets what could be redone.
        sheet.add(line(0, 10, 500));
        QVERIFY(!sheet.canRedo());

        // Whole strokes: the one touched goes, untouched ones stay.
        Ink::Sheet whole;
        whole.add(first);
        whole.add(second);
        const Ink::Change erased = whole.erase(QPointF(50, 302), 3, Ink::Erasing::Strokes);
        QCOMPARE(erased.removed, QStringList{second.id});
        QVERIFY(erased.added.empty());
        QCOMPARE(whole.strokes().size(), std::size_t{1});
        QVERIFY(!whole.clear().empty());
        QVERIFY(whole.strokes().empty());
        whole.undo();
        QCOMPARE(whole.strokes().size(), std::size_t{1});
    }

    // Changes as one batch: a stroke drawn and erased again before the
    // batch went never goes; an erasing undone before it went is no removal.
    void changesMergeIntoOneBatch() {
        Ink::Sheet sheet;
        const Ink::Stroke kept = line(0, 200, 100);
        sheet.add(kept);
        Ink::Change batch; // what went before: the kept line is where it is kept
        const Ink::Stroke scribble = line(0, 50, 300);
        batch.merge(sheet.add(scribble));
        batch.merge(sheet.erase(QPointF(25, 300), 100, Ink::Erasing::Strokes));
        QVERIFY(batch.empty());
        batch.merge(sheet.erase(QPointF(100, 100), 5, Ink::Erasing::Parts));
        QCOMPARE(batch.removed, QStringList{kept.id});
        batch.merge(sheet.undo());
        QVERIFY2(batch.empty(), qPrintable(batch.removed.join(',')));
    }

    // Strokes from elsewhere lie one over the other by when they began.
    void appliedStrokesLieInTheOrderTheyBegan() {
        Ink::Sheet sheet;
        Ink::Stroke late = line(0, 10, 0);
        late.began = 3000;
        Ink::Stroke early = line(0, 10, 5);
        early.began = 1000;
        sheet.add(late);
        sheet.apply(Ink::Change{.added = {early}, .removed = {}});
        QCOMPARE(sheet.strokes().front().id, early.id);
        sheet.apply(Ink::Change{.added = {}, .removed = {late.id}});
        QCOMPARE(sheet.strokes().size(), std::size_t{1});
        sheet.replace({late, early});
        QCOMPARE(sheet.strokes().front().id, early.id);
        QVERIFY(!sheet.canUndo());
    }

    // Pointers down on a sheet: a stroke goes on it as its pointer lifts,
    // one too long goes on as the next, an eraser's sweep is one step of
    // undo, and a pointer dropped (a palm) leaves nothing.
    void pointersWriteAndEraseOnASheet() {
        Ink::Sheet sheet;
        Ink::Writing writing(sheet);
        const Ink::Pointer pen{7, -1};
        const Ink::Pointer finger{9, 1};
        writing.write(pen, Ink::Input{.at = QPointF(0, 10), .pressure = 0.3F, .ms = 1000}, Ink::Nib{.pressured = true}, false);
        for (int x = 1; x <= 100; ++x)
            QVERIFY(writing.move(pen, Ink::Input{.at = QPointF(x, 10), .pressure = 0.8F, .ms = 1000 + 4U * x}).empty());
        QCOMPARE(writing.strokes().size(), std::size_t{1});
        QVERIFY(sheet.strokes().empty());
        // Lifting says no pressure; the line ends as hard as it was.
        const Ink::Change written = writing.lift(pen, Ink::Input{.at = QPointF(101, 10), .pressure = 0.0F, .ms = 1408});
        QCOMPARE(written.added.size(), std::size_t{1});
        QCOMPARE(sheet.strokes().size(), std::size_t{1});
        QCOMPARE(sheet.strokes().front().points.back().pressure, 0.8F);
        QCOMPARE(sheet.strokes().front().points.back().ms, 408U);
        QVERIFY(writing.empty());

        writing.write(finger, Ink::Input{.at = QPointF(0, 50)}, Ink::Nib{}, true);
        writing.move(finger, Ink::Input{.at = QPointF(30, 50)});
        QVERIFY(!writing.toolDown());
        QCOMPARE(writing.dropTouches(), std::vector<Ink::Pointer>{finger});
        QVERIFY(writing.empty() && sheet.strokes().size() == 1);

        // A sweep across the line twice: one step, the line in three.
        Ink::Change swept = writing.erase(pen, Ink::Input{.at = QPointF(30, 0)}, Ink::Rubber{.radius = 3.0}, false);
        swept.merge(writing.move(pen, Ink::Input{.at = QPointF(30, 20)}));
        swept.merge(writing.move(pen, Ink::Input{.at = QPointF(70, 20)}));
        swept.merge(writing.lift(pen, Ink::Input{.at = QPointF(70, 0)}));
        QCOMPARE(swept.removed.size(), 1);
        QCOMPARE(swept.added.size(), std::size_t{3});
        QCOMPARE(sheet.strokes().size(), std::size_t{3});
        sheet.undo();
        QCOMPARE(sheet.strokes().size(), std::size_t{1});

        // A line longer than a stroke holds goes on as the next, from where it got to.
        writing.write(pen, Ink::Input{.at = QPointF(0, 0), .ms = 0}, Ink::Nib{}, false);
        Ink::Change split;
        for (std::size_t i = 1; i < Ink::maxPoints + 10; ++i)
            split.merge(writing.move(pen, Ink::Input{.at = QPointF(static_cast<double>(i), 0), .ms = i}));
        QCOMPARE(split.added.size(), std::size_t{1});
        QCOMPARE(split.added.front().points.size(), Ink::maxPoints);
        const Ink::Stroke *going = writing.strokes().front();
        QCOMPARE(going->points.front().at, split.added.front().points.back().at);
        QVERIFY(going->began > split.added.front().began);
    }

    // The lasso (#129): a path selects the strokes most of whose points it
    // goes around, and what is done to them is one step, a removal of the
    // strokes and the addition of new ones, which undo puts back.
    void theLassoSelectsAndEditsAsOneStep() {
        Ink::Sheet sheet;
        Ink::Lasso lasso(sheet);
        int told = 0;
        lasso.onChange([&told] { ++told; });
        const Ink::Stroke top = line(0, 100, 10);
        const Ink::Stroke middle = line(0, 100, 50);
        const Ink::Stroke across = line(0, 300, 30); // a third of its points within
        for (const Ink::Stroke &stroke : {top, middle, across})
            sheet.add(stroke);
        QVERIFY(Ink::lassoed(top, QPolygonF({{-5, 0}, {120, 0}, {120, 60}, {-5, 60}})));
        QVERIFY(!Ink::lassoed(across, QPolygonF({{-5, 0}, {120, 0}, {120, 60}, {-5, 60}})));
        QVERIFY(!Ink::lassoed(top, QPolygonF({{-5, 0}, {120, 0}})));

        const auto around = [&](QList<QPointF> corners) {
            lasso.press(corners.front(), 4);
            for (const QPointF &corner : corners)
                lasso.drag(corner);
            return lasso.release(corners.front());
        };
        QVERIFY(around({{-5, 0}, {120, 0}, {120, 60}, {-5, 60}}).empty());
        QCOMPARE(QSet<QString>(lasso.selection().begin(), lasso.selection().end()), (QSet<QString>{top.id, middle.id}));
        QVERIFY(told > 0);
        QVERIFY(lasso.box().contains(QRectF(0, 10, 100, 40)));
        QVERIFY(!sheet.canRedo());

        // Dragged from within its box: drawn where it goes, then moved, one step.
        lasso.press(QPointF(50, 30), 4);
        QVERIFY(lasso.busy());
        lasso.drag(QPointF(60, 130));
        QCOMPARE(lasso.moving(top.id)->points.front().at, QPointF(10, 110));
        QCOMPARE(sheet.find(top.id)->points.front().at, QPointF(0, 10));
        const Ink::Change moved = lasso.release(QPointF(60, 130));
        QCOMPARE(QSet<QString>(moved.removed.begin(), moved.removed.end()), (QSet<QString>{top.id, middle.id}));
        QCOMPARE(moved.added.size(), std::size_t{2});
        QVERIFY(lasso.moving(moved.added.front().id) == nullptr);
        QVERIFY(lasso.takeStale().contains(top.id));
        QCOMPARE(sheet.strokes().size(), std::size_t{3});
        QVERIFY(sheet.find(top.id) == nullptr);
        for (const Ink::Stroke &stroke : moved.added) {
            QVERIFY(lasso.selection().contains(stroke.id));
            QCOMPARE(stroke.began, top.began); // it lies where it lay among the others
            QVERIFY(stroke.points.front().at == QPointF(10, 110) || stroke.points.front().at == QPointF(10, 150));
        }
        // Let go where it was taken, it is not moved; a tap elsewhere selects nothing.
        lasso.press(QPointF(50, 130), 4);
        QVERIFY(lasso.release(QPointF(50, 130)).empty());
        QCOMPARE(lasso.selection().size(), 2);

        const Ink::Change recoloured = lasso.recolor(Qt::red);
        QCOMPARE(recoloured.removed.size(), 2);
        QCOMPARE(recoloured.added.front().color, QColor(Qt::red));
        QVERIFY(lasso.recolor(Qt::red).empty());
        const Ink::Change removed = lasso.remove();
        QCOMPARE(removed.removed.size(), 2);
        QVERIFY(removed.added.empty());
        QVERIFY(lasso.selection().isEmpty());
        QCOMPARE(sheet.strokes().size(), std::size_t{1});

        // Undone a step at a time; the selection follows what comes back.
        lasso.follow(sheet.undo());
        QCOMPARE(sheet.strokes().size(), std::size_t{3});
        QCOMPARE(sheet.undo().removed.size(), 2);
        std::ignore = sheet.undo();
        lasso.select({top.id});
        QVERIFY(sheet.find(top.id) != nullptr && sheet.find(middle.id) != nullptr);
        QCOMPARE(sheet.find(top.id)->color, top.color);
        QVERIFY(sheet.canRedo());

        // Pasted: copies with new ids, on top, moved, selected; one step.
        const Ink::Change pasted = lasso.paste(lasso.selected(), QPointF(5, 5));
        QCOMPARE(pasted.added.size(), std::size_t{1});
        QVERIFY(pasted.removed.isEmpty());
        QCOMPARE(sheet.strokes().back().id, pasted.added.front().id);
        QCOMPARE(pasted.added.front().points.front().at, QPointF(5, 15));
        QCOMPARE(lasso.selection(), QStringList{pasted.added.front().id});
        std::ignore = sheet.undo();
        lasso.prune();
        QVERIFY(lasso.selection().isEmpty());
        // A palm while dragging leaves the selection where it was.
        lasso.selectAll();
        lasso.press(QPointF(50, 30), 4);
        lasso.drag(QPointF(80, 30));
        lasso.cancel();
        QVERIFY(!lasso.busy());
        QCOMPARE(sheet.find(top.id)->points.front().at, QPointF(0, 10));
        QCOMPARE(lasso.selection().size(), 3);
    }

    // What the lasso shows: its box, faintly filled, and its path, in the
    // coordinates it is shown in.
    void theLassoShowsItsPathAndBox() {
        Ink::Mesh mesh;
        Ink::marks(QPolygonF({{0, 0}, {10, 0}, {10, 10}}), QRectF(20, 20, 10, 10), Qt::blue, 2.0, 1.0, mesh);
        // The box's fill and four sides, the path's two pieces: four corners each.
        QCOMPARE(mesh.vertices.size(), std::size_t{7 * 4});
        QCOMPARE(mesh.indices.size(), std::size_t{7 * 6});
        const auto [left, right] = std::ranges::minmax(mesh.vertices, {}, &Ink::Vertex::x);
        // The box stands three lines off its strokes: 60 + 3 + half a line.
        QVERIFY(left.x < 0.0F && right.x > 63.0F && right.x < 64.0F);
        QVERIFY(mesh.vertices.front().a < 64); // the fill, faint
        Ink::marks({}, {}, Qt::blue, 1.0, 1.0, mesh);
        QVERIFY(mesh.vertices.empty());
    }

    // Which touches are a palm, as each thing a device may not say is missing.
    void aPalmIsToldByWhatTheDeviceSays() {
        using Ink::Act;
        using Ink::Fingers;
        // Nothing of a pen yet: a finger draws, a palm-sized touch doesn't.
        Ink::InputRules fresh;
        QCOMPARE(fresh.finger(8.0, 1000, Fingers::Auto), Act::Draw);
        QCOMPARE(fresh.finger(30.0, 1000, Fingers::Auto), Act::Palm);
        // No contact size reported (0): a finger, unless the pen is near.
        QCOMPARE(fresh.finger(0.0, 1000, Fingers::Auto), Act::Draw);

        // With proximity: near from entering until a moment after leaving.
        Ink::InputRules near;
        near.proximity(true, 1000);
        QVERIFY(near.penSeen() && near.penNear() && near.proximityKnown());
        QCOMPARE(near.finger(8.0, 5000, Fingers::Auto), Act::Palm);
        near.proximity(false, 6000);
        QCOMPARE(near.finger(8.0, 6300, Fingers::Draw), Act::Palm);
        QCOMPARE(near.finger(8.0, 6600, Fingers::Draw), Act::Draw);
        // Then a finger scrolls: a pen has been seen.
        QCOMPARE(near.finger(8.0, 6600, Fingers::Auto), Act::Pass);
        QCOMPARE(near.finger(8.0, 6600, Fingers::Scroll), Act::Pass);
        // With proximity known, a pen event alone is not nearness.
        near.penEvent(7000);
        QCOMPARE(near.finger(8.0, 7100, Fingers::Draw), Act::Draw);

        // Without proximity: near for a moment after any pen event.
        Ink::InputRules heard;
        heard.penEvent(1000);
        QVERIFY(heard.penSeen() && !heard.proximityKnown() && !heard.penNear());
        QCOMPARE(heard.finger(8.0, 1200, Fingers::Draw), Act::Palm);
        QCOMPARE(heard.finger(8.0, 1600, Fingers::Draw), Act::Draw);
        QCOMPARE(heard.finger(8.0, 1600, Fingers::Auto), Act::Pass);
        QCOMPARE(heard.finger(30.0, 1600, Fingers::Draw), Act::Palm);

        // A pen Qt knew before any event: fingers scroll from the start.
        const Ink::InputRules known(true);
        QCOMPARE(known.finger(8.0, 10, Fingers::Auto), Act::Pass);
        QCOMPARE(known.finger(8.0, 10, Fingers::Draw), Act::Draw);
    }
};

QTEST_GUILESS_MAIN(InkTests)
#include "ink_tests.moc"
