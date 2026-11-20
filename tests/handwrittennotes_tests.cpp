// Handwritten margin notes (#90): a margin note written with the pen is its
// ink, strokes kept on the note in the change log (AnnotationInk), fitted to
// the note's width whatever its card's; written on the text or in the margin
// column, it is on the line there, stands beside it in the column or opens on
// a card, and the pen goes on writing on it, its other end erasing, a click
// giving undo. Once the pen rests the handwriting is read by the AI server's
// vision model (a stand-in here), only one's own unless asked for and
// allowed, and its words are found by the find in the book. Notes travel
// with their ink between one's computers (annotationsync_tests). The pen is
// synthetic tablet events, as a touchscreen laptop's sends them.
//
// OMABOOK_SHOTS=<folder> saves a picture of each state, to look at by eye.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <chrono>

#include "annotationink.h"
#include "desk.h"
#include "fakeaiserver.h"
#include "hyphenation.h"
#include "inksurface.h"
#include "marginnotes.h"
#include "noterecognition.h"

using namespace std::chrono_literals;

namespace {

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &device(bool eraser) {
    static QPointingDevice pen{"pen", 9801, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    static QPointingDevice rubber{
        "pen eraser", 9802, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Eraser, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&pen);
        QWindowSystemInterface::registerInputDevice(&rubber);
        return true;
    }();
    return eraser ? rubber : pen;
}

// Handwriting as a pen draws it: a few strokes side by side, a moment apart.
[[nodiscard]] std::vector<Ink::Stroke> writing(double across = 30) {
    std::vector<Ink::Stroke> strokes;
    for (int letter = 0; letter < 3; ++letter) {
        Ink::Stroke stroke{.id = Ink::newId(),
                           .color = Qt::blue,
                           .width = 2,
                           .pressured = true,
                           .tilted = false,
                           .began = 1'700'000'000'000 + letter * 400,
                           .points = {}};
        for (int i = 0; i < 12; ++i)
            stroke.points.push_back({.at = {100.0 + (letter * across) + (i % 2) * 6, 300.0 + i * 2},
                                     .pressure = 0.6F,
                                     .xTilt = 0,
                                     .yTilt = 0,
                                     .ms = static_cast<quint32>(i * 8)});
        strokes.push_back(stroke);
    }
    return strokes;
}

[[nodiscard]] QRectF boundsOf(const std::vector<Ink::Stroke> &strokes) {
    QRectF box;
    for (const Ink::Stroke &stroke : strokes)
        for (const Ink::Point &point : stroke.points)
            box |= QRectF(point.at, QSizeF(0.01, 0.01));
    return box;
}

[[nodiscard]] QStringList idsOf(const std::vector<Ink::Stroke> &strokes) {
    QStringList ids;
    for (const Ink::Stroke &stroke : strokes)
        ids << stroke.id;
    return ids;
}

// Where words are in a text as the page shows it, its soft hyphens left out of the looking.
[[nodiscard]] std::pair<qsizetype, qsizetype> spanOf(const QString &shown, const QString &words) {
    const Hyphenation::Plain plain(shown);
    const qsizetype at = plain.text().indexOf(words);
    return at < 0 ? std::pair<qsizetype, qsizetype>{-1, -1} : std::pair{plain.toShown(at), plain.toShownEnd(at + words.size())};
}

[[nodiscard]] QRectF sceneRect(QQuickItem *item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

[[nodiscard]] QString textOf(QQuickItem *text) {
    QString words;
    QMetaObject::invokeMethod(text, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0), Q_ARG(int, text->property("length").toInt()));
    return words;
}

// An AI server of one's own, on this computer, that reads what it is sent as `words`.
void serve(Desk &desk, const QString &url) {
    desk.settings->setAiProvider("openai");
    desk.settings->setAiEndpoint({{"preset", "custom"}, {"base_url", url}});
    desk.settings->setRecognition({{"ink_model", "llava"}});
}

// How often the server was asked to read handwriting: the window asks it other things too (its models).
[[nodiscard]] qsizetype readings(const FakeServer &server) {
    return std::ranges::count_if(server.received, [](const FakeServer::Request &one) { return one.line.contains("chat/completions"); });
}

// What the server answers, to each request a dozen times over: these words.
void answer(FakeServer &server, const QString &words) {
    server.replies.clear();
    for (int time = 0; time < 12; ++time)
        server.read(words);
}

// The window with the chronicle open, a pen to write with.
struct Bench {
    Desk desk;
    QList<qint64> ids;
    ulong clock = 900'000;

