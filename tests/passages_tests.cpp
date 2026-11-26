// Passages found again by their words (#80): a highlight keeps the words it
// is on, with a few before and after, and its places; when the book's file
// changes (a new edition, a typo fixed, a reflow) it is found again by its
// words first and its place second, with a confidence, and one that can't be
// found is a lost annotation, listed for the reader to place again or remove.

#include <QElapsedTimer>
#include <QPainter>
#include <QPdfWriter>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

#include "annotationanchors.h"
#include "backend.h"
#include "booktext.h"
#include "document.h"
#include "filepicker.h"
#include "fixtures.h"
#include "formats.h"
#include "library.h"
#include "passagematch.h"
#include "reader.h"
#include "readingpassages.h"
#include "settings.h"

namespace {

const QString story = "It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                      "twice, as he always did, and wrote the numbers in a book that nobody read. Mira stood at the rail and "
                      "watched the lantern on the far pier go out, one wick at a time. She had recieved the letter three days "
                      "before, and had not opened it.";

[[nodiscard]] BookText bookOf(const QStringList &sections) {
    return BookText(static_cast<int>(sections.size()), [sections](int section) { return sections.value(section); });
}

[[nodiscard]] Passage marked(const QString &text, int section, const QString &words) {
    const qsizetype at = text.indexOf(words);
    return Passage::of(text, section, at, at + words.size());
}

// The words a passage's places point to in a book's text.
[[nodiscard]] QString wordsAt(const BookText &text, const Passage &passage) {
    const qsizetype from = text.offsetOf(passage.from);
    return text.text(passage.from.section).mid(from, text.offsetOf(passage.to) - from);
}

// The first section of a book that holds these words, however its lines
// break them, and where in it: {section, start, end}; section -1 when none does.
struct Found {
    int section = -1;
    qsizetype start = 0, end = 0;
};

[[nodiscard]] Found findWords(const BookText &text, const QString &words) {
    const QRegularExpression pattern(QRegularExpression::escape(words).replace("\\ ", "\\s+"));
    for (int section = 0; section < text.sections(); ++section)
        if (const QRegularExpressionMatch match = pattern.match(text.text(section)); match.hasMatch())
            return {.section = section, .start = match.capturedStart(), .end = match.capturedEnd()};
    return {};
}

[[nodiscard]] Passage passageOn(const BookText &text, const QString &words) {
    const Found found = findWords(text, words);
    return Passage::of(text.text(found.section), found.section, found.start, found.end);
}

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
    // A PDF's lines break where the page is full, wherever that is in a passage.
    QPdfWriter writer(path);
    writer.setTitle("The Ferry");
    writer.setPageSize(QPageSize(QPageSize::A5));
    QPainter painter(&writer);
    painter.drawText(QRect(300, 300, 5000, 8000), Qt::TextWordWrap, paragraphs.join("\n\n"));
    return painter.end();
}

const QStringList firstEdition{"It was late in the autumn when the ferry came back to the island.",
                               "Mira stood at the rail and watched the lantern on the far pier go out, one wick at a time.",
                               "She had recieved the letter three days before, and had not opened it.",
                               "The gulls were quiet that evening over the grey water of the bay."};
// A foreword put in, a typo fixed, the last paragraph taken out.
const QStringList secondEdition{"A foreword was written for this edition by somebody else entirely.",
                                "It was late in the autumn when the ferry came back to the island.",
                                "Mira stood at the rail and watched the lantern on the far pier go out, one wick at a time.",
                                "She had received the letter three days before, and had not opened it."};

class NoPicker : public FilePicker {
public:
    void pick(Purpose, const QString &) override {}
};

} // namespace

class PassagesTests : public QObject {
    Q_OBJECT

private slots:
    void aPassageIsItsWordsAndAFewAroundThem() {
        const Passage passage = marked(story, 2, "watched the lantern on the far pier go out");
        QCOMPARE(passage.quote, QString("watched the lantern on the far pier go out"));
        QCOMPARE(passage.before, QString("nobody read. Mira stood at the rail and "));
        QCOMPARE(passage.after, QString(", one wick at a time. She had recieved"));
        QCOMPARE(passage.from.section, 2);
        QCOMPARE(passage.from.fraction, static_cast<double>(story.indexOf("watched")) / static_cast<double>(story.size()));
        QCOMPARE(passage.to.fraction, static_cast<double>(story.indexOf(", one wick")) / static_cast<double>(story.size()));
        // At the very start of a section there is nothing before it.
        QCOMPARE(marked(story, 0, "It was late").before, QString());
        QCOMPARE(marked(story, 0, "had not opened it.").after, QString());
    }

