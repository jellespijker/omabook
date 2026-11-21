// Others' annotations in the window (#85): someone's margin notes stand in the
// margin column under their name and in their colour, their highlights lie
// under the words, as far as the reader has read and no further; their words
// are plain text, never markup; each person is switched off on their own, or
// all at once; and a note of theirs is kept as a fact. At every size, in a
// language that reads right to left, and under fingers, with nothing cut off
// or overlapping (the layout survey). Where there is no margin column, their
// notes are marked in the text and open on a card (#124); their book ink is
// drawn over the pages of a PDF in their colour, as far as the reader has
// read and no more strokes than an annotation may have.

#include <QDateTime>
#include <QtTest>
#include <algorithm>

#include "annotationedits.h"
#include "annotationlayerview.h"
#include "bookink.h"
#include "desk.h"
#include "graph.h"
#include "inksurface.h"
#include "pageformat.h"
#include "person.h"
#include "plugindata.h"
#include "publishedspace.h"
#include "subscriptions.h"

namespace {

// Someone who read the chronicle and shares it with their notes: two near the
// start of its first chapter, a highlight after them, and a note far on.
struct Sharer {
    explicit Sharer(Desk &desk, qint64 book) : library(desk.dir.filePath("Sharer")) {
        QVERIFY(library.open());
        const qint64 theirs = library.add(desk.backend->library()->formatFiles(book).first().path).id;
        QVERIFY(theirs > 0);
        const auto shelf = library.addCollection("Lanterns");
        QVERIFY(shelf && library.putInCollection(*shelf, theirs, true));
        for (const auto &[kind, words, note] :
             {std::tuple{"margin-note", "Paragraph 2 of a long chapter", "<b>Not bold</b>: the second paragraph, as I read it."},
              std::tuple{"margin-note", "Paragraph 4 of a long chapter",
                         "A longer note of theirs, which runs on over several lines of the margin column, to see it wrap."},
              std::tuple{"highlight", "Paragraph 3 of a long chapter", ""},
              std::tuple{"margin-note", "Paragraph 70 of a long chapter", "Far ahead: who the keeper really is."}}) {
            const auto added =
                AnnotationEdits::add(library, theirs, {.kind = kind, .place = {1, 0}, .words = words, .color = {}, .note = note});
            QVERIFY2(added.has_value(), qPrintable(Fixtures::whyNot(added)));
        }
        section = library.annotations(theirs, "margin-note").first().place.section;
        const auto person = Person::generate();
        QVERIFY(person.has_value());
        id = person->id();
        const auto snapshot = PublishedSpace::snapshot(library, *shelf, {.annotations = true}, {.name = "Ada Lovelace", .person = id});
        QVERIFY(snapshot.has_value());
        QVERIFY(Subscriptions::subscribe(*desk.backend->library(), *snapshot, {}).has_value());
    }

    Library library;
    int section = 0;
    QString id;
};

// Someone who drew on the first two pages of a PDF and shares the ink: on
// the first `strokes` strokes, on the second a few. Written as their pen
// would, a batch of at most 500 at a time.
struct Inker {
    Inker(Desk &desk, const QString &file, int strokes) : library(desk.dir.filePath("Inker")) {
        QVERIFY(library.open());
        const qint64 theirs = library.add(file).id;
        QVERIFY(theirs > 0);
        const auto shelf = library.addCollection("Ferries");
        QVERIFY(shelf && library.putInCollection(*shelf, theirs, true));
        for (int page = 0; page < 2; ++page) {
            const int many = page == 0 ? strokes : 3;
            for (int from = 0; from < many; from += 500) {
                Ink::Change change;
                for (int i = from; i < std::min(many, from + 500); ++i)
                    change.added.push_back(line(i));
                const auto written = BookInk::write(library, theirs, page, change);
                QVERIFY2(written.has_value(), qPrintable(Fixtures::whyNot(written)));
            }
        }
        const auto person = Person::generate();
        QVERIFY(person.has_value());
        id = person->id();
        const auto snapshot = PublishedSpace::snapshot(library, *shelf, {.annotations = true}, {.name = "Ada Lovelace", .person = id});
        QVERIFY(snapshot.has_value());
        QVERIFY(Subscriptions::subscribe(*desk.backend->library(), *snapshot, {}).has_value());
    }
    // A short line down the page, the i-th, in a colour of their own.
    static Ink::Stroke line(int i) {
        Ink::Stroke stroke{
            .id = Ink::newId(), .color = Qt::black, .width = 3, .pressured = false, .tilted = false, .began = 1000 + i, .points = {}};
        const double x = 80 + (i % 40) * 20.0;
        const double y = 60 + (i / 40 % 30) * 40.0;
        for (int k = 0; k < 3; ++k)
            stroke.points.push_back(
                {.at = {x + k * 6.0, y + k * 8.0}, .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(k)});
        return stroke;
    }