    explicit Bench(const QByteArray &language = "en", const QByteArray &colors = {}, bool touch = false) : desk(language, colors, touch) {
        ids = stock(desk);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
    }
    bool open(const QString &layout) {
        desk.reader->format()->set("layout", layout, false);
        desk.reader->openBook(ids.first());
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.shows("flowText"); }, 5000))
            return false;
        desk.settle();
        return true;
    }
    [[nodiscard]] MarginNotes *notes() const { return desk.find("readingNotes")->property("notes").value<MarginNotes *>(); }
    [[nodiscard]] QQuickItem *text() const { return desk.item("flowText"); }
    [[nodiscard]] Library &library() { return *desk.backend->library(); }
    [[nodiscard]] QList<Annotation> made() { return library().annotations(ids.first(), "margin-note"); }
    [[nodiscard]] std::vector<Ink::Stroke> inkOf(qint64 id) { return AnnotationInk::strokes(library(), id); }
    // The middle of a character of the page, in the window.
    [[nodiscard]] QPointF at(const QString &words, int into = 3) const {
        const qsizetype start = spanOf(textOf(text()), words).first;
        QRectF rect;
        QMetaObject::invokeMethod(text(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, static_cast<int>(start) + into));
        return text()->mapToScene(rect.center());
    }
    void pen(QPointF at, bool down, bool eraser = false) {
        clock += 5;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &device(eraser), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.6 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    void stroke(const QList<QPointF> &points, bool eraser = false) {
        for (const QPointF &point : points)
            pen(point, true, eraser);
        pen(points.last(), false, eraser);
        QTest::qWait(desk.item("penGestures")->property("batchPause").toInt() + 120);
    }
    // A word written by hand: up and down, as letters go, nothing a highlight's gesture.
    static QList<QPointF> word(QPointF at) {
        const QList<QPointF> turns{at, at + QPointF(6, -10), at + QPointF(12, 0), at + QPointF(18, -10), at + QPointF(4, 6)};
        QList<QPointF> points;
        for (qsizetype i = 1; i < turns.size(); ++i)
            for (int step = 0; step < 6; ++step)
                points << turns.at(i - 1) + ((turns.at(i) - turns.at(i - 1)) * (step / 6.0));
        points << turns.last();
        return points;
    }
    // A point of a note's ink in the window, from ink units.
    [[nodiscard]] static QPointF onInk(QQuickItem *surface, QPointF inUnits) {
        return surface->mapToScene(inUnits * surface->property("inkScale").toDouble());
    }
};

[[nodiscard]] QQuickItem *inkIn(QQuickItem *card) { return card ? card->findChild<QQuickItem *>("noteInk") : nullptr; }

} // namespace

class HandwrittenNotesTests : public QObject {
    Q_OBJECT

    QStringList found;
    int looked = 0;

