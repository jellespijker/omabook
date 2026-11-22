// The margin folded away (#167): Shift+E, the small button at the margin's
// edge and the Aa panel fold the margin column away for every book, kept
// across books and restarts, and the text takes its room; a dot beside each
// line with a margin note, someone else's note or ink in the margin stands
// for them, one with a count where a line has several, named for a screen
// reader by its note's first words. Enter on a dot (Tab goes to it), a
// click, a finger's tap and the pen's open the line's note on its card
// beside the words; Escape closes it, the margin still folded. Where the
// window has no room for the column at all (#157) the same dots show: large
// type in a laptop's window, under a finger. And the survey at every size,
// in German, right to left and under fingers.
//
// OMABOOK_SHOTS=<folder> saves a picture of each state, to look at by eye.
// Offscreen pictures don't draw the ink; the dots and the cards are what to look at.

#include <QPointingDevice>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>
#include <qpa/qwindowsysteminterface_p.h>

#include <functional>

#include "desk.h"
#include "inksurface.h"
#include "lineink.h"
#include "marginnotes.h"
#include "pageformat.h"
#include "readingink.h"

namespace {

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did, and Mira watched the lantern on the far pier.";

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 9671, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

QPointingDevice *finger() {
    static QPointingDevice *device = QTest::createTouchDevice();
    return device;
}

[[nodiscard]] QPointF middleOf(const QQuickItem *item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)); }

// The reading format the person reads with: large type, long lines, narrow margins.
void largeType(Desk &desk) {
    desk.reader->format()->set("font_size", 34, false);
    desk.reader->format()->set("measure", 83, false);
    desk.reader->format()->set("margins", 24, false);
}

// The window with a book of the story open, as a column that scrolls unless told.
struct Bench {
    Desk desk;
    QList<qint64> books;
    mutable ulong clock = 700'000;
    mutable int contact = 0;

