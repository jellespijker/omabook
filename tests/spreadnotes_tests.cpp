// Margin notes on a spread (#165): a book laid out as two pages side by side
// takes a note on the words of either page, typed (Shift+N, or the menu's
// Note) or written by hand, where the margin column stands beyond the pages
// and where there is no room for one and the note is written on its card
// (#157). Each page of the spread is a text of its own holding the whole
// chapter; the first note asks for the margin column, which can lay the
// spread out again with fewer or narrower pages. The note is kept: closed and
// opened again, the book has it on its passage.

#include <QGuiApplication>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QtTest>
#include <qpa/qwindowsysteminterface.h>

#include "desk.h"
#include "hyphenation.h"
#include "marginnotes.h"
#include "pagelayout.h"

namespace {

constexpr auto penCaps = QInputDevice::Capability::Position | QInputDevice::Capability::Pressure | QInputDevice::Capability::Hover;

QPointingDevice &pen() {
    static QPointingDevice device{"pen", 9811, QInputDevice::DeviceType::Stylus, QPointingDevice::PointerType::Pen, penCaps, 1, 2};
    [[maybe_unused]] static const bool registered = [] {
        QWindowSystemInterface::registerInputDevice(&device);
        return true;
    }();
    return device;
}

[[nodiscard]] QRectF sceneRect(QQuickItem *item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

[[nodiscard]] QString textOf(QQuickItem *text) {
    QString words;
    QMetaObject::invokeMethod(text, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0), Q_ARG(int, text->property("length").toInt()));
    return words;
}

void type(QWindow *window, const QString &words) {
    for (const QChar letter : words)
        QTest::keyClick(window, letter.toLatin1());
}

// The reader's settings a row sets, and the window it reads in.
struct Room {
    QSize window;
    QList<std::pair<QString, QJsonValue>> options;
};

struct Bench {
    Desk desk;
    QList<qint64> ids;
    ulong clock = 800'000;

