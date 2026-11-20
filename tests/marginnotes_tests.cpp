// Typed margin notes, in a margin column (#82): a note is words about a
// passage, kept with the passage's words so it is found again as a highlight
// is (#80). In a window with room for it, notes stand in a margin column
// beside the text at their passages' height, stacked where passages are
// close, and stay beside them as the page is laid out again; in a narrower
// one a mark in the text opens the note. The margin column is the layout's
// to decide (PageLayout::columns), so the same room always cuts the same
// pages. Notes are written, edited and removed from the keyboard first; they
// travel between one's own computers in the change log (annotationsync_tests).

#include <QPainter>
#include <QPdfWriter>
#include <QQuickStyle>
#include <QQuickTextDocument>
#include <QtTest>

#include "annotationanchors.h"
#include "booktext.h"
#include "desk.h"
#include "hyphenation.h"
#include "marginnotes.h"
#include "pagelayout.h"
#include "readingpassages.h"

namespace {

const QStringList firstEdition{"It was late in the autumn when the ferry came back to the island.",
                               "Mira stood at the rail and watched the lantern on the far pier go out, one wick at a time.",
                               "She had recieved the letter three days before, and had not opened it.",
                               "The gulls were quiet that evening over the grey water of the bay."};

// A story of paragraphs in each format Omabook reads text from.
[[nodiscard]] bool writeBook(const QString &path, const QStringList &paragraphs) {
    const QString format = QFileInfo(path).suffix();
    QString body;
    for (const QString &paragraph : paragraphs)
        body += "<p>" + paragraph + "</p>";
    if (format == "epub")
        return Fixtures::chapteredEpub(path, "The Ferry", {{"One", "<h1>One</h1>" + body}});
    if (format == "fb2")
        return Fixtures::write(path, QString("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                             "<FictionBook xmlns=\"http://www.gribuser.ru/xml/fictionbook/2.0\"><description><title-info>"
                                             "<book-title>The Ferry</book-title><lang>en</lang></title-info></description>"
                                             "<body><section><title><p>One</p></title>%1</section></body></FictionBook>")
                                         .arg(body)
                                         .toUtf8());
    if (format == "html")
        return Fixtures::write(path, ("<html><head><title>The Ferry</title></head><body><h1>One</h1>" + body + "</body></html>").toUtf8());
    if (format == "md")
        return Fixtures::write(path, ("# One\n\n" + paragraphs.join("\n\n") + "\n").toUtf8());
    if (format == "txt")
        return Fixtures::write(path, ("The Ferry\n\n" + paragraphs.join("\n\n") + "\n").toUtf8());
    QPdfWriter writer(path);
    writer.setTitle("The Ferry");
    writer.setPageSize(QPageSize(QPageSize::A5));
    QPainter painter(&writer);
    painter.drawText(QRect(300, 300, 5000, 8000), Qt::TextWordWrap, paragraphs.join("\n\n"));
    return painter.end();
}

// The section's text as a page shows it: a heading above, its lines broken,
// and soft hyphens in its longer words, as hyphenation puts them.
[[nodiscard]] QString asShown(const QString &text) {
    QString shown = "One\n\n" + QString(text).replace(". ", ".\n");
    for (const char *word : {"lantern", "letter", "autumn", "evening"})
        shown.replace(QString(word), QString(word).insert(3, Hyphenation::softHyphen));
    return shown;
}

// Where words are in a text (the first time after `after`), however its
// lines break them, its soft hyphens left out of the looking.
[[nodiscard]] std::pair<qsizetype, qsizetype> spanOf(const QString &shown, const QString &words, qsizetype after = 0) {
    const Hyphenation::Plain plain(shown);
    const QRegularExpression pattern(QRegularExpression::escape(words).replace("\\ ", "\\s+"));
    const QRegularExpressionMatch match = pattern.match(plain.text(), plain.toPlain(after));
    return !match.hasMatch() ? std::pair<qsizetype, qsizetype>{-1, -1}
                             : std::pair{plain.toShown(match.capturedStart()), plain.toShownEnd(match.capturedEnd())};
}

[[nodiscard]] QList<double> stacked(const QList<PageLayout::Note> &notes, double bottom = -1) {
    return PageLayout::stack(notes, 8, bottom);
}

// The window's things, by name, and what they say.
[[nodiscard]] QRectF sceneRect(QQuickItem *item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

[[nodiscard]] QRectF charRect(QQuickItem *text, int position) {
    QRectF rect;
    QMetaObject::invokeMethod(text, "positionToRectangle", Q_RETURN_ARG(QRectF, rect), Q_ARG(int, position));
    return text->mapRectToScene(rect);
}

// Words typed into the window, a key at a time.
void type(QWindow *window, const QString &words) {
    for (const QChar letter : words)
        QTest::keyClick(window, letter.toLatin1());
}

[[nodiscard]] QString textOf(QQuickItem *text) {
    QString words;
    QMetaObject::invokeMethod(text, "getText", Q_RETURN_ARG(QString, words), Q_ARG(int, 0), Q_ARG(int, text->property("length").toInt()));
    return words;
}

} // namespace

class MarginNotesTests : public QObject {
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
        desk.shot(QString("notes-%1%2-%3-%4x%5")
                      .arg(QString::fromLatin1(desk.language), desk.touch ? "-touch" : "", state)
                      .arg(desk.window->width())
                      .arg(desk.window->height()));
    }