    explicit Bench(const QByteArray &language = "en", bool touch = false, QSize size = QSize(1600, 900), const QByteArray &colors = {})
        : desk(language, colors, touch) {
        QString body;
        for (int i = 0; i < 8; ++i)
            body += "<p>" + story + "</p>";
        for (const QString title : {"The Ferry", "The Lantern"}) {
            const QString file = desk.dir.filePath(title + ".epub");
            QVERIFY(Fixtures::chapteredEpub(file, title, {{"The Crossing", body}, {"The Return", body}}));
            books << desk.backend->library()->add(file).id;
        }
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(size));
        desk.settings->setPages(1);
        QVERIFY(open(books.first()));
    }
    bool open(qint64 book, const QString &layout = "scroll") {
        desk.reader->format()->set("layout", layout, false);
        desk.reader->openBook(book);
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && text() && text()->width() > 0; }, 5000))
            return false;
        desk.settle();
        return true;
    }
    [[nodiscard]] QQuickItem *content() const { return desk.find("contentLoader")->property("item").value<QQuickItem *>(); }
    [[nodiscard]] QQuickItem *text() const { return desk.item("flowText"); }
    [[nodiscard]] MarginNotes *notes() const { return desk.find("readingNotes")->property("notes").value<MarginNotes *>(); }
    [[nodiscard]] PageFormat *format() const { return desk.reader->format(); }
    [[nodiscard]] bool cardShown() const { return desk.find("noteCard")->property("shown").toBool(); }
    [[nodiscard]] QString shownText() const {
        QString shown;
        QMetaObject::invokeMethod(text(), "getText", Q_RETURN_ARG(QString, shown), Q_ARG(int, 0),
                                  Q_ARG(int, text()->property("length").toInt()));
        return shown;
    }
    [[nodiscard]] int find(const QString &words, int occurrence = 0) const {
        const QString shown = shownText();
        qsizetype at = -1;
        for (int i = 0; i <= occurrence; ++i)
            at = shown.indexOf(words, at + 1);
        return static_cast<int>(at);
    }
    // A margin note on these words, the `occurrence`th time the page shows them.
    qint64 note(const QString &words, const QString &note, int occurrence = 0) {
        auto *document = text()->property("textDocument").value<QQuickTextDocument *>();
        const int start = find(words, occurrence);
        return start < 0 ? 0 : notes()->write(document, start, start + static_cast<int>(words.size()), note);
    }
    [[nodiscard]] QRectF lineOf(int position) const {
        QRectF rect;
        QMetaObject::invokeMethod(text(), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return text()->mapRectToScene(rect);
    }
    // The dots shown, top to bottom.
    [[nodiscard]] QList<QQuickItem *> dots() const {
        QList<QQuickItem *> found;
        QList<QQuickItem *> items{desk.window->contentItem()};
        while (!items.isEmpty()) {
            QQuickItem *item = items.takeFirst();
            if (!item->isVisible())
                continue;
            if (item->objectName().startsWith("marginDot_"))
                found << item;
            items << item->childItems();
        }
        std::ranges::sort(found, {}, [](const QQuickItem *dot) { return dot->mapToScene(QPointF(0, 0)).y(); });
        return found;
    }
    // What a screen reader calls it (its Accessible.name).
    [[nodiscard]] static QString nameOf(const QQuickItem *dot) { return dot->property("said").toString(); }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = {}) const { QTest::keyClick(desk.window, key, modifiers); }

    // A finger, as the window system sends it.
    void touch(QEventPoint::State state, QPointF at) const {
        if (state == QEventPoint::State::Pressed)
            ++contact;
        QWindowSystemInterface::TouchPoint point;
        point.id = contact;
        point.state = state;
        point.pressure = state == QEventPoint::State::Released ? 0.0 : 1.0;
        const QPointF global = desk.window->mapToGlobal(at);
        point.area = QRectF(0, 0, 8, 8);
        point.area.moveCenter(global);
        const QRect screen = desk.window->screen()->geometry();
        point.normalPosition = QPointF(global.x() / screen.width(), global.y() / screen.height());
        clock += 16;
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(desk.window, clock, finger(), {point});
    }
    void tap(QPointF at) const {
        touch(QEventPoint::State::Pressed, at);
        QTest::qWait(60);
        clock += 60;
        touch(QEventPoint::State::Released, at);
    }
    void penAt(QPointF at, bool down) const {
        clock += 8;
        QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), at, desk.window->mapToGlobal(at),
                                                  down ? Qt::LeftButton : Qt::NoButton, down ? 0.5 : 0.0, 0, 0, 0, 0, 0);
        QWindowSystemInterface::flushWindowSystemEvents();
    }
    void penTap(QPointF at) const {
        penAt(at, false);
        penAt(at, true);
        penAt(at, false);
    }
    // Ink in the margin where the lines start, beside the line of a character, by the pen with D on.
    void inkBeside(int position) {
        const QRectF line = lineOf(position);
        const double edge = text()->mapToScene(QPointF(0, 0)).x();
        key(Qt::Key_D);
        QTRY_VERIFY(desk.reader->ink()->inking());
        auto *surface = qobject_cast<InkSurface *>(desk.item("lineInk"));
        QVERIFY(surface);
        const QPointF from(edge - 30, line.center().y() - 4), to(edge - 10, line.center().y() + 4);
        for (int i = 0; i <= 12; ++i)
            penAt(from + ((to - from) * (i / 12.0)), true);
        penAt(to, false);
        QTest::qWait(surface->batchPause() + 150);
        key(Qt::Key_D);
        QTRY_VERIFY(!desk.reader->ink()->inking());
    }
};

[[nodiscard]] QString warned(const Desk &desk) { return desk.warnings.join('\n'); }

} // namespace

