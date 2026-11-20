// A reader's annotations taken in, taken out and read by the AI (#86):
// Calibre's highlights, their notes and bookmarks arriving on the passages of
// their words; a book's annotations exported as Markdown, as Readwise's CSV
// (read back) and written into a copy of its PDF (read back as another PDF
// reader reads it); the companion given them only up to where the reader
// is, and only where the privacy rule lets them go; and the command and the
// MCP tools that list, add, change, remove and export them.

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <poppler-annotation.h>
#include <poppler-qt6.h>

#include "annotationanchors.h"
#include "annotationcontext.h"
#include "annotationedits.h"
#include "annotationexport.h"
#include "bookink.h"
#include "calibre.h"
#include "fixtures.h"
#include "highlightcolors.h"
#include "importer.h"
#include "library.h"
#include "mcpharness.h"
#include "pdfannotations.h"
#include "prompts.h"
#include "settings.h"

using Fixtures::whyNot;
using McpHarness::Machine;
using McpHarness::Server;
using namespace Qt::StringLiterals;

namespace {

const QList<Fixtures::Chapter> chapters{
    {"The Harbour", "<p>It was late in the autumn when the ferry came back to the island. The harbour master counted the crates "
                    "twice, as he always did, and wrote the numbers in a book that nobody read.</p>"},
    {"The Storm", "<p>By nightfall the wind had turned. Somewhere past the point the lighthouse went dark, and nobody on the quay "
                  "said a word about it until morning.</p>"},
    {"The Morning", "<p>The keeper was found asleep at the foot of the stairs, and the lamp had been put out by hand.</p>"}};

// The rows of a CSV, each its fields, quoted ones with their quotes undone (RFC 4180).
QList<QStringList> csvRows(const QString &csv) {
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < csv.size(); ++i) {
        const QChar c = csv.at(i);
        if (quoted) {
            if (c == '"' && i + 1 < csv.size() && csv.at(i + 1) == '"') {
                field += '"';
                ++i;
            } else if (c == '"') {
                quoted = false;
            } else {
                field += c;
            }
        } else if (c == '"') {
            quoted = true;
        } else if (c == ',') {
            row << field;
            field.clear();
        } else if (c == '\n') {
            row << field;
            rows << row;
            row.clear();
            field.clear();
        } else if (c != '\r') {
            field += c;
        }
    }
    return rows;
}

// A library with the three chapters as an EPUB, read to the middle of the second.
struct Shelf {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    qint64 book = 0;

    Shelf() {
        if (!Fixtures::chapteredEpub(dir.filePath("Ferry.epub"), "The Ferry", chapters) || !library.open())
            qFatal("no shelf");
        book = library.add(dir.filePath("Ferry.epub")).id;
        library.setProgress(book, {.place = {.section = 1, .fraction = 0.5}, .percent = 50, .updated = 0});
    }

    qint64 add(const QString &kind, int section, const QString &words, const QString &note = {}, const QString &color = {}) {
        const auto made = AnnotationEdits::add(
            library, book, {.kind = kind, .place = {.section = section, .fraction = 0}, .words = words, .color = color, .note = note});
        if (!made)
            qFatal("%s", qPrintable(made.error()));
        return *made;
    }
};

[[nodiscard]] QByteArray hashOf(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256) : QByteArray();
}

[[nodiscard]] QString settingsFile(const QTemporaryDir &dir, const QJsonObject &settings) {
    const QByteArray json = QJsonDocument(settings).toJson();
    const QString path = dir.filePath("config-" + QString::number(qHash(json)) + ".json");
    Fixtures::write(path, json);
    return path;
}

} // namespace

