// Others' annotations in the window (#85): someone's margin notes stand in the
// margin column under their name and in their colour, their highlights lie
// under the words, as far as the reader has read and no further; their words
// are plain text, never markup; each person is switched off on their own, or
// all at once; and a note of theirs is kept as a fact. At every size, in a
// language that reads right to left, and under fingers, with nothing cut off
// or overlapping (the layout survey).

#include <QtTest>

#include "annotationedits.h"
#include "annotationlayerview.h"
#include "desk.h"
#include "graph.h"
#include "person.h"
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

[[nodiscard]] QList<QQuickItem *> shownCards(Desk &desk) { return shownItems(desk, "othersNote"); }

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

private slots:
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
        QTRY_COMPARE(shownCards(desk).size(), 2);
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
        QTRY_COMPARE(shownCards(desk).size(), 0);
        layer->setPersonShown(ada.id, true);
        QTRY_COMPARE(shownCards(desk).size(), 2);
        layer->setShown(false);
        QTRY_COMPARE(shownCards(desk).size(), 0);
        QTRY_VERIFY(shownItems(desk, "othersHighlightRect").isEmpty());
        layer->setShown(true);
        QTRY_COMPARE(shownCards(desk).size(), 2);

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