    void aPassageIsFoundWhereItWas() {
        const BookText text = bookOf({story});
        const Passage passage = marked(story, 0, "watched the lantern on the far pier go out");
        const std::optional<FoundPassage> found = passage.findAgain(text);
        QVERIFY(found);
        QCOMPARE(found->passage, passage);
        QCOMPARE(found->confidence, 1.0);
    }

    void aPassageIsFoundAgainInAnEditedText_data() {
        QTest::addColumn<QString>("words");
        QTest::addColumn<QString>("edited");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<double>("least");
        QTest::addColumn<double>("most");
        const QString lantern = "watched the lantern on the far pier go out";
        QTest::newRow("a sentence put in before it") << lantern << "A gull cried over the water. " + story << lantern << 1.0 << 1.0;
        QTest::newRow("sentences taken out before it")
            << lantern
            << QString(story).remove(
                   "The harbour master counted the crates twice, as he always did, and wrote the numbers in a book that nobody read. ")
            << lantern << 0.9 << 0.99;
        QTest::newRow("a typo fixed in it") << "She had recieved the letter three days before"
                                            << QString(story).replace("recieved", "received")
                                            << "She had received the letter three days before" << 0.85 << 0.99;
        QTest::newRow("a word put in it") << lantern << QString(story).replace("pier go out", "pier slowly go out")
                                          << "watched the lantern on the far pier slowly go out" << 0.85 << 0.99;
        QTest::newRow("a word taken out of it")
            << lantern << QString(story).replace("the far pier", "the pier") << "watched the lantern on the pier go out" << 0.85 << 0.99;
        // Laid out anew: other line breaks and spaces, a soft hyphen, another case.
        QString reflowed = story;
        reflowed.replace("the rail and watched the lantern", "the rail and\nwatched  the lan­tern").replace("far pier", "Far\tPier");
        QTest::newRow("reflowed") << lantern << reflowed << "watched  the lan­tern on the Far\tPier go out" << 1.0 << 1.0;
    }

    void aPassageIsFoundAgainInAnEditedText() {
        QFETCH(QString, words);
        QFETCH(QString, edited);
        QFETCH(QString, expected);
        QFETCH(double, least);
        QFETCH(double, most);
        const BookText text = bookOf({edited});
        const std::optional<FoundPassage> found = marked(story, 0, words).findAgain(text);
        QVERIFY(found);
        QCOMPARE(found->passage.quote, expected);
        QCOMPARE(wordsAt(text, found->passage), expected);
        QCOMPARE(found->passage.from.fraction, static_cast<double>(edited.indexOf(expected)) / static_cast<double>(edited.size()));
        QVERIFY2(found->confidence >= least && found->confidence <= most, qPrintable(QString::number(found->confidence)));
    }

    // The same words twice: the words around them say which, and without
    // any, the place it was at.
    void theWordsAroundItOrItsPlaceTellOneOccurrenceFromAnother() {
        const QString text = "The bell rang at noon. Nobody came to the door. The bell rang at noon. The postman left a parcel.";
        const qsizetype second = text.lastIndexOf("The bell rang");
        const Passage passage = Passage::of(text, 0, second, second + 13);
        const std::optional<FoundPassage> found = passage.findAgain(bookOf({"A line put in. " + text}));
        QVERIFY(found);
        QCOMPARE(found->passage.before, QString("rang at noon. Nobody came to the door. "));
        QCOMPARE(found->confidence, 1.0);

        Passage bare{.from = passage.from, .to = passage.to, .quote = "The bell rang", .before = {}, .after = {}};
        QCOMPARE(bare.findAgain(bookOf({text}))->passage.from, passage.from);
        bare.from.fraction = 0;
        QCOMPARE(bare.findAgain(bookOf({text}))->passage.from.fraction, 0.0);
    }