class MarginFoldTests : public QObject {
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
        desk.shot(QString("marginfold-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

private slots:
    void initTestCase() {
        QQuickStyle::setStyle("Material");
        // As on Wayland: the pen's events and no mouse's with them.
        QWindowSystemInterfacePrivate::TabletEvent::setPlatformSynthesizesMouse(false);
    }

    // Shift+E folds the column away and the text takes its room; the button
    // at the margin's edge and the Aa panel fold and unfold it too, and it
    // stays as it was left in another book and after a restart.
    void foldingGivesTheTextTheColumnsRoom() {
        Bench bench;
        bench.desk.reader->format()->set("font_size", 24, false);
        bench.desk.reader->format()->set("measure", 83, false);
        bench.desk.reader->format()->set("margins", 24, false);
        QVERIFY(bench.note("ferry came back", "Where it docks"));
        // A window where the column fits and leaves the lines shorter than the measure.
        bool squeezed = false;
        for (int width = 1200; width <= 2600 && !squeezed; width += 40) {
            QVERIFY(bench.desk.resize(QSize(width, 900)));
            squeezed = bench.format()->noteWidth() > 0 && bench.text()->width() < bench.format()->lineWidth() - 40;
        }
        QVERIFY2(squeezed, "no width has the column and short lines");
        QTRY_VERIFY(bench.desk.shows("marginColumn"));
        QVERIFY(bench.desk.shows("marginFold"));
        QVERIFY(!bench.desk.shows("marginDots"));
        const double beside = bench.text()->width();

        bench.content()->forceActiveFocus();
        bench.key(Qt::Key_E, Qt::ShiftModifier);
        QTRY_VERIFY(bench.format()->marginFolded());
        QVERIFY(bench.desk.settings->marginFolded());
        QCOMPARE(bench.format()->noteWidth(), 0.0);
        QTRY_VERIFY(!bench.desk.shows("marginColumn"));
        // As long as the measure, or as wide as the room left once the column is gone.
        const double room = bench.content()->width() - (2 * bench.content()->property("side").toDouble());
        const double wanted = std::min(bench.format()->lineWidth(), room);
        QTRY_VERIFY2(
            bench.text()->width() > beside + 40 && std::abs(bench.text()->width() - std::max(200.0, wanted)) < 2,
            qPrintable(QString("%1 wide folded, %2 beside the column, %3 wanted").arg(bench.text()->width()).arg(beside).arg(wanted)));
        QTRY_COMPARE(bench.dots().size(), 1);
        QVERIFY(bench.desk.shows("marginFold"));

        // The button at the margin's edge unfolds it, and folds it again.
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middleOf(bench.desk.item("marginFold")).toPoint());
        QTRY_VERIFY(!bench.format()->marginFolded());
        QTRY_VERIFY(bench.desk.shows("marginColumn"));
        QTRY_COMPARE(bench.text()->width(), beside);
        QTRY_VERIFY(bench.dots().isEmpty());
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middleOf(bench.desk.item("marginFold")).toPoint());
        QTRY_VERIFY(bench.format()->marginFolded());

        // The Aa panel's switch says so, and unfolds it.
        bench.key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(bench.desk.shows("formatMarginFolded"));
        QQuickItem *folded = bench.desk.item("formatMarginFolded");
        QVERIFY(folded->property("checked").toBool());
        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middleOf(folded).toPoint());
        QTRY_VERIFY(!bench.desk.settings->marginFolded());
        QVERIFY(!bench.format()->marginFolded());
        // The panel put away, the key folds it, and the panel says so when it is shown again.
        bench.key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(!bench.desk.shows("formatMarginFolded"));
        QTRY_VERIFY(bench.desk.shows("marginColumn"));
        bench.content()->forceActiveFocus();
        bench.key(Qt::Key_E, Qt::ShiftModifier);
        QTRY_VERIFY(bench.desk.settings->marginFolded());
        bench.key(Qt::Key_A, Qt::ShiftModifier);
        QTRY_VERIFY(bench.desk.shows("formatMarginFolded"));
        QVERIFY(bench.desk.item("formatMarginFolded")->property("checked").toBool());