class AnnotationsTests : public QObject {
    Q_OBJECT

private slots:
    // Calibre's viewer keeps a highlight as its words, its chapter and its
    // style, a note on it inside it, and a bookmark as a place. They come
    // across once each, the highlight in its colour and its note as a margin
    // note on the same words, and are found by their words in the book's file.
    void calibreAnnotationsArriveOnTheirPassages() {
        QTemporaryDir dir;
        const QString calibre = dir.filePath("Calibre");
        QVERIFY(Fixtures::calibreLibrary(calibre));
        QVERIFY(
            Fixtures::chapteredEpub(calibre + "/Writer, Bo/Shelved Wrong (1)/Shelved Wrong - Writer, Bo.epub", "Shelved Wrong", chapters));
        QVERIFY(Fixtures::calibreChange(
            calibre,
            {"INSERT INTO annotations VALUES (1, 1, 'EPUB', 'local', 'viewer', 1700003000.0, 'h-a', 'highlight', "
             "'{\"type\": \"highlight\", \"highlighted_text\": \"the lighthouse went dark\", \"notes\": \"Why dark?\", "
             "\"spine_index\": 1, \"style\": {\"type\": \"builtin\", \"kind\": \"color\", \"which\": \"red\"}}', ''), "
             "(2, 1, 'EPUB', 'local', 'viewer', 1700003100.0, 'h-b', 'highlight', "
             "'{\"type\": \"highlight\", \"highlighted_text\": \"counted the crates twice\", \"spine_index\": 0, "
             "\"style\": {\"type\": \"builtin\", \"kind\": \"color\", \"which\": \"green\"}}', ''), "
             "(3, 1, 'EPUB', 'local', 'viewer', 1700003200.0, 'Stairs', 'bookmark', "
             "'{\"type\": \"bookmark\", \"title\": \"Stairs\", \"pos\": \"epubcfi(/6/2/4/1:0)\", \"pos_type\": \"epubcfi\"}', '')"}));
        QCOMPARE(Calibre::colorOf(QJsonObject{{"type", "builtin"}, {"kind", "color"}, {"which", "purple"}}), u"purple"_s);
        QCOMPARE(Calibre::colorOf(QJsonObject{{"type", "builtin"}, {"kind", "decoration"}, {"which", "wavy"}}), QString());

        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const ImportSummary summary = importBooks(library, {calibre});
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        qint64 book = 0;
        for (const Book &one : library.books())
            if (one.meta.title == "Calibre's Title")
                book = one.id;
        QVERIFY(book > 0);
        QCOMPARE(library.annotations(book).size(), 4);
        // When the annotations are first listed (or the book opened), each passage is found by its words.
        const auto anchored = AnnotationAnchors::anchor(library, book);
        QVERIFY2(anchored, qPrintable(whyNot(anchored)));
        QCOMPARE(anchored->lost, 0);
        QHash<QString, Annotation> by;
        for (const Annotation &annotation : library.annotations(book))
            by.insert(annotation.kind + ":" + annotation.passage().quote + annotation.text, annotation);
        const Annotation dark = by.value("highlight:the lighthouse went dark");
        QCOMPARE(dark.color, u"orange"_s);
        QCOMPARE(dark.place.section, 1);
        QVERIFY(dark.place.fraction > 0.2);
        QVERIFY(!dark.lost);
        QCOMPARE(dark.created, qint64(1700003000));
        const Annotation why = by.value("margin-note:the lighthouse went darkthe lighthouse went dark");
        QCOMPARE(why.note, u"Why dark?"_s);
        QCOMPARE(why.place, dark.place);
        QCOMPARE(by.value("highlight:counted the crates twice").color, u"green"_s);
        QCOMPARE(by.value("bookmark:Stairs").place.section, 2);

        // Again: nothing more, nothing back.
        QCOMPARE(importBooks(library, {calibre}).added, 0);
        QCOMPARE(library.annotations(book).size(), 4);
    }