    // A preface put in before it: the section it is in now, the nearest first.
    void aPassageIsFoundInTheSectionItWentTo() {
        const Passage passage = marked(story, 1, "watched the lantern on the far pier go out");
        const BookText after = bookOf({"A preface.", "Chapter one. The ferry.", story});
        const std::optional<FoundPassage> found = passage.findAgain(after);
        QVERIFY(found);
        QCOMPARE(found->passage.from.section, 2);
        QCOMPARE(found->confidence, 1.0);
        QCOMPARE(wordsAt(after, found->passage), passage.quote);
    }

    void aPassageWhoseWordsAreGoneIsNotFound() {
        const Passage passage = marked(story, 0, "watched the lantern on the far pier go out");
        QVERIFY(!passage.findAgain(bookOf({"It was late in the autumn when the ferry came back to the island."})));
        QVERIFY(!passage.findAgain(bookOf({})));
        // One word, changed, with nothing around it to go by.
        const Passage word{.from = {}, .to = {}, .quote = "recieved", .before = {}, .after = {}};
        QVERIFY(!word.findAgain(bookOf({QString(story).replace("recieved", "received")})));
        // No words at all is nothing to find.
        QVERIFY(!Passage{}.findAgain(bookOf({story})));
    }

    // The reader's page shows the section with more around it (a title, its own spacing).
    void aPassageIsFoundOnThePageThatShowsIt() {
        const Passage passage = marked(story, 0, "watched the lantern on the far pier go out");
        const QString shown = "One\n\n" + QString(story).replace(". ", ".\n");
        const auto span = passage.spanIn(shown);
        QVERIFY(span);
        QCOMPARE(shown.mid(span->first, span->second - span->first), passage.quote);
        QVERIFY(!passage.spanIn(u"Nothing of it here."));
    }

    void wordsAreLettersAndDigitsAndEachCharacterOfWritingWithoutSpaces() {
        const QString text = "Don't stop\u2014now, 42 lan\u00ADterns. \u6211\u559C\u6B22\u8BFB\u4E66";
        const QList<PassageMatch::Word> words = PassageMatch::words(text);
        QStringList keys;
        for (const PassageMatch::Word &word : words)
            keys << text.mid(word.start, word.end - word.start);
        QCOMPARE(keys, (QStringList{"Don", "t", "stop", "now", "42", QString("lan\u00ADterns"), "我", "喜", "欢", "读", "书"}));
        const QString chinese = "他说我喜欢读书和写字";
        const auto found = marked(chinese, 0, "喜欢读书").findAgain(bookOf({"今天" + chinese}));
        QVERIFY(found);
        QCOMPARE(found->passage.quote, QString("喜欢读书"));
    }

    // In every format text is read from: a highlight found again in a new
    // edition of the book's file, moved where the edition moved it, its words
    // the edition's, and one whose words the edition took out lost, listed,
    // and placed again where the reader says.
    void aNewEditionOfTheFileMovesItsHighlightsAndLosesWhatItTookOut_data() {
        QTest::addColumn<QString>("format");
        for (const char *format : {"epub", "fb2", "md", "html", "txt", "pdf"})
            QTest::newRow(format) << QString(format);
    }