    Library library;
    QString id;
};

// The items of that name on screen, wherever they are: a repeater's are no QObject children.
[[nodiscard]] QList<QQuickItem *> shownItems(Desk &desk, const QString &name) {
    QList<QQuickItem *> shown;
    QList<QQuickItem *> items{desk.window->contentItem()};
    while (!items.isEmpty()) {
        QQuickItem *item = items.takeFirst();
        if (item->objectName() == name && item->isVisible())
            shown << item;
        items << item->childItems();
    }
    return shown;
}

// Of those, the ones within the window: a mark of the chapter's text may be scrolled out of it.
[[nodiscard]] QList<QQuickItem *> inSight(Desk &desk, const QString &name) {
    QList<QQuickItem *> seen = shownItems(desk, name);
    const QRectF window(QPointF(0, 0), desk.window->size());
    seen.removeIf(
        [&](const QQuickItem *item) { return !window.contains(item->mapToScene(QPointF(item->width() / 2, item->height() / 2))); });
    return seen;
}

// The page scrolled to the first mark of their notes: the reader's place is a little past their passages.
void scrollToMark(Desk &desk) {
    const QList<QQuickItem *> marks = shownItems(desk, "othersNoteMark");
    QQuickItem *flick = marks.isEmpty() ? nullptr : marks.first();
    while (flick != nullptr && !flick->inherits("QQuickFlickable"))
        flick = flick->parentItem();
    if (flick == nullptr)
        return;
    const double markAt = marks.first()->mapToScene(QPointF(0, 0)).y();
    flick->setProperty("contentY", std::max(0.0, flick->property("contentY").toDouble() + markAt - desk.window->height() / 3.0));
    desk.settle();
}

[[nodiscard]] QList<QQuickItem *> shownCards(Desk &desk) { return shownItems(desk, "othersNote"); }

// Empty once `wanted` cards stand; else what the page was like, and a picture of it (#132).
[[nodiscard]] QString cardsStand(Desk &desk, int wanted) {
    if (QTest::qWaitFor([&] { return shownCards(desk).size() == wanted; }, 5000))
        return {};
    const QObject *notes = desk.find("readingNotes");
    const auto *layer = notes != nullptr ? notes->property("others").value<AnnotationLayerView *>() : nullptr;
    // Not in the desk's own folder, which goes with the desk.
    const QString folder = qEnvironmentVariable("OMABOOK_SHOTS", QDir::tempPath());
    const QString picture = folder + QString("/cards-%1.png").arg(QDateTime::currentMSecsSinceEpoch());
    desk.window->grabWindow().save(picture);
    return QString("%1 cards, not %2; beside %3, note width %4, the layer's count %5, window %6x%7; the page is in %8")
        .arg(shownCards(desk).size())
        .arg(wanted)
        .arg(notes != nullptr && notes->property("beside").toBool())
        .arg(desk.reader->format()->noteWidth())
        .arg(layer != nullptr ? layer->count() : -1)
        .arg(desk.window->width())
        .arg(desk.window->height())
        .arg(picture);
}

[[nodiscard]] QQuickItem *within(QQuickItem *item, const QString &name) {
    QList<QQuickItem *> items{item};
    while (!items.isEmpty()) {
        QQuickItem *one = items.takeFirst();
        if (one->objectName() == name)
            return one;
        items << one->childItems();
    }
    return nullptr;
}

[[nodiscard]] AnnotationLayerView *layerOf(Desk &desk) {
    return desk.find("readingNotes")->property("others").value<AnnotationLayerView *>();
}

} // namespace