    static MarginNotes *notesOf(Desk &desk) { return desk.find("readingNotes")->property("notes").value<MarginNotes *>(); }

    // The chronicle open in the window, read as `layout` says, with its first
    // chapter's text on the page.
    static QQuickItem *openChronicle(Desk &desk, const QList<qint64> &ids, const QString &layout) {
        desk.reader->format()->set("layout", layout, false);
        desk.reader->openBook(ids.first());
        if (!QTest::qWaitFor([&] { return desk.reader->isOpen() && desk.shows("flowText"); }, 5000))
            return nullptr;
        desk.settle();
        return desk.item("flowText");
    }

    // A note written in the window on the words, as the keyboard writes it:
    // they are selected, Shift+N, the words typed, Escape. Its number.
    static qint64 writeOn(Desk &desk, QQuickItem *text, const QString &words, const QString &note, qsizetype after = 0) {
        const auto [start, end] = spanOf(textOf(text), words, after);
        if (start < 0)
            return 0;
        QMetaObject::invokeMethod(text, "select", Q_ARG(int, static_cast<int>(start)), Q_ARG(int, static_cast<int>(end)));
        const int before = notesOf(desk)->count();
        QTest::keyClick(desk.window, Qt::Key_N, Qt::ShiftModifier);
        desk.settle();
        type(desk.window, note);
        QTest::keyClick(desk.window, Qt::Key_Escape);
        if (!QTest::qWaitFor([&] { return notesOf(desk)->count() == before + 1; }, 2000))
            return 0;
        desk.settle();
        const QVariantList all = notesOf(desk)->notes();
        for (const QVariant &one : all)
            if (one.toMap().value("note").toString() == note)
                return one.toMap().value("id").toLongLong();
        return 0;
    }

    // Whether the note's card stands at the height of its passage's first line.
    static bool besideItsPassage(Desk &desk, qint64 id, const QString &words) {
        QQuickItem *card = desk.item(QString("marginNote_%1").arg(id));
        QQuickItem *text = desk.item("flowText");
        if (!card || !card->isVisible() || !text)
            return false;
        const qsizetype start = spanOf(textOf(text), words).first;
        return std::abs(sceneRect(card).top() - charRect(text, static_cast<int>(start)).top()) < 1.5;
    }

private slots:
    void initTestCase() { QQuickStyle::setStyle("Material"); }