        // Another book, and the settings read again as after a restart.
        QVERIFY(bench.open(bench.books.last()));
        QVERIFY(bench.desk.settings->marginFolded());
        QVERIFY(Settings(bench.desk.dir.filePath("config.json")).marginFolded());
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // A dot beside each line with something on it: one with a count where
    // a line has two notes, and one for ink in the margin; each beside its
    // line, in the note's colour, named by the note's first words.
    void aDotBesideEachNotedLine() {
        Bench bench;
        bench.desk.settings->setMarginFolded(true);
        const qint64 first = bench.note("late in the autumn", "The season matters later in the book");
        QVERIFY(bench.note("ferry came back", "Where it docks"));
        QCOMPARE(bench.lineOf(bench.find("late in")).y(), bench.lineOf(bench.find("ferry came")).y());
        const qint64 alone = bench.note("harbour master counted", "He counts twice", 2);
        QVERIFY(alone);
        bench.inkBeside(bench.find("Mira watched", 4));
        QTRY_COMPARE(LineInk::lines(*bench.desk.backend->library(), bench.books.first()).size(), 1);
        QTRY_COMPARE(bench.dots().size(), 3);
        const QList<QQuickItem *> dots = bench.dots();
        QCOMPARE(dots.at(0)->objectName(), QString("marginDot_%1").arg(first));
        QCOMPARE(dots.at(0)->property("count").toInt(), 2);
        QVERIFY2(Bench::nameOf(dots.at(0)).contains("Note: The season matters later in the"), qPrintable(Bench::nameOf(dots.at(0))));
        QVERIFY2(Bench::nameOf(dots.at(0)).contains("1 more"), qPrintable(Bench::nameOf(dots.at(0))));
        QCOMPARE(dots.at(1)->objectName(), QString("marginDot_%1").arg(alone));
        QCOMPARE(Bench::nameOf(dots.at(1)), QString("Note: He counts twice"));
        QCOMPARE(dots.at(2)->objectName(), QString("marginDot_ink"));
        QCOMPARE(Bench::nameOf(dots.at(2)), QString("Ink in the margin"));
        // Beside its line, at the end the lines end at, clear of the text.
        const double end = bench.text()->mapToScene(QPointF(bench.text()->width(), 0)).x();
        for (const auto &[dot, position] :
             {std::pair{dots.at(0), bench.find("late in")}, std::pair{dots.at(1), bench.find("harbour master", 2)}}) {
            QVERIFY2(std::abs(middleOf(dot).y() - bench.lineOf(position).center().y()) < 2, qPrintable(dot->objectName()));
            QVERIFY2(middleOf(dot).x() > end, qPrintable(dot->objectName()));
        }
        // In the theme's accent, as one's own notes are marked.
        const auto *mark = dots.at(1)->childItems().at(1);
        QCOMPARE(mark->property("color").value<QColor>(), bench.desk.look->property("accent").value<QColor>());
        // A note removed takes its dot with it.
        bench.notes()->remove(alone);
        QTRY_COMPARE(bench.dots().size(), 2);
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
    }