class AnnotationLayerViewTests : public QObject {
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
        QSet<QString> once;
        for (const QString &problem : std::as_const(survey.problems))
            if (!once.contains(problem)) {
                once.insert(problem);
                found << where + " — " + problem;
            }
        desk.shot(QString("others-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

    // The chronicle open where its reader is: a little into the chapter the notes are in.
    static bool open(Desk &desk, qint64 book, int section) {
        // Being read, not read to its end: the cut is where its reader is.
        desk.backend->library()->setReading(book, "reading", 0, {});
        desk.backend->library()->setProgress(book, {section, 0.06, 6});
        desk.reader->openBook(book);
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.shows("flowText"); }, 5000))
            return false;
        desk.settle();
        return true;
    }

    // A click (or, under fingers, a tap) in the middle of an item.
    static void tap(Desk &desk, const QQuickItem *item) {
        const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QTest::mouseClick(desk.window, Qt::LeftButton, {}, at);
    }

    // A PDF open at its first page, someone's ink on its first two.
    static bool openPdf(Desk &desk, qint64 book) {
        desk.settings->setOption("layout", "scroll");
        desk.backend->library()->setReading(book, "reading", 0, {});
        desk.backend->library()->setProgress(book, {0, 0.5, 1});
        desk.reader->openBook(book);
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.item("othersInk0") != nullptr; }, 5000))
            return false;
        desk.settle();
        return true;
    }