    void look(Desk &desk, const QString &state) {
        desk.settle(1);
        Survey survey{desk.window, desk.touch, {}};
        survey.walk();
        ++looked;
        const QString where = QString("%1%2 %3x%4 %5")
                                  .arg(QString::fromLatin1(desk.language), desk.touch ? " touch" : "")
                                  .arg(desk.window->width())
                                  .arg(desk.window->height())
                                  .arg(state);
        for (const QString &problem : std::as_const(survey.problems))
            if (!found.contains(where + " — " + problem))
                found << where + " — " + problem;
        desk.shot(QString("handwritten-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // A note by hand is its ink, kept on it in the change log and fitted to
    // the note's width: it has no words until they are read, keeps none
    // given it, loses a stroke erased, and goes with its last.
    void theInkIsTheNote() {
        Bench bench;
        QVERIFY(bench.open("scroll"));
        MarginNotes notes;
        notes.setReader(&*bench.desk.reader);
        const QString shown = textOf(bench.text());
        const auto [start, end] = spanOf(shown, "Paragraph 2 of a long chapter");
        QVERIFY(start >= 0);
        QVERIFY(!notes.writeInk(shown, start, end, {}, 0.5));
        const std::optional<qint64> id = notes.writeInk(shown, start, end, writing(), 0.5);
        QVERIFY(id);
        QCOMPARE(bench.made().size(), 1);
        QCOMPARE(bench.made().first().note, QString());
        QCOMPARE(bench.made().first().quote.simplified(), QString("Paragraph 2 of a long chapter"));
        const std::vector<Ink::Stroke> kept = bench.inkOf(*id);
        QCOMPARE(kept.size(), size_t{3});
        // Moved to the note's corner, at half the size it was written.
        const QRectF box = boundsOf(kept);
        QVERIFY2(box.left() >= 0 && box.top() >= 0 && box.left() < 12 && box.top() < 12, qPrintable(QString::number(box.left())));
        QVERIFY(std::abs(box.width() - (boundsOf(writing()).width() * 0.5)) < 0.5);
        QVERIFY(notes.notes().first().toMap().value("ink").toBool());
        QCOMPARE(notes.spansIn(shown).size(), 1);
        QVERIFY(notes.strokes(*id).size() == 3);
        // Written wider than a note, it is made to fit.
        const std::optional<qint64> wide = notes.writeInk(shown, start, end, writing(400), 1);
        QVERIFY(wide);
        QVERIFY(boundsOf(bench.inkOf(*wide)).right() <= MarginNotes::inkWidth);
        notes.remove(*wide);

        // No words given it: still there, its ink the note.
        QVERIFY(notes.change(*id, "  "));
        QCOMPARE(notes.count(), 1);
        QVERIFY(notes.ink(*id, Ink::Change{.added = {}, .removed = {kept.front().id}}));
        QCOMPARE(idsOf(bench.inkOf(*id)), idsOf(kept).mid(1));
        QVERIFY(notes.ink(*id, Ink::Change{.added = {}, .removed = idsOf(kept).mid(1)}));
        QVERIFY(bench.made().isEmpty());
        QCOMPARE(notes.count(), 0);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Once the pen rests the handwriting is read by one's own AI server and
    // becomes the note's words, which the find in the book finds; ink
    // written anew is read anew. A hosted server is never sent it unasked,
    // even where the settings allow hosted ones.
    void theHandwritingIsReadOnOwnServersAndFound() {
        Bench bench;
        QVERIFY(bench.open("scroll"));
        FakeServer server;
        serve(bench.desk, server.url());
        MarginNotes notes;
        notes.setReader(&*bench.desk.reader);
        notes.setSettings(&*bench.desk.settings);
        QVERIFY(notes.recognition());
        notes.recognition()->setPause(10ms);
        const QString shown = textOf(bench.text());
        const auto [start, end] = spanOf(shown, "Paragraph 3 of a long chapter");
        answer(server, "Ferry\nlights");
        const std::optional<qint64> id = notes.writeInk(shown, start, end, writing(), 0.5);
        QVERIFY(id);
        QTRY_VERIFY2_WITH_TIMEOUT(bench.made().first().note == "Ferry\nlights", qPrintable(notes.readingOf(*id)), 10000);
        QCOMPARE(readings(server), 1);
        QCOMPARE(server.received.last().json().value("model").toString(), QString("llava"));
        QCOMPARE(notes.readingOf(*id), QString());
        QCOMPARE(notes.found("FERRY").size(), 1);
        QCOMPARE(notes.found("ferry").first().toMap().value("note").toLongLong(), *id);
        QVERIFY(notes.found("harbour").isEmpty());
        // A stroke more: read again.
        answer(server, "Ferry lights out");
        std::vector<Ink::Stroke> more = writing(12);
        QVERIFY(notes.ink(*id, Ink::Change{.added = {more.front()}, .removed = {}}));
        QTRY_COMPARE_WITH_TIMEOUT(bench.made().first().note, QString("Ferry lights out"), 10000);
        QCOMPARE(readings(server), 2);

        // Someone else's server: not sent, and said why.
        bench.desk.settings->setAiEndpoint({{"preset", "custom"}, {"base_url", "https://api.example.com/v1"}});
        bench.desk.settings->setRecognitionHosted(true);
        const std::optional<qint64> other = notes.writeInk(shown, start, end, writing(20), 0.5);
        QVERIFY(other);
        QTRY_VERIFY_WITH_TIMEOUT(!notes.readingOf(*other).isEmpty(), 5000);
        QVERIFY2(notes.readingOf(*other).contains("only where you chose it"), qPrintable(notes.readingOf(*other)));
        QCOMPARE(readings(server), 2);
        QVERIFY(bench.library().annotations(bench.ids.first(), "margin-note").last().note.isEmpty());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // In the window, with the pen: a word written over the text is a note
    // on its line, standing beside it in the margin column, where the pen
    // writes on it again, its other end erases and a click gives undo; one
    // written in the column is on the line beside it. Read, it is found by
    // the find in the book and its words shown on request. Without room for
    // the column the note opens on a card beside its line. A line under
    // words is still a highlight.
    void thePenWritesNotesBesideTheirLines() {
        Bench bench;
        QVERIFY(bench.desk.resize(QSize(1600, 900)));
        bench.desk.settings->setPages(1);
        FakeServer server;
        serve(bench.desk, server.url());
        QVERIFY(bench.open("scroll"));
        QVERIFY(bench.notes()->recognition());
        bench.notes()->recognition()->setPause(50ms);
        // Read again after each pause of the pen, the same words each time.
        answer(server, "Ferry lights");

        bench.stroke(Bench::word(bench.at("Paragraph 2 of a long chapter")));
        QTRY_COMPARE(bench.notes()->count(), 1);
        const qint64 id = bench.made().first().id;
        QVERIFY2(bench.made().first().quote.contains("Paragraph 2"), qPrintable(bench.made().first().quote));
        QCOMPARE(bench.inkOf(id).size(), size_t{1});
        QTRY_VERIFY(bench.desk.reader->format()->noteWidth() > 0);
        QQuickItem *card = nullptr;
        QTRY_VERIFY((card = bench.desk.item(QString("marginNote_%1").arg(id))) && card->isVisible());
        QQuickItem *surface = inkIn(card);
        QVERIFY(surface);
        QTRY_COMPARE(surface->property("count").toInt(), 1);
        // Beside its line.
        QVERIFY(std::abs(sceneRect(card).top() - bench.at("Paragraph 2 of a long chapter").y()) < 30);
        bench.desk.shot("handwritten-pen-written");

        // The pen on the note writes on it.
        bench.stroke(Bench::word(Bench::onInk(surface, {60, 30})));
        QTRY_COMPARE(bench.inkOf(id).size(), size_t{2});
        QCOMPARE(bench.notes()->count(), 1);
        // Its other end erases the first stroke.
        const Ink::Stroke first = bench.inkOf(id).front();
        QList<QPointF> rubbed;
        for (const Ink::Point &point : first.points)
            rubbed << Bench::onInk(surface, point.at);
        bench.stroke(rubbed, true);
        QTRY_VERIFY(!idsOf(bench.inkOf(id)).contains(first.id));
        // A click on it, and Ctrl+Z: back.
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, sceneRect(card).center().toPoint());
        QTRY_VERIFY(card->property("editing").toBool());
        QVERIFY(bench.desk.shows("noteInkUndo"));
        bench.desk.shot("handwritten-editing");
        QTest::keyClick(bench.desk.window, Qt::Key_Z, Qt::ControlModifier);
        QTRY_VERIFY(idsOf(bench.inkOf(id)).contains(first.id));

        // Read once the pen rested; W shows its words, the find finds them.
        QTRY_COMPARE_WITH_TIMEOUT(bench.made().first().note, QString("Ferry lights"), 10000);
        QTest::keyClick(bench.desk.window, Qt::Key_W);
        QTRY_VERIFY(bench.desk.shows("noteInkWords"));
        QCOMPARE(bench.desk.item("noteInkWords")->property("text").toString(), QString("Ferry lights"));
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!card->property("editing").toBool());
        QObject *finder = bench.desk.find("finder");
        QMetaObject::invokeMethod(finder, "findWords", Q_ARG(QVariant, QVariant("ferry")));
        QTRY_VERIFY(!finder->property("results").toList().isEmpty());
        QCOMPARE(finder->property("results").toList().first().toMap().value("note").toLongLong(), id);
        QVERIFY(bench.desk.closePopups());

        // Written in the column beside a line: a note on that line.
        const QPointF beside(sceneRect(card).center().x(), bench.at("Paragraph 5 of a long chapter").y());
        bench.stroke(Bench::word(beside));
        QTRY_COMPARE(bench.notes()->count(), 2);
        QVERIFY(std::ranges::any_of(bench.made(), [](const Annotation &note) { return note.quote.contains("Paragraph 5"); }));

        // Narrow: no column, the note opens on a card beside its line.
        QVERIFY(bench.desk.resize(QSize(560, 800)));
        QTRY_COMPARE(bench.desk.reader->format()->noteWidth(), 0.0);
        bench.desk.settle();
        bench.stroke(Bench::word(bench.at("Paragraph 3 of a long chapter")));
        QTRY_COMPARE(bench.notes()->count(), 3);
        QTRY_VERIFY(bench.desk.shows("noteCard"));
        QQuickItem *onCard = inkIn(bench.desk.item("noteCard"));
        QVERIFY(onCard && onCard->isVisible());
        QTRY_COMPARE(onCard->property("count").toInt(), 1);
        bench.desk.shot("handwritten-card");
        // The pen goes on writing on the card.
        bench.stroke(Bench::word(Bench::onInk(onCard, {80, 30})));
        const qint64 third = bench.desk.find("noteCard")->property("noteId").toLongLong();
        QTRY_COMPARE(bench.inkOf(third).size(), size_t{2});
        QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!bench.desk.shows("noteCard"));

        // A line under words: a highlight, no note.
        const QPointF from = bench.at("Paragraph 4 of a long chapter", 0), to = bench.at("Paragraph 4 of a long chapter", 20);
        QList<QPointF> under;
        for (int i = 0; i <= 20; ++i)
            under << from + ((to - from) * (i / 20.0)) + QPointF(0, 9);
        bench.stroke(under);
        QTRY_COMPARE(bench.library().annotations(bench.ids.first(), "highlight").size(), 1);
        QCOMPARE(bench.notes()->count(), 3);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
    }

    // Handwritten notes, edited, on a card and among the lost ones, at every
    // size, in English, German, a right-to-left language and under fingers.
    void handwrittenNotesAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Bench bench(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            if (bench.desk.window->minimumWidth() > 420 || bench.desk.window->minimumHeight() > 360)
                bench.desk.window->setMinimumSize(QSize(420, 360));
            bench.desk.quicken();
            QVERIFY(bench.desk.resize(QSize(1440, 900)));
            QVERIFY(bench.open("auto"));
            MarginNotes *notes = bench.notes();
            const QString shown = textOf(bench.text());
            for (const char *words : {"Paragraph 1 of a long", "Paragraph 2 of a long"}) {
                const auto [start, end] = spanOf(shown, words);
                QVERIFY(notes->writeInk(shown, start, end, writing(), 0.5));
            }
            const std::optional<qint64> lost = notes->writeInk(shown, spanOf(shown, "Paragraph 3 of a long").first,
                                                               spanOf(shown, "Paragraph 3 of a long").second, writing(), 0.5);
            QVERIFY(lost);
            Annotation gone = bench.made().last();
            gone.lost = true;
            QVERIFY(bench.library().storeAnnotation(bench.ids.first(), gone));
            notes->refresh();
            for (const QSize &size : sizes()) {
                QVERIFY(bench.desk.resize(size));
                look(bench.desk, "notes");
                const qint64 first = notes->notes().first().toMap().value("id").toLongLong();
                if (bench.desk.reader->format()->noteWidth() > 0) {
                    QQuickItem *card = bench.desk.item(QString("marginNote_%1").arg(first));
                    if (card && card->isVisible()) {
                        QMetaObject::invokeMethod(card, "edit");
                        look(bench.desk, "editing");
                        QMetaObject::invokeMethod(card, "finish");
                        bench.desk.settle();
                    }
                    continue;
                }
                QQuickItem *mark = bench.desk.item(QString("noteMark_%1").arg(first));
                if (mark && mark->isVisible()) {
                    QTest::mouseClick(bench.desk.window, Qt::LeftButton, Qt::NoModifier, sceneRect(mark).center().toPoint());
                    if (QTest::qWaitFor([&] { return bench.desk.shows("noteCard"); }, 1000))
                        look(bench.desk, "card");
                    QTest::keyClick(bench.desk.window, Qt::Key_Escape);
                    bench.desk.settle();
                }
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(bench.desk.resize(size));
                QMetaObject::invokeMethod(bench.desk.find("lostNotes"), "clicked");
                QVERIFY(bench.desk.waitForPopup());
                look(bench.desk, "lost");
                QVERIFY(bench.desk.closePopups());
            }
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));
        }
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(HandwrittenNotesTests)
#include "handwrittennotes_tests.moc"