    // A book with notes has a margin column where a comfortable page still
    // fits beside it, and none where it doesn't; one without notes never
    // has one. The same room gives the same layout every time.
    void theMarginColumnIsTheLayoutsToDecide() {
        PageLayout::Space room{.available = 1400, .characterWidth = 9, .measure = 66, .margin = 56, .gutter = 54, .notes = true};
        const PageLayout::Columns wide = PageLayout::columns(room, 1);
        QVERIFY(wide.notes >= PageLayout::leastNoteWidth);
        QCOMPARE(wide.notes, PageLayout::noteMeasure * PageLayout::noteScale * 9);
        QVERIFY(2 * room.margin + wide.width + room.gutter + wide.notes <= room.available);
        QCOMPARE(PageLayout::columns(room, 1), wide);
        // The pages share what the column leaves: two at the measure fit
        // without it, and with it they are narrower or fewer.
        const PageLayout::Columns two = PageLayout::columns(room, 2);
        room.notes = false;
        const PageLayout::Columns without = PageLayout::columns(room, 2);
        QCOMPARE(without.notes, 0.0);
        QVERIFY(two.count < without.count || two.width < without.width);
        // Too narrow for a comfortable page and a column: marks in the text.
        room.notes = true;
        room.available = 700;
        const PageLayout::Columns narrow = PageLayout::columns(room, 0);
        QCOMPARE(narrow.notes, 0.0);
        room.notes = false;
        QCOMPARE(PageLayout::columns(room, 0), narrow);
    }

    // Notes stand where their passages start, each under the one before
    // when they are close, never over another; within a page's foot they
    // are pushed up as far as the top, and what still doesn't fit runs past it.
    void notesAreStackedNeverOverAnother() {
        QCOMPARE(stacked({{.top = 100, .height = 50}, {.top = 300, .height = 50}}), (QList<double>{100, 300}));
        QCOMPARE(stacked({{.top = 100, .height = 50}, {.top = 110, .height = 30}, {.top = 120, .height = 20}}),
                 (QList<double>{100, 158, 196}));
        // Given in another order, each keeps its own place.
        QCOMPARE(stacked({{.top = 110, .height = 30}, {.top = 100, .height = 50}}), (QList<double>{158, 100}));
        QCOMPARE(stacked({{.top = 150, .height = 40}, {.top = 160, .height = 40}}, 200), (QList<double>{112, 160}));
        QCOMPARE(stacked({{.top = 50, .height = 100}, {.top = 60, .height = 100}, {.top = 70, .height = 100}}, 200),
                 (QList<double>{0, 108, 216}));
    }

    // In every format text is read from: a note written on words as the page
    // shows them is on the book's own text, its words those of the passage,
    // placed in the file it is read from; it is found where the page shows
    // it, changed, and removed when its words are taken away.
    void aNoteIsWrittenOnTheBooksOwnTextInEveryFormat_data() {
        QTest::addColumn<QString>("format");
        for (const char *format : {"epub", "fb2", "md", "html", "txt", "pdf"})
            QTest::newRow(format) << QString(format);
    }