private slots:
    // Where the window has no room for the margin column, their notes are
    // marked at the end of their passages in their colour; a mark opens the
    // note on a card under their name, and Escape puts it away.
    void othersNotesOpenOnCardsWithoutTheColumn() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        const Sharer ada(desk, ids.first());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        QVERIFY(desk.resize(QSize(1440, 900)));
        QVERIFY(open(desk, ids.first(), ada.section));
        if (const QString why = cardsStand(desk, 2); !why.isEmpty()) QFAIL(qPrintable(why));
        QVERIFY(shownItems(desk, "othersNoteMark").isEmpty()); // they stand in the column
        QVERIFY(desk.resize(QSize(560, 800)));
        QTRY_VERIFY(!desk.find("readingNotes")->property("beside").toBool());
        QTRY_COMPARE(shownItems(desk, "othersNoteMark").size(), 2);
        QCOMPARE(shownCards(desk).size(), 0);

        scrollToMark(desk);
        QTRY_VERIFY(!inSight(desk, "othersNoteMark").isEmpty());
        tap(desk, inSight(desk, "othersNoteMark").first());
        QTRY_VERIFY(desk.shows("othersNoteCard"));
        QCOMPARE(shownCards(desk).size(), 1);
        QQuickItem *card = shownCards(desk).first();
        QCOMPARE(within(card, "othersNoteName")->property("text").toString(), QString("Ada Lovelace"));
        QCOMPARE(within(card, "othersNoteWords")->property("textFormat").toInt(), 0);
        QVERIFY(within(card, "othersNoteWords")->property("text").toString().contains("second paragraph"));
        look(desk, "narrow-card");
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.shows("othersNoteCard"));

        // Switched off, no mark of theirs is left.
        layerOf(desk)->setShown(false);
        QTRY_VERIFY(shownItems(desk, "othersNoteMark").isEmpty());
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Their ink is drawn over the pages in their colour, under one's own:
    // of the pages the reader has reached, and of a page no more strokes than
    // an annotation may hold; switched off, it is gone.
    void othersInkIsDrawnOnThePagesInTheirColour() {
        Desk desk("en", {}, false);
        const QString file = desk.dir.filePath("The Ferry.pdf");
        QVERIFY(Fixtures::pdf(file, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."}));
        const qint64 book = desk.backend->library()->add(file).id;
        QVERIFY(book > 0);
        const Inker ada(desk, file, PluginData::maxItems + 200);
        desk.backend->reload();
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        QVERIFY(desk.resize(QSize(1180, 780)));
        QVERIFY(openPdf(desk, book));
        AnnotationLayerView *layer = layerOf(desk);
        QVERIFY(layer);

        const auto *surface = qobject_cast<InkSurface *>(desk.item("othersInk0"));
        QVERIFY(surface);
        QTRY_COMPARE(static_cast<qsizetype>(surface->count()), PluginData::maxItems);
        QVERIFY(surface->isVisible());
        QVERIFY(surface->readOnly());
        const std::vector<Ink::Stroke> drawn = layer->inkOn(0);
        QCOMPARE(static_cast<qsizetype>(drawn.size()), PluginData::maxItems);
        const QColor theirs = AnnotationLayers::colorOf(ada.id);
        QVERIFY(std::ranges::all_of(drawn, [&](const Ink::Stroke &stroke) { return stroke.color == theirs; }));
        // The earliest they drew, as an annotation's strokes are capped.
        QCOMPARE(drawn.front().began, qint64(1000));
        QCOMPARE(drawn.back().began, qint64(1000 + PluginData::maxItems - 1));
        // The page after is past the reader: nothing of it.
        QVERIFY(layer->inkOn(1).empty());
        // One's own ink is still one's own, and empty.
        QCOMPARE(qobject_cast<InkSurface *>(desk.item("bookInk0"))->count(), 0);
        look(desk, "ink");

        // Read on to the second page: its ink comes.
        desk.backend->library()->setProgress(book, {1, 0.5, 2});
        layer->refresh();
        QCOMPARE(static_cast<qsizetype>(layer->inkOn(1).size()), 3);

        layer->setPersonShown(ada.id, false);
        QTRY_COMPARE(surface->count(), 0);
        QVERIFY(!surface->isVisible());
        layer->setPersonShown(ada.id, true);
        QTRY_COMPARE(static_cast<qsizetype>(surface->count()), PluginData::maxItems);
        layer->setShown(false);
        QTRY_COMPARE(surface->count(), 0);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Their ink on the pages, at every size, in German, right to left and under fingers.
    void othersInkAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"nl", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            const QString file = desk.dir.filePath("The Ferry.pdf");
            QVERIFY(Fixtures::pdf(file, "The Ferry", {"The harbour master counted the crates twice.", "Mira watched the lantern."}));
            const qint64 book = desk.backend->library()->add(file).id;
            const Inker ada(desk, file, 120);
            desk.backend->reload();
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            QVERIFY(desk.resize(QSize(1180, 780)));
            QVERIFY(openPdf(desk, book));
            QTRY_COMPARE(qobject_cast<InkSurface *>(desk.item("othersInk0"))->count(), 120);
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "ink");
            }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }

    // Their notes beside the text, theirs alone and as far as the reader is,
    // their words as words; switched off by person and all at once; kept as a fact.
    void othersNotesStandBesideTheTextAsFarAsTheReaderRead() {
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        const Sharer ada(desk, ids.first());
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        desk.quicken();
        QVERIFY(desk.resize(QSize(1440, 900)));
        QVERIFY(open(desk, ids.first(), ada.section));
        AnnotationLayerView *layer = layerOf(desk);
        if (const QString why = cardsStand(desk, 2); !why.isEmpty()) QFAIL(qPrintable(why));
        QVERIFY(layer);
        QCOMPARE(layer->count(), 3); // two notes and the highlight; the note far on is past the reader
        QCOMPARE(layer->people().size(), 1);
        QCOMPARE(layer->people().first().toMap().value("called").toString(), QString("Ada Lovelace"));
        QStringList words;
        for (QQuickItem *card : shownCards(desk)) {
            const QQuickItem *label = within(card, "othersNoteWords");
            QVERIFY(label);
            QCOMPARE(label->property("textFormat").toInt(), 0); // Text.PlainText: words, never markup
            words << label->property("text").toString();
            QCOMPARE(within(card, "othersNoteName")->property("text").toString(), QString("Ada Lovelace"));
        }
        QVERIFY(words.contains("<b>Not bold</b>: the second paragraph, as I read it."));
        QVERIFY(!words.join('\n').contains("Far ahead"));
        QTRY_VERIFY(!shownItems(desk, "othersHighlightRect").isEmpty());

        // Ada's off, then everyone's: nothing of hers is left on the page.
        layer->setPersonShown(ada.id, false);
        if (const QString why = cardsStand(desk, 0); !why.isEmpty()) QFAIL(qPrintable(why));
        layer->setPersonShown(ada.id, true);
        if (const QString why = cardsStand(desk, 2); !why.isEmpty()) QFAIL(qPrintable(why));
        layer->setShown(false);
        if (const QString why = cardsStand(desk, 0); !why.isEmpty()) QFAIL(qPrintable(why));
        QTRY_VERIFY(shownItems(desk, "othersHighlightRect").isEmpty());
        layer->setShown(true);
        if (const QString why = cardsStand(desk, 2); !why.isEmpty()) QFAIL(qPrintable(why));

        // Her note kept as a fact of the lantern keeper, in the book's graph.
        const QVariantList notes = layer->notesIn(desk.item("flowText")->property("textDocument").value<QQuickTextDocument *>());
        QVERIFY(!notes.isEmpty());
        QCOMPARE(layer->makeFact(notes.first().toMap().value("id").toString(), "The Lantern Keeper", "character"), QString());
        const Graph graph = Graph::ofReader(*desk.backend->library(), ids.first());
        QVERIFY(graph.named({}, "character", "The Lantern Keeper") > 0);
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // The notes, the people's switches and the card that keeps a fact, at
    // every size, in English, German, a right-to-left language and under fingers.
    void othersAnnotationsAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"nl", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            const QList<qint64> ids = stock(desk);
            const Sharer ada(desk, ids.first());
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            QVERIFY(desk.resize(QSize(1440, 900)));
            QVERIFY(open(desk, ids.first(), ada.section));
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "notes");
            }
            // Where there is no column, a note of theirs opened from its mark, on its card.
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                if (desk.find("readingNotes")->property("beside").toBool())
                    continue;
                desk.settle();
                scrollToMark(desk);
                const QList<QQuickItem *> marks = inSight(desk, "othersNoteMark");
                if (marks.isEmpty())
                    continue;
                tap(desk, marks.first());
                if (!QTest::qWaitFor([&] { return desk.shows("othersNoteCard"); }, 2000))
                    continue;
                look(desk, "card");
                QTest::keyClick(desk.window, Qt::Key_Escape);
                desk.settle();
            }
            // A card asked to keep a note as a fact.
            QVERIFY(desk.resize(QSize(1440, 900)));
            if (QTest::qWaitFor([&] { return !shownCards(desk).isEmpty(); }, 2000)) {
                QQuickItem *card = shownCards(desk).first();
                card->setProperty("keeping", true);
                desk.settle(4); // the column stacks the cards again once the card has grown
                look(desk, "keeping");
                card->setProperty("keeping", false);
            }
            // Whose are shown, in the contents' list of annotations.
            QMetaObject::invokeMethod(desk.find("readerView"), "showList", Q_ARG(QVariant, QString("annotations")));
            if (QTest::qWaitFor([&] { return desk.shows("othersAnnotations"); }, 2000))
                for (const QSize &size : sizes()) {
                    QVERIFY(desk.resize(size));
                    look(desk, "switches");
                }
            QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
        }
        const QStringList problems = found;
        found.clear();
        if (!problems.isEmpty())
            QFAIL(qPrintable(
                QString("%1 layout problems in %2 looks:\n%3").arg(problems.size()).arg(looked).arg(problems.mid(0, 40).join('\n'))));
    }
};

QTEST_MAIN(AnnotationLayerViewTests)
#include "annotationlayerview_tests.moc"