    void aNewEditionOfTheFileMovesItsHighlightsAndLosesWhatItTookOut() {
        QFETCH(QString, format);
        QTemporaryDir dir;
        const QString first = dir.filePath("first/The Ferry." + format), second = dir.filePath("second/The Ferry." + format);
        QVERIFY(QDir().mkpath(dir.filePath("first")) && QDir().mkpath(dir.filePath("second")));
        QVERIFY(writeBook(first, firstEdition) && writeBook(second, secondEdition));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(first).id;
        QVERIFY(id > 0);
        const std::optional<FormatFile> file = AnnotationAnchors::readableFile(library, id);
        QVERIFY(file);
        const std::unique_ptr<Document> opened = Formats::open(file->path);
        QVERIFY(opened);
        const BookText text(*opened);
        for (const QString &words : {"watched the lantern on the far pier go out", "She had recieved the letter three days before",
                                     "The gulls were quiet that evening"}) {
            QVERIFY2(findWords(text, words).section >= 0, qPrintable(words + " in " + text.text(0)));
            Annotation highlight{.id = 0, .kind = "highlight", .place = {}, .text = words, .note = "A note", .created = 0};
            highlight.setPassage(passageOn(text, words));
            highlight.anchored = file->hash;
            QVERIFY(library.addAnnotation(id, highlight) > 0);
        }
        QVERIFY(!AnnotationAnchors::needed(library, id, file->hash));
        QCOMPARE(*AnnotationAnchors::anchor(library, id), AnnotationAnchors::Summary{});

        // The new edition replaces the file.
        QVERIFY(library.addFormat(id, second, Library::SameFormat::Overwrite));
        const std::optional<FormatFile> edition = AnnotationAnchors::readableFile(library, id);
        QVERIFY(edition && edition->hash != file->hash);
        const auto summary = AnnotationAnchors::anchor(library, id);
        QVERIFY2(summary, qPrintable(Fixtures::whyNot(summary)));
        QCOMPARE(*summary, (AnnotationAnchors::Summary{.kept = 0, .moved = 2, .lost = 1}));
        const std::unique_ptr<Document> reopened = Formats::open(edition->path);
        QVERIFY(reopened);
        const BookText now(*reopened);
        QStringList quotes;
        for (const Annotation &annotation : library.annotations(id)) {
            QCOMPARE(annotation.anchored, edition->hash);
            QCOMPARE(annotation.note, QString("A note"));
            if (annotation.lost)
                continue;
            QCOMPARE(wordsAt(now, annotation.passage()).simplified(), annotation.quote.simplified());
            QVERIFY(annotation.confidence >= 0.8);
            quotes << annotation.quote.simplified();
        }
        QCOMPARE(quotes, (QStringList{"watched the lantern on the far pier go out", "She had received the letter three days before"}));

        const QList<Annotation> lost = AnnotationAnchors::lost(library, id);
        QCOMPARE(lost.size(), 1);
        QCOMPARE(lost.first().quote.simplified(), QString("The gulls were quiet that evening"));
        QCOMPARE(lost.first().confidence, 0.0);
        // Nothing more to do for this file; placed again where the reader selected it, it is found.
        QVERIFY(!AnnotationAnchors::needed(library, id, edition->hash));
        QVERIFY(AnnotationAnchors::placeAgain(library, lost.first().id, passageOn(now, "the ferry came back"), edition->hash));
        QVERIFY(AnnotationAnchors::lost(library, id).isEmpty());
        QVERIFY(!AnnotationAnchors::placeAgain(library, 9999, Passage{}, edition->hash));
    }

    // A highlight from before passages is a place and its words: found by
    // them the first time its book is opened, or lost.
    void aHighlightFromBeforePassagesIsFoundByItsWords() {
        QTemporaryDir dir;
        QVERIFY(writeBook(dir.filePath("The Ferry.epub"), firstEdition));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 id = library.add(dir.filePath("The Ferry.epub")).id;
        for (const QString &words : {"watched the lantern on the far pier", "words this book never had in it"})
            QVERIFY(library.addAnnotation(id, {.id = 0, .kind = "highlight", .place = {}, .text = words, .note = {}, .created = 0}) > 0);
        QVERIFY(library.addAnnotation(id, {.id = 0, .kind = "bookmark", .place = {}, .text = "A place", .note = {}, .created = 0}) > 0);
        QCOMPARE(*AnnotationAnchors::anchor(library, id), (AnnotationAnchors::Summary{.kept = 0, .moved = 1, .lost = 1}));
        const QList<Annotation> annotations = library.annotations(id);
        const auto found = std::ranges::find(annotations, QString("watched the lantern on the far pier"), &Annotation::text);
        QVERIFY(found != annotations.cend());
        QCOMPARE(found->quote, found->text);
        QVERIFY2(found->before.endsWith("Mira stood at the rail and "), qPrintable(found->before));
        QVERIFY(found->place.fraction > 0);
        const auto bookmark = std::ranges::find(annotations, QString("bookmark"), &Annotation::kind);
        QVERIFY(bookmark->anchored.isEmpty() && !bookmark->lost);
        QCOMPARE(AnnotationAnchors::lost(library, id).first().text, QString("words this book never had in it"));
        // Once is enough.
        QCOMPARE(*AnnotationAnchors::anchor(library, id), AnnotationAnchors::Summary{});
    }