    void aNoteIsWrittenOnTheBooksOwnTextInEveryFormat() {
        QFETCH(QString, format);
        Desk desk("en", {}, false);
        const QString file = desk.dir.filePath("The Ferry." + format);
        QVERIFY(writeBook(file, firstEdition));
        Library &library = *desk.backend->library();
        const qint64 book = library.add(file).id;
        QVERIFY(book > 0);
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        // To the section that has the words: the book's own heading may be one of its own.
        while (!desk.reader->sectionText().contains("lantern") && desk.reader->section() + 1 < desk.reader->count())
            desk.reader->setSection(desk.reader->section() + 1);
        MarginNotes notes;
        notes.setReader(&*desk.reader);
        QCOMPARE(desk.reader->format()->notes(), false);
        const QString shown = asShown(desk.reader->sectionText());
        const QString words = "watched the lantern on the far pier";
        const auto [start, end] = spanOf(shown, words);
        QVERIFY(start >= 0);
        const std::optional<qint64> id = notes.write(shown, start, end, "  The lantern again.  ");
        QVERIFY(id);
        QVERIFY(desk.reader->format()->notes());
        const QList<Annotation> kept = library.annotations(book, "margin-note");
        QCOMPARE(kept.size(), 1);
        const Annotation &note = kept.first();
        QCOMPARE(note.note, QString("The lantern again."));
        QCOMPARE(note.quote.simplified(), words);
        const QString own = desk.reader->sectionText();
        const qsizetype from = static_cast<qsizetype>(std::lround(note.place.fraction * own.size()));
        QCOMPARE(own.mid(from, words.size()).simplified(), words);
        QCOMPARE(note.anchored, AnnotationAnchors::readableFile(library, book)->hash);
        QCOMPARE(note.confidence, 1.0);
        // Where the page shows it, however it breaks and hyphenates its lines.
        const auto spans = notes.spansIn(shown);
        QCOMPARE(spans.size(), 1);
        QCOMPARE(spans.first(), (std::pair{*id, std::pair{start, end}}));
        // Not a bookmark: the contents' list of bookmarks doesn't show it.
        QVERIFY(desk.reader->bookmarks().isEmpty());
        QVERIFY(notes.change(*id, "Another lantern."));
        QCOMPARE(library.annotations(book, "margin-note").first().note, QString("Another lantern."));
        QCOMPARE(library.annotations(book, "margin-note").first().created, note.created);
        QVERIFY(notes.change(*id, "   "));
        QVERIFY(library.annotations(book, "margin-note").isEmpty());
        QCOMPARE(notes.count(), 0);
        QCOMPARE(desk.reader->format()->notes(), false);
        // Words nobody can see are no passage.
        QVERIFY(!notes.write(shown, 3, 5, "Nothing")); // the space under the heading
        QVERIFY(!notes.write(shown, start, end, "   "));
    }

    // A note whose words a new edition took out is lost: listed, kept with
    // its words, and placed again on the words the reader selects.
    void aLostNoteIsListedAndPlacedAgain() {
        Desk desk("en", {}, false);
        const QString file = desk.dir.filePath("The Ferry.epub");
        QVERIFY(writeBook(file, firstEdition));
        Library &library = *desk.backend->library();
        const qint64 book = library.add(file).id;
        desk.reader->openBook(book);
        QTRY_VERIFY(desk.reader->isOpen());
        while (!desk.reader->sectionText().contains("gulls") && desk.reader->section() + 1 < desk.reader->count())
            desk.reader->setSection(desk.reader->section() + 1);
        MarginNotes notes;
        notes.setReader(&*desk.reader);
        const QString shown = desk.reader->sectionText();
        const auto gulls = spanOf(shown, "The gulls were quiet that evening");
        QVERIFY(notes.write(shown, gulls.first, gulls.second, "Quiet gulls"));
        // The same file, but for its last paragraph, taken out.
        const int section = desk.reader->section();
        QString cut = desk.reader->sectionText();
        cut.remove(QRegularExpression("The gulls[^.]*\\."));
        QVERIFY(!cut.contains("gulls"));
        const BookText text(desk.reader->count(), [&](int asked) { return asked == section ? cut : QString(); });
        const AnnotationAnchors::Summary summary = AnnotationAnchors::anchor(library, book, text, "edition");
        QVERIFY2(summary.lost == 1,
                 qPrintable(QString("%1 kept, %2 moved, %3 lost").arg(summary.kept).arg(summary.moved).arg(summary.lost)));
        notes.refresh();
        QCOMPARE(notes.count(), 0);
        QCOMPARE(notes.lost().size(), 1);
        QCOMPARE(notes.lost().first().toMap().value("note").toString(), QString("Quiet gulls"));
        QVERIFY(notes.spansIn(shown).isEmpty());
        const auto rail = spanOf(shown, "stood at the rail");
        QVERIFY(notes.placeAgain(notes.lost().first().toMap().value("id").toLongLong(), shown, rail.first, rail.second));
        QVERIFY(notes.lost().isEmpty());
        QCOMPARE(notes.count(), 1);
        QCOMPARE(library.annotations(book, "margin-note").first().quote, QString("stood at the rail"));
    }