    // Tab goes to a dot and Enter opens its note on its card; Escape closes
    // it, the margin still folded. A click, a finger's tap and the pen's
    // open it too, and a tap beside it closes it.
    void aDotOpensItsNoteByKeyFingerAndPen() {
        for (const bool touch : {false, true}) {
            Bench bench("en", touch);
            bench.desk.settings->setMarginFolded(true);
            const qint64 id = bench.note("harbour master counted", "He counts twice");
            QTRY_COMPARE(bench.dots().size(), 1);
            QQuickItem *dot = bench.dots().first();
            QVERIFY(std::min(dot->width(), dot->height()) >= (touch ? Survey::fingertip : 24) - 0.5);

            bench.content()->forceActiveFocus();
            for (int press = 0; press < 40 && !dot->hasActiveFocus(); ++press)
                bench.key(Qt::Key_Tab);
            QVERIFY2(dot->hasActiveFocus(), "Tab never reached the dot");
            bench.key(Qt::Key_Return);
            QTRY_VERIFY(bench.cardShown());
            QCOMPARE(bench.desk.find("noteCard")->property("noteId").toLongLong(), id);
            bench.key(Qt::Key_Escape);
            QTRY_VERIFY(!bench.cardShown());
            QVERIFY(bench.format()->marginFolded());
            QCOMPARE(bench.notes()->notes().first().toMap().value("note").toString(), QString("He counts twice"));

            const auto opens = [&](const char *how, const std::function<void(QPointF)> &press) {
                bench.desk.settle();
                QQuickItem *now = bench.dots().first();
                const double top = bench.content()->property("contentY").toDouble();
                press(middleOf(now));
                QTRY_VERIFY2(bench.cardShown(), how);
                QCOMPARE(bench.desk.find("noteCard")->property("noteId").toLongLong(), id);
                QCOMPARE(bench.content()->property("contentY").toDouble(), top);
                bench.desk.shot(QString("marginfold-opened-%1").arg(how));
                bench.key(Qt::Key_Escape);
                QTRY_VERIFY2(!bench.cardShown(), how);
                QVERIFY2(bench.desk.reader->isOpen(), how);
            };
            if (touch) {
                opens("finger", [&](QPointF at) { bench.tap(at); });
                opens("pen", [&](QPointF at) { bench.penTap(at); });
                // A tap beside the card closes it; long after the pen, so the finger is no palm.
                bench.clock += 3000;
                QTest::qWait(50);
                bench.tap(middleOf(bench.dots().first()));
                QTRY_VERIFY(bench.cardShown());
                bench.desk.settle();
                const QQuickItem *card = bench.desk.item("noteCard");
                const QPointF away = card->mapToScene(QPointF(card->width() / 2, card->height() + 120));
                bench.tap(away);
                QTRY_VERIFY(!bench.cardShown());
            } else {
                opens("mouse", [&](QPointF at) { QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, at.toPoint()); });
            }
            QVERIFY(bench.format()->marginFolded());
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
        }
    }

    // Large type in a laptop's window (#157): no room for the column, folded
    // or not, so no button; the same dots stand beside the lines, as a
    // column that scrolls and as pages, and a finger opens a note from one.
    void dotsWhereTheColumnHasNoRoom() {
        for (const QString layout : {"scroll", "pages"}) {
            Bench bench("en", true, QSize(1512, 914));
            largeType(bench.desk);
            QVERIFY(bench.open(bench.books.first(), layout));
            const qint64 id = bench.note("late in the autumn", "The season matters");
            QVERIFY(id);
            QTRY_VERIFY(bench.format()->notes());
            QCOMPARE(bench.format()->noteWidth(), 0.0);
            QVERIFY(!bench.format()->marginFolded());
            QTRY_COMPARE(bench.dots().size(), 1);
            QVERIFY(!bench.desk.shows("marginFold"));
            bench.desk.shot("marginfold-noroom-" + layout);
            bench.tap(middleOf(bench.dots().first()));
            QTRY_VERIFY(bench.cardShown());
            bench.desk.shot("marginfold-noroom-card-" + layout);
            bench.key(Qt::Key_Escape);
            QTRY_VERIFY(!bench.cardShown());
            // Folded, it is the same: there was no column to fold.
            bench.content()->forceActiveFocus();
            bench.key(Qt::Key_E, Qt::ShiftModifier);
            QTRY_VERIFY(bench.desk.settings->marginFolded());
            QCOMPARE(bench.dots().size(), 1);
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
        }
    }

    // The folded margin, its dots and a card opened from one, and the column
    // with its button, at every size; in English, German, a right-to-left
    // language and under fingers, and in large type at a laptop's size.
    void foldedMarginAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Bench bench(way.language, way.touch, QSize(1440, 900), way.language == "ar" ? lightTheme : QByteArray());
            if (bench.desk.window->minimumWidth() > 420 || bench.desk.window->minimumHeight() > 360)
                bench.desk.window->setMinimumSize(QSize(420, 360));
            bench.desk.quicken();
            QVERIFY(bench.note("late in the autumn", "The season matters later in the book"));
            QVERIFY(bench.note("ferry came back", "Where it docks"));
            QVERIFY(bench.note("harbour master counted", "He counts twice", 1));
            QList<QSize> all = sizes();
            all << QSize(1512, 914);
            const QVariantMap usual = bench.format()->snapshot();
            for (const bool folded : {false, true}) {
                bench.desk.settings->setMarginFolded(folded);
                for (const QSize &size : std::as_const(all)) {
                    QVERIFY(bench.desk.resize(size));
                    if (size == QSize(1512, 914))
                        largeType(bench.desk);
                    bench.desk.settle();
                    look(bench.desk, folded ? "folded" : "unfolded");
                    const QList<QQuickItem *> dots = bench.dots();
                    if (dots.isEmpty())
                        continue;
                    QQuickItem *dot = dots.first();
                    if (way.touch)
                        bench.tap(middleOf(dot));
                    else
                        QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, middleOf(dot).toPoint());
                    if (!QTest::qWaitFor([&] { return bench.cardShown(); }, 1000)) {
                        found << QString("%1 %2x%3: the dot didn't open its note")
                                     .arg(QString::fromLatin1(way.language))
                                     .arg(size.width())
                                     .arg(size.height());
                        continue;
                    }
                    look(bench.desk, folded ? "folded-card" : "unfolded-card");
                    bench.key(Qt::Key_Escape);
                    QTRY_VERIFY(!bench.cardShown());
                    QVERIFY(bench.desk.reader->isOpen());
                    if (size == QSize(1512, 914))
                        bench.format()->restore(usual);
                }
            }
            QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(warned(bench.desk)));
        }
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(MarginFoldTests)
#include "marginfold_tests.moc"