    // The reader finds them again as it opens a book, beside the window.
    void openingABookFindsItsPassagesAgain() {
        QTemporaryDir dir;
        QVERIFY(writeBook(dir.filePath("The Ferry.epub"), firstEdition));
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        NoPicker picker;
        Backend backend(&settings, &picker, nullptr, {}, {}, dir.filePath("plugins"), "-");
        Library &library = *backend.library();
        const qint64 id = library.add(dir.filePath("The Ferry.epub")).id;
        QVERIFY(
            library.addAnnotation(id, {.id = 0, .kind = "highlight", .place = {}, .text = "the ferry came back", .note = {}, .created = 0})
            > 0);
        Reader reader(&backend, {});
        ReadingPassages passages(&reader);
        QSignalSpy anchored(&passages, &ReadingPassages::anchored);
        reader.openBook(id);
        QTRY_COMPARE(anchored.size(), 1);
        QCOMPARE(anchored.first(), (QVariantList{id, 1, 0}));
        QCOMPARE(library.annotations(id).first().quote, QString("the ferry came back"));
        // Opened again, there is nothing to do.
        reader.close();
        reader.openBook(id);
        QTRY_VERIFY(reader.isOpen());
        QVERIFY(!passages.working());
        QCOMPARE(anchored.size(), 1);
    }

    // A book of forty chapters with a thousand highlights, the new edition
    // with a sentence put in at the start of every chapter, a word changed in
    // every tenth highlight and every fiftieth taken out: all found again (or
    // lost) and written within a few seconds.
    void aThousandHighlightsAreFoundAgainQuickly() {
        // Made-up words, picked as a fixed sequence would, so a sentence shares few with the next.
        quint32 seed = 80;
        const auto word = [&seed] {
            seed = (seed * 1103515245U) + 12345U;
            return QString("w%1").arg((seed >> 8) % 3000, 0, 36);
        };
        QStringList first, second;
        QList<std::pair<int, QString>> marks;
        for (int chapter = 0; chapter < 40; ++chapter) {
            QString one, two = "This edition begins the chapter anew. ";
            for (int sentence = 0; sentence < 250; ++sentence) {
                QStringList words;
                for (int i = 0; i < 12; ++i)
                    words << word();
                const QString line = words.join(' ') + ". ";
                one += line;
                const qsizetype mark = marks.size();
                const bool marked = sentence % 10 == 0;
                if (marked)
                    marks.append({chapter, line.trimmed()});
                if (marked && mark % 50 == 49)
                    continue; // taken out
                QStringList changed = words;
                changed[5] = "changed";
                two += marked && mark % 10 == 3 ? changed.join(' ') + ". " : line;
            }
            first << one;
            second << two;
        }
        QCOMPARE(marks.size(), 1000);
        QTemporaryDir dir;
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        BookMeta meta;
        meta.title = "A Thousand Marks";
        const qint64 id = library.addEntry(meta, false).id;
        QVERIFY(id > 0);
        const BookText before = bookOf(first);
        {
            const Library::Batch batch(library);
            for (const auto &[chapter, words] : std::as_const(marks)) {
                Annotation highlight{.id = 0, .kind = "highlight", .place = {}, .text = words, .note = {}, .created = 0};
                const qsizetype at = first.at(chapter).indexOf(words);
                highlight.setPassage(Passage::of(before.text(chapter), chapter, at, at + words.size()));
                highlight.anchored = "first";
                QVERIFY(library.addAnnotation(id, highlight) > 0);
            }
        }
        QElapsedTimer timer;
        timer.start();
        const AnnotationAnchors::Summary summary = AnnotationAnchors::anchor(library, id, bookOf(second), "second");
        const qint64 took = timer.elapsed();
        qInfo("1000 highlights found again in %lld ms: %d moved, %d lost", took, summary.moved, summary.lost);
        QCOMPARE(summary, (AnnotationAnchors::Summary{.kept = 0, .moved = 980, .lost = 20}));
        QVERIFY2(took < 5000, qPrintable(QString("%1 ms").arg(took)));
    }
};

QTEST_MAIN(PassagesTests)
#include "passages_tests.moc"