    // In the window: written by the keyboard, a note stands beside its
    // passage in the margin column, and stays beside it as the window
    // narrows and the type grows, in a column that scrolls and on pages that
    // turn; two on one line are stacked; E goes to them, Enter edits one and
    // Delete removes it. Where there is no room for the column, a mark at
    // the passage's end opens the note on a card, to be edited there.
    void aNoteStandsBesideItsPassageThroughReflow_data() {
        QTest::addColumn<QString>("layout");
        QTest::newRow("scroll") << "scroll";
        QTest::newRow("pages") << "pages";
    }

    void aNoteStandsBesideItsPassageThroughReflow() {
        QFETCH(QString, layout);
        Desk desk("en", {}, false);
        const QList<qint64> ids = stock(desk);
        QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
        QVERIFY(desk.resize(QSize(1600, 900)));
        desk.settings->setPages(1);
        QQuickItem *text = openChronicle(desk, ids, layout);
        QVERIFY(text);
        QCOMPARE(desk.reader->format()->noteWidth(), 0.0);
        const QString first = "Paragraph 2 of a long chapter";
        const qint64 one = writeOn(desk, text, first, "The second paragraph");
        QVERIFY(one > 0);
        QVERIFY(desk.reader->format()->noteWidth() > 0);
        QTRY_VERIFY(besideItsPassage(desk, one, first));
        // Laid out again: narrower, then larger type.
        QVERIFY(desk.resize(QSize(1280, 800)));
        QTRY_VERIFY(besideItsPassage(desk, one, first));
        desk.reader->format()->zoom(2);
        QTRY_VERIFY(besideItsPassage(desk, one, first));
        QVERIFY(desk.reader->format()->noteWidth() > 0);

        // A second on the same line stands under the first.
        const qint64 two = writeOn(desk, text, "a long chapter, with enough", "And the same line", spanOf(textOf(text), first).first);
        QVERIFY(two > 0);
        QQuickItem *upper = desk.item(QString("marginNote_%1").arg(one));
        QQuickItem *lower = desk.item(QString("marginNote_%1").arg(two));
        QTRY_VERIFY(lower && lower->isVisible() && sceneRect(lower).top() >= sceneRect(upper).bottom() + 7.5);

        // E to the notes, Enter to edit the first, the words typed over.
        desk.reader->format()->zoom(-2);
        desk.settle();
        QTest::keyClick(desk.window, Qt::Key_E);
        QTRY_COMPARE(desk.find("readingNotes")->property("current").toLongLong(), one);
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTRY_COMPARE(desk.find("readingNotes")->property("current").toLongLong(), two);
        QTest::keyClick(desk.window, Qt::Key_Up);
        QTest::keyClick(desk.window, Qt::Key_Return);
        QTest::keyClick(desk.window, Qt::Key_A, Qt::ControlModifier);
        type(desk.window, "Changed by keys");
        QTest::keyClick(desk.window, Qt::Key_Return, Qt::ControlModifier);
        QTRY_COMPARE(desk.backend->library()->annotations(ids.first(), "margin-note").first().note, QString("Changed by keys"));
        // Still on the notes: Down to the second, Delete removes it.
        QTest::keyClick(desk.window, Qt::Key_Down);
        QTest::keyClick(desk.window, Qt::Key_Delete);
        QTRY_COMPARE(notesOf(desk)->count(), 1);
        QTest::keyClick(desk.window, Qt::Key_Escape);

        // Narrow: no column; the mark opens the note, which is edited on its card.
        QVERIFY(desk.resize(QSize(560, 800)));
        QTRY_COMPARE(desk.reader->format()->noteWidth(), 0.0);
        QVERIFY(!desk.shows("marginColumn"));
        QQuickItem *mark = nullptr;
        QTRY_VERIFY((mark = desk.item(QString("noteMark_%1").arg(one))) && mark->isVisible());
        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, sceneRect(mark).center().toPoint());
        QTRY_VERIFY(desk.shows("noteCard"));
        QTest::keyClick(desk.window, Qt::Key_End, Qt::ControlModifier);
        type(desk.window, ", by a finger");
        QTest::keyClick(desk.window, Qt::Key_Escape);
        QTRY_VERIFY(!desk.shows("noteCard"));
        QCOMPARE(desk.backend->library()->annotations(ids.first(), "margin-note").first().note, QString("Changed by keys, by a finger"));
        QVERIFY2(desk.warnings.isEmpty(), qPrintable(desk.warnings.join('\n')));
    }

    // Notes, a card and the lost ones' list at every size, in English,
    // German and a right-to-left language, and under fingers.
    void theNotesAtEverySize() {
        struct Way {
            QByteArray language;
            bool touch;
        };
        for (const Way &way : {Way{"en", false}, Way{"de", false}, Way{"ar", false}, Way{"de", true}}) {
            Desk desk(way.language, way.language == "ar" ? lightTheme : QByteArray(), way.touch);
            const QList<qint64> ids = stock(desk);
            QVERIFY2(desk.show(), qPrintable(desk.warnings.join('\n')));
            if (desk.window->minimumWidth() > 420 || desk.window->minimumHeight() > 360)
                desk.window->setMinimumSize(QSize(420, 360));
            desk.quicken();
            QVERIFY(desk.resize(QSize(1440, 900)));
            QQuickItem *text = openChronicle(desk, ids, "auto");
            QVERIFY(text);
            MarginNotes *notes = notesOf(desk);
            const QString shown = textOf(text);
            for (const auto &[words, note] :
                 {std::pair{"Paragraph 1 of a long", "A short note."},
                  std::pair{"Paragraph 2 of a long", "A longer note, which runs on over several lines of the margin column to see "
                                                     "that it wraps, stacks under the one before and is cut where a page ends."}}) {
                const auto [start, end] = spanOf(shown, words);
                QVERIFY(start >= 0);
                QVERIFY(notes->write(shown, start, end, note));
            }
            Annotation lost{.id = 0, .kind = "margin-note", .place = {}, .text = "gone", .note = "Lost words", .created = 0};
            lost.setPassage({.from = {}, .to = {}, .quote = "words this book never had", .before = {}, .after = {}});
            lost.lost = true;
            QVERIFY(desk.backend->library()->addAnnotation(ids.first(), lost) > 0);
            notes->refresh();
            for (const QSize &size : sizes()) {
                QVERIFY(desk.resize(size));
                look(desk, "notes");
                if (desk.reader->format()->noteWidth() <= 0) {
                    QQuickItem *mark = desk.item(QString("noteMark_%1").arg(notes->notes().first().toMap().value("id").toLongLong()));
                    if (mark && mark->isVisible()) {
                        QTest::mouseClick(desk.window, Qt::LeftButton, Qt::NoModifier, sceneRect(mark).center().toPoint());
                        if (QTest::qWaitFor([&] { return desk.shows("noteCard"); }, 1000))
                            look(desk, "notes-card");
                        QTest::keyClick(desk.window, Qt::Key_Escape);
                        desk.settle();
                    }
                }
            }
            for (const QSize &size : dialogSizes()) {
                QVERIFY(desk.resize(size));
                QMetaObject::invokeMethod(desk.find("lostNotes"), "clicked");
                QVERIFY(desk.waitForPopup());
                look(desk, "notes-lost");
                QVERIFY(desk.closePopups());
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

QTEST_MAIN(MarginNotesTests)
#include "marginnotes_tests.moc"