    // A highlight and the note on its words are one passage in an export;
    // a note on words not highlighted is one of its own. The Markdown has a
    // heading for each chapter, and the CSV reads back as Readwise reads it,
    // quotes, commas and line breaks and all.
    void markdownAndReadwiseSayEachPassageOnce() {
        Shelf shelf;
        shelf.add("highlight", 0, "counted the crates twice", {}, "blue");
        shelf.add("margin-note", 0, "counted the crates twice", "He said \"no\", twice.\nThen left.");
        shelf.add("margin-note", 1, "the lighthouse went dark", "Who did it?");
        AnnotationEdits::Wanted mark{
            .kind = "bookmark", .place = {.section = 2, .fraction = 0}, .words = "Stairs", .color = {}, .note = {}};
        QVERIFY(AnnotationEdits::add(shelf.library, shelf.book, mark));

        const QList<AnnotationExport::Marked> books{AnnotationExport::of(shelf.library, shelf.book)};
        const QString markdown = AnnotationExport::markdown(books, {{"blue", "Questions"}});
        QVERIFY2(markdown.startsWith("# The Ferry\n"), qPrintable(markdown));
        for (const QString &part :
             {u"## The Harbour\n\n> counted the crates twice\n\nHe said \"no\", twice.\nThen left.\n\n*blue · Questions*"_s,
              u"## The Storm\n\n> the lighthouse went dark\n\nWho did it?"_s, u"## The Morning\n\n- Bookmark: Stairs"_s})
            QVERIFY2(markdown.contains(part), qPrintable(markdown));
        QCOMPARE(markdown.count("> counted the crates twice"), 1);

        const QList<QStringList> rows = csvRows(QString::fromUtf8(AnnotationExport::readwise(books)));
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0), (QStringList{"Highlight", "Title", "Author", "URL", "Note", "Location", "Date"}));
        QCOMPARE(rows.at(1).mid(0, 6),
                 (QStringList{"counted the crates twice", "The Ferry", rows.at(1).at(2), "", "He said \"no\", twice.\nThen left.", "1"}));
        QVERIFY2(QRegularExpression("^\\d{4}-\\d\\d-\\d\\d \\d\\d:\\d\\d:\\d\\d$").match(rows.at(1).at(6)).hasMatch(),
                 qPrintable(rows.at(1).at(6)));
        QCOMPARE(rows.at(2).at(0), u"the lighthouse went dark"_s);
        QCOMPARE(rows.at(2).at(4), u"Who did it?"_s);
        QCOMPARE(rows.at(2).at(5), u"2"_s);

        // The library's: every book with annotations, by title.
        QVERIFY(Fixtures::chapteredEpub(shelf.dir.filePath("Atlas.epub"), "An Atlas", {{"One", "<p>Maps of the coast.</p>"}}));
        const qint64 atlas = shelf.library.add(shelf.dir.filePath("Atlas.epub")).id;
        QVERIFY(AnnotationEdits::add(shelf.library, atlas,
                                     {.kind = "highlight", .place = {}, .words = "Maps of the coast", .color = {}, .note = {}}));
        const QString all = AnnotationExport::markdown(AnnotationExport::library(shelf.library));
        QVERIFY2(all.indexOf("# An Atlas") >= 0 && all.indexOf("# An Atlas") < all.indexOf("# The Ferry"), qPrintable(all));
    }

    // Written into a copy of the PDF as standard annotations, read back the
    // way another PDF reader reads them: the highlight over its words in its
    // colour, the note beside its words, the page's ink as ink. The book's own
    // file is untouched, and a book that isn't a PDF says so.
    void aPdfGetsItsAnnotationsWrittenIn() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::outlinedPdf(dir.filePath("Guide.pdf"), "The Guide", 3, {{"Start", 0}}));
        Library library(dir.filePath("Library"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("Guide.pdf")).id;
        const auto add = [&](const QString &kind, const QString &words, const QString &note) {
            return AnnotationEdits::add(
                library, book, {.kind = kind, .place = {.section = 1, .fraction = 0}, .words = words, .color = "green", .note = note});
        };
        QVERIFY(add("highlight", "Line 3 of page 2", {}));
        QVERIFY(add("margin-note", "Line 5 of page 2", "See the map."));
        const QString file = AnnotationAnchors::readableFile(library, book)->path;
        const QByteArray before = hashOf(file);
        Ink::Stroke stroke{.id = "s1", .color = Qt::red, .width = 3, .pressured = false, .tilted = false, .began = 1, .points = {}};
        for (int i = 0; i <= 10; ++i)
            stroke.points.push_back(
                {.at = {100.0 + (i * 50), 300}, .pressure = 0.5F, .xTilt = 0, .yTilt = 0, .ms = static_cast<quint32>(i)});
        QVERIFY(BookInk::write(library, book, 2, {.added = {stroke}, .removed = {}}, AnnotationAnchors::readableFile(library, book)->hash));

        const QString output = dir.filePath("Guide annotated.pdf");
        const auto written = PdfAnnotations::write(library, book, output);
        QVERIFY2(written, qPrintable(whyNot(written)));
        QCOMPARE(*written, 3);
        QCOMPARE(hashOf(file), before);
        QVERIFY(!PdfAnnotations::write(library, book, file));

        const std::unique_ptr<Poppler::Document> read = Poppler::Document::load(output);
        QVERIFY(read);
        QVERIFY(read->page(0)->annotations().empty());
        const auto onPage = read->page(1)->annotations();
        QCOMPARE(onPage.size(), size_t(2));
        QCOMPARE(onPage.at(0)->subType(), Poppler::Annotation::AHighlight);
        QCOMPARE(onPage.at(1)->subType(), Poppler::Annotation::AText);
        const auto *highlight = static_cast<const Poppler::HighlightAnnotation *>(onPage.at(0).get());
        const auto *note = static_cast<const Poppler::TextAnnotation *>(onPage.at(1).get());
        QCOMPARE(highlight->highlightQuads().size(), 1);
        const QColor green = HighlightColors::of(u"green").color, drawn = highlight->style().color();
        QVERIFY2(qAbs(drawn.red() - green.red()) <= 2 && qAbs(drawn.green() - green.green()) <= 2 && qAbs(drawn.blue() - green.blue()) <= 2,
                 qPrintable(drawn.name()));
        // Over the words: Line 3 is the third line of text under the page's heading.
        const QList<QRectF> words = read->page(1)->search(u"Line 3 of page 2"_s);
        QCOMPARE(words.size(), 1);
        // Poppler finds words in points; the highlight's boundary is a share of the page.
        const QSizeF size = read->page(1)->pageSizeF();
        const QRectF expected(words.first().x() / size.width(), words.first().y() / size.height(), words.first().width() / size.width(),
                              words.first().height() / size.height());
        QVERIFY(words.first().width() > 1);
        QVERIFY2(highlight->boundary().intersects(expected), qPrintable(QString("%1 %2").arg(highlight->boundary().y()).arg(expected.y())));
        QCOMPARE(note->contents(), u"See the map."_s);
        const auto inked = read->page(2)->annotations();
        QCOMPARE(inked.size(), size_t(1));
        QCOMPARE(inked.at(0)->subType(), Poppler::Annotation::AInk);
        const auto *ink = static_cast<const Poppler::InkAnnotation *>(inked.at(0).get());
        QCOMPARE(ink->inkPaths().size(), 1);
        QCOMPARE(ink->inkPaths().first().size(), 11);
        QVERIFY(qAbs(ink->inkPaths().first().first().x() - 0.1) < 0.01);

        Shelf epub;
        const auto refused = PdfAnnotations::write(epub.library, epub.book, dir.filePath("No.pdf"));
        QVERIFY(!refused && refused.error().contains("PDF"));
    }

    // What the companion is given of one's annotations stops at one's place:
    // nothing past it, nor a lost one; the place is the earlier of the
    // reader's and the last section asked about.
    void theAiIsToldNothingPastTheReadersPlace() {
        Shelf shelf;
        shelf.add("highlight", 0, "counted the crates twice", {}, "blue");
        shelf.add("margin-note", 1, "By nightfall the wind had turned", "The turn.");
        shelf.add("highlight", 1, "nobody on the quay said a word");
        shelf.add("highlight", 2, "the lamp had been put out by hand");
        const QList<Annotation> annotations = shelf.library.annotations(shelf.book);
        const BookPlace reader = shelf.library.progress(shelf.book).place;
        QCOMPARE(reader, (BookPlace{.section = 1, .fraction = 0.5}));

        const QString lines = AnnotationContext::lines(annotations, reader, {{"blue", "Questions"}});
        QVERIFY2(lines.contains("counted the crates twice") && lines.contains("blue \"Questions\""), qPrintable(lines));
        QVERIFY2(lines.contains("margin note") && lines.contains("The turn."), qPrintable(lines));
        QVERIFY2(!lines.contains("nobody on the quay"), qPrintable(lines));
        QVERIFY2(!lines.contains("put out by hand"), qPrintable(lines));
        QCOMPARE(lines.count('\n'), 1);
        QCOMPARE(std::ranges::count_if(annotations, [&](const Annotation &one) { return AnnotationContext::reached(one, reader); }), 2);

        // Asked up to the first section only, the second's are not told either.
        QCOMPARE(AnnotationContext::cutFor(shelf.library, shelf.book, 0), (BookPlace{.section = 0, .fraction = 1}));
        QCOMPARE(AnnotationContext::cutFor(shelf.library, shelf.book, 5), reader);
        QCOMPARE(AnnotationContext::cutFor(shelf.library, shelf.book), reader);
        QVERIFY(!AnnotationContext::lines(annotations, AnnotationContext::cutFor(shelf.library, shelf.book, 0)).contains("The turn."));

        // A lost one is not told: its place is another file's.
        Annotation lost = annotations.first();
        lost.lost = true;
        QVERIFY(AnnotationContext::lines({lost}, reader).isEmpty());

        // In the question, marked as the reader's and as data.
        const QString asked = Prompts::question("text", {}, "What did I highlight?", {}, lines);
        QVERIFY(asked.contains("<reader_annotations>\n" + lines + "\n</reader_annotations>"));
        QVERIFY(!Prompts::question("text", {}, "What?").contains("reader_annotations"));
    }

    // One's annotations go to one's own servers; to someone else's computer
    // (a hosted server, an agent's harness) only when hosted servers are
    // allowed in the settings, as handwriting and speech do.
    void annotationsGoOnlyWhereThePrivacyRuleLetsThem() {
        Shelf shelf;
        shelf.add("highlight", 0, "counted the crates twice");
        const BookPlace reader = shelf.library.progress(shelf.book).place;
        const QTemporaryDir dir;
        const QJsonObject ollama{{"ai_provider", "openai"}, {"ai_endpoint", QJsonObject{{"preset", "ollama"}}}};
        const Settings local(settingsFile(dir, ollama));
        QVERIFY2(AnnotationContext::whyNot(local).isEmpty(), qPrintable(AnnotationContext::whyNot(local)));
        QVERIFY(AnnotationContext::forQuestion(shelf.library, shelf.book, reader, local).contains("counted the crates twice"));

        QJsonObject elsewhere{{"ai_provider", "openai"},
                              {"ai_endpoint", QJsonObject{{"preset", "custom"}, {"base_url", "https://api.example.com/v1"}}}};
        const Settings hosted(settingsFile(dir, elsewhere));
        QVERIFY2(AnnotationContext::whyNot(hosted).contains("someone else's computer"), qPrintable(AnnotationContext::whyNot(hosted)));
        QVERIFY(AnnotationContext::forQuestion(shelf.library, shelf.book, reader, hosted).isEmpty());
        elsewhere.insert("recognition", QJsonObject{{"hosted", true}});
        const Settings allowed(settingsFile(dir, elsewhere));
        QVERIFY(AnnotationContext::whyNot(allowed).isEmpty());
        // An address marked as one's own is one's own.
        elsewhere.insert("recognition", QJsonObject{{"own", QJsonArray{"api.example.com"}}});
        const Settings own(settingsFile(dir, elsewhere));
        QVERIFY(AnnotationContext::whyNot(own).isEmpty());
        // A command or an agent's harness sends them who knows where.
        const Settings command(settingsFile(dir, {{"ai_provider", "command"}, {"ai_command", "cat"}}));
        QVERIFY(!AnnotationContext::whyNot(command).isEmpty());
        const Settings off(settingsFile(dir, {{"ai_provider", "off"}}));
        QVERIFY(!AnnotationContext::whyNot(off).isEmpty());
    }

    // Typed rather than selected: on words of the book, found near the
    // section given; words that aren't there are refused, and what may
    // change of one is its note, a highlight's colour and a bookmark's name.
    void annotationsAreAddedOnWordsAndChanged() {
        Shelf shelf;
        // The colour by the name the reader gave it.
        const auto added = AnnotationEdits::add(shelf.library, shelf.book,
                                                {.kind = "highlight",
                                                 .place = {.section = 1, .fraction = 0},
                                                 .words = "the LIGHTHOUSE went   dark",
                                                 .color = "questions",
                                                 .note = {}},
                                                {{"blue", "Questions"}});
        QVERIFY2(added, qPrintable(whyNot(added)));
        const qint64 made = *added;
        const Annotation highlight = *AnnotationEdits::find(shelf.library, shelf.book, made);
        QCOMPARE(highlight.color, u"blue"_s);
        QCOMPARE(highlight.quote, u"the lighthouse went dark"_s);
        QCOMPARE(highlight.place.section, 1);
        QVERIFY(!highlight.anchored.isEmpty());
        QVERIFY(!AnnotationEdits::add(shelf.library, shelf.book,
                                      {.kind = "highlight", .place = {}, .words = "a whale", .color = {}, .note = {}}));
        QVERIFY(!AnnotationEdits::add(shelf.library, shelf.book,
                                      {.kind = "margin-note", .place = {}, .words = "the ferry", .color = {}, .note = {}}));
        QVERIFY(!AnnotationEdits::add(shelf.library, shelf.book,
                                      {.kind = "scribble", .place = {}, .words = "the ferry", .color = {}, .note = {}}));
        QVERIFY(!AnnotationEdits::add(shelf.library, shelf.book,
                                      {.kind = "highlight", .place = {}, .words = "the ferry", .color = "mauve", .note = {}}));

        const auto recoloured = AnnotationEdits::change(shelf.library, shelf.book, made, {.note = {}, .color = u"purple"_s, .name = {}});
        QVERIFY2(recoloured, qPrintable(whyNot(recoloured)));
        QCOMPARE(recoloured->color, u"purple"_s);
        QVERIFY(!AnnotationEdits::change(shelf.library, shelf.book, made, {.note = {}, .color = {}, .name = u"x"_s}));
        const qint64 note = shelf.add("margin-note", 0, "the ferry came back", "Home.");
        QVERIFY(!AnnotationEdits::change(shelf.library, shelf.book, note, {.note = u" "_s, .color = {}, .name = {}}));
        QVERIFY(!AnnotationEdits::change(shelf.library, shelf.book, note, {.note = {}, .color = u"blue"_s, .name = {}}));
        QCOMPARE(AnnotationEdits::change(shelf.library, shelf.book, note, {.note = u"Back home."_s, .color = {}, .name = {}})->note,
                 u"Back home."_s);
        QVERIFY(AnnotationEdits::remove(shelf.library, shelf.book, note));
        QVERIFY(!AnnotationEdits::remove(shelf.library, shelf.book, note));
        QCOMPARE(shelf.library.annotations(shelf.book).size(), 1);
    }

    // The command and the MCP tools, as a person and an agent meet them.
    void theCommandAndTheToolsListAddChangeRemoveAndExport() {
        const Machine machine;
        qint64 book = 0;
        {
            Library library(machine.library);
            QVERIFY(library.open());
            QVERIFY(Fixtures::chapteredEpub(machine.dir.filePath("Ferry.epub"), "The Ferry", chapters));
            book = library.add(machine.dir.filePath("Ferry.epub")).id;
        }
        const QString id = QString::number(book);
        const auto ran = machine.omabook({"annotations", id, "--add", "highlight", "--section", "1", "--words", "the lighthouse went dark",
                                          "--color", "orange", "--json", "--library", machine.library});
        QVERIFY2(ran.code == 0, qPrintable(ran.errors));
        const QJsonObject added = QJsonDocument::fromJson(ran.output.toUtf8()).object().value("annotation").toObject();
        QCOMPARE(added.value("color").toString(), u"orange"_s);
        QCOMPARE(added.value("book_id").toInteger(), book);
        const auto listed = machine.omabook({"annotations", "all", "--library", machine.library});
        QVERIFY2(listed.output.contains("highlight") && listed.output.contains("the lighthouse went dark"), qPrintable(listed.output));
        QCOMPARE(machine.omabook({"annotations", id, "--add", "highlight", "--words", "a whale", "--library", machine.library}).code, 1);

        Server server(machine);
        const QJsonObject note = server.data(
            "annotation_add",
            {{"id", book}, {"kind", "margin-note"}, {"section", 0}, {"words", "counted the crates twice"}, {"note", "Why twice?"}});
        const qint64 noteId = note.value("annotation").toObject().value("id").toInteger();
        QVERIFY2(noteId > 0, qPrintable(server.noise().join('\n')));
        QVERIFY(server.failure("annotation_add", {{"id", book}, {"kind", "highlight"}, {"section", 0}, {"words", "a whale"}})
                    .contains("aren't in the book"));
        QJsonArray all = server.data("annotations_list", {{"id", book}}).value("annotations").toArray();
        QCOMPARE(all.size(), 2);
        QCOMPARE(server.data("annotations_list", {{"kind", "margin-note"}}).value("annotations").toArray().size(), 1);
        QCOMPARE(server.data("annotations_list", {{"id", book}, {"color", "orange"}}).value("annotations").toArray().size(), 1);

        const QJsonObject changed = server.data("annotation_change", {{"id", book}, {"annotation_id", noteId}, {"note", "Habit."}});
        QCOMPARE(changed.value("annotation").toObject().value("note").toString(), u"Habit."_s);
        const QString markdown = server.data("annotations_export", {{"id", book}, {"format", "markdown"}}).value("text").toString();
        QVERIFY2(markdown.contains("> counted the crates twice\n\nHabit.") && markdown.contains("## The Storm"), qPrintable(markdown));
        const QString csv = machine.dir.filePath("ferry.csv");
        QVERIFY(server.data("annotations_export", {{"format", "readwise"}, {"output_path", csv}}).contains("written"));
        QFile written(csv);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(csvRows(QString::fromUtf8(written.readAll())).size(), 3);
        QVERIFY(server.failure("annotations_export", {{"id", book}, {"format", "pdf"}, {"output_path", csv + ".pdf"}}).contains("PDF"));

        QCOMPARE(server.data("annotation_remove", {{"id", book}, {"annotation_id", noteId}}).value("removed").toInteger(), noteId);
        QVERIFY(server.failure("annotation_remove", {{"id", book}, {"annotation_id", noteId}}).contains("no annotation"));
        QCOMPARE(server.data("annotations_list", {{"id", book}}).value("annotations").toArray().size(), 1);
    }
};

QTEST_MAIN(AnnotationsTests)
#include "annotations_tests.moc"