    explicit Bench(const Room &room) : desk("en", {}, false) {
        ids = stock(desk);
        for (const auto &[key, value] : room.options)
            QVERIFY2(desk.settings->setOption(key, value), qPrintable(key));
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(room.window));
        desk.quicken();
    }
    [[nodiscard]] bool open() {
        desk.reader->format()->set("layout", "pages", false);
        desk.reader->openBook(ids.first());
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && spread() && desk.shows("flowText"); }, 5000))
            return false;
        desk.settle();
        return true;
    }
    // The window made as wide as `kind` asks, for the pages the book has
    // without notes and with them (PageLayout::columns, as the window decides):
    // two pages, then one beside the margin column ("fewer"); two narrower
    // beside it ("narrower"); two and no room for it ("card"); three ("three").
    [[nodiscard]] bool fit(const QString &kind) {
        const PageFormat &format = *desk.reader->format();
        const int pages = desk.settings->pages();
        const double around = desk.window->width() - format.available();
        for (double width = 400; width < 3800; width += 2) {
            PageLayout::Space space{.available = width,
                                    .characterWidth = format.characterWidth(),
                                    .measure = format.effective().value("measure").toInt(),
                                    .margin = format.margin(),
                                    .gutter = format.gutter(),
                                    .notes = false};
            const PageLayout::Columns without = PageLayout::columns(space, pages);
            space.notes = true;
            const PageLayout::Columns with = PageLayout::columns(space, pages, without.count);
            const bool wanted = kind == "three"      ? without.count == 3
                                : without.count != 2 ? false
                                : kind == "fewer"    ? with.notes > 0 && with.count == 1
                                : kind == "narrower" ? with.notes > 0 && with.count == 2
                                                     : with.notes == 0;
            if (!wanted)
                continue;
            if (!desk.resize(QSize(static_cast<int>(std::ceil(width + around)), desk.window->height())))
                return false;
            desk.settle();
            return QTest::qWaitFor([&] { return spread() && spread()->property("columns").toInt() == without.count; }, 3000);
        }
        return false;
    }
    [[nodiscard]] QQuickItem *spread() const { return desk.item("spreadReader"); }
    [[nodiscard]] MarginNotes *notes() const { return desk.find("readingNotes")->property("notes").value<MarginNotes *>(); }
    [[nodiscard]] QList<Annotation> made() { return desk.backend->library()->annotations(ids.first(), "margin-note"); }
    [[nodiscard]] int columns() const { return spread()->property("columns").toInt(); }
    // A page of the spread's text, the first counted 0.
    [[nodiscard]] QQuickItem *text(int sheet) const {
        QQuickItem *page = desk.item("page" + QString::number(sheet));
        return page ? page->property("text").value<QQuickItem *>() : nullptr;
    }
    [[nodiscard]] int startOf(int index) const {
        QVariant at;
        QMetaObject::invokeMethod(spread(), "startOf", Q_RETURN_ARG(QVariant, at), Q_ARG(QVariant, index));
        return at.toInt();
    }
    // The first paragraph's opening words that start on a page that shows,
    // and where they are in its text (with the soft hyphens it shows them with).
    [[nodiscard]] std::pair<QString, std::pair<int, int>> wordsOn(int sheet) const {
        const QString shown = textOf(text(sheet));
        const Hyphenation::Plain plain(shown);
        const int page = spread()->property("page").toInt() + sheet;
        const qsizetype at = plain.text().indexOf("Paragraph ", plain.toPlain(startOf(page)));
        const qsizetype end = at < 0 ? -1 : plain.text().indexOf(" of a long", at);
        if (end < 0 || plain.toShown(at) >= startOf(page + 1))
            return {};
        return {plain.text().mid(at, end + 10 - at), {static_cast<int>(plain.toShown(at)), static_cast<int>(plain.toShownEnd(end + 10))}};
    }
    [[nodiscard]] QPointF at(int sheet, int position) const {
        QRectF rect;
        QMetaObject::invokeMethod(text(sheet), "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
        return text(sheet)->mapToScene(rect.center());
    }
    // The words selected on a page with the mouse, from where they begin to where they end.
    void drag(int sheet, std::pair<int, int> span) {
        const QPointF from = at(sheet, span.first);
        const QPointF to = at(sheet, span.second);
        QTest::mousePress(desk.window, Qt::LeftButton, {}, from.toPoint());
        for (int i = 1; i <= 8; ++i)
            QTest::mouseMove(desk.window, (from + (to - from) * (i / 8.0)).toPoint(), 10);
        QTest::mouseRelease(desk.window, Qt::LeftButton, {}, to.toPoint());
    }
    void stroke(QPointF from) {
        const QList<QPointF> turns{from, from + QPointF(6, -10), from + QPointF(12, 0), from + QPointF(18, -10), from + QPointF(4, 6)};
        const auto penAt = [&](QPointF point, bool down) {
            clock += 5;
            QWindowSystemInterface::handleTabletEvent(desk.window, clock, &pen(), point, desk.window->mapToGlobal(point),
                                                      down ? Qt::LeftButton : Qt::NoButton, down ? 0.6 : 0.0, 0, 0, 0, 0, 0);
            QWindowSystemInterface::flushWindowSystemEvents();
        };
        for (qsizetype i = 1; i < turns.size(); ++i)
            for (int step = 0; step < 6; ++step)
                penAt(turns.at(i - 1) + ((turns.at(i) - turns.at(i - 1)) * (step / 6.0)), true);
        penAt(turns.last(), false);
        QTest::qWait(desk.item("penGestures")->property("batchPause").toInt() + 120);
    }
    // The note being written takes what is typed: its field has the keyboard.
    [[nodiscard]] bool fieldHasKeys() const {
        const QQuickItem *focused = desk.window->activeFocusItem();
        return focused && focused->objectName() == "noteField" && focused->isVisible();
    }
};

// The reader's type for a row: the large type of #157 on its laptop's
// window, as it is, or made as wide as the row asks.
[[nodiscard]] Room roomOf(const QString &kind) {
    const QList<std::pair<QString, QJsonValue>> large{{"font_size", 34}, {"measure", 83}, {"margins", 24}, {"align", "justify"}};
    if (kind == "large")
        return {QSize(1512, 914), large + QList<std::pair<QString, QJsonValue>>{{"pages", "auto"}}};
    if (kind == "fewer")
        return {QSize(1512, 914), large + QList<std::pair<QString, QJsonValue>>{{"pages", 2}}};
    if (kind == "card")
        return {QSize(1512, 914), {{"measure", 90}, {"pages", 2}}};
    if (kind == "three")
        return {QSize(1512, 914), {{"measure", 50}, {"pages", 3}}};
    return {QSize(1512, 914), {{"pages", 2}}};
}

} // namespace

class SpreadNotesTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // On the left page and the right, typed or by hand, in a spread with
    // room for the margin column and in the large type of #157, where there
    // is none: the note is written, kept, and found on its passage when the
    // book is opened again.
    void aNoteOnEitherPageOfASpreadIsKept_data() {
        QTest::addColumn<QString>("room");
        QTest::addColumn<int>("sheet");
        QTest::addColumn<QString>("way");
        for (const char *room : {"fewer", "narrower", "card", "three", "large"})
            for (const int sheet : {0, 1, 2})
                for (const char *way : {"key", "menu", "pen"})
                    if (sheet < 2 || QString(room) == "three")
                        QTest::newRow(qPrintable(QString("%1-page%2-%3").arg(room).arg(sheet).arg(way)))
                            << QString(room) << sheet << QString(way);
    }

    void aNoteOnEitherPageOfASpreadIsKept() {
        QFETCH(QString, room);
        QFETCH(int, sheet);
        QFETCH(QString, way);
        const Room chosen = roomOf(room);
        Bench bench(chosen);
        QVERIFY(bench.open());
        if (room != "large")
            QVERIFY2(bench.fit(room), qPrintable(room));
        if (bench.columns() <= sheet)
            QSKIP(qPrintable(QString("the window shows %1 pages side by side here").arg(bench.columns())));
        const auto [words, span] = bench.wordsOn(sheet);
        QVERIFY2(!words.isEmpty(), "no paragraph starts on the page");
        const int before = bench.columns();
        if (way == "pen") {
            bench.stroke(bench.at(sheet, span.first + 3));
        } else {
            if (way == "key") {
                QMetaObject::invokeMethod(bench.text(sheet), "select", Q_ARG(int, span.first), Q_ARG(int, span.second));
                QTest::keyClick(bench.desk.window, Qt::Key_N, Qt::ShiftModifier);
            } else {
                bench.drag(sheet, span);
                QObject *menu = bench.desk.find("readingMenu");
                QTRY_VERIFY(menu->property("shown").toBool());
                QQuickItem *button = nullptr;
                QTRY_VERIFY((button = bench.desk.item("readingAction_note")) && button->isVisible() && button->width() > 0);
                bench.desk.settle();
                QTest::mouseClick(bench.desk.window, Qt::LeftButton, {}, sceneRect(button).center().toPoint());
            }
            QTRY_VERIFY2(bench.fieldHasKeys(), qPrintable(QString("%1 pages, then %2").arg(before).arg(bench.columns())));
            bench.desk.settle();
            QVERIFY(bench.fieldHasKeys());
            type(bench.desk.window, "On the page");
            QTest::keyClick(bench.desk.window, Qt::Key_Escape);
        }
        QTRY_COMPARE(bench.made().size(), 1);
        const Annotation note = bench.made().first();
        // Typed, on the words selected; by hand, on the line written beside.
        const auto onWords = [&](const QString &passage) {
            const QString plain = Hyphenation::Plain(passage).text().simplified();
            return way == "pen" ? plain.contains(words) : plain == words;
        };
        QVERIFY2(onWords(note.quote), qPrintable(note.quote));
        if (way != "pen")
            QCOMPARE(note.note, QString("On the page"));
        QVERIFY2(bench.desk.warnings.isEmpty(), qPrintable(bench.desk.warnings.join('\n')));

        // Closed, and the book opened again: on its passage.
        bench.desk.reader->close();
        QTRY_VERIFY(!bench.desk.reader->isOpen());
        bench.desk.settle();
        QVERIFY(bench.open());
        QTRY_COMPARE(bench.notes()->count(), 1);
        const QString shown = textOf(bench.text(0));
        const auto spans = bench.notes()->spansIn(shown);
        QCOMPARE(spans.size(), 1);
        const auto [start, end] = spans.first().second;
        QVERIFY2(onWords(shown.mid(start, end - start)), qPrintable(shown.mid(start, end - start)));
    }
};

QTEST_MAIN(SpreadNotesTests)
#include "spreadnotes_tests.moc"
