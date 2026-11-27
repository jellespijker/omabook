#include "annotationcommands.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <cstdio>
#include <expected>
#include <memory>
#include <print>
#include <tuple>

#include "annotationanchors.h"
#include "annotationedits.h"
#include "annotationexport.h"
#include "annotationfacts.h"
#include "annotationlayers.h"
#include "bookops.h"
#include "highlightcolors.h"
#include "highlights.h"
#include "library.h"
#include "numberoptions.h"
#include "pdfannotations.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

void printJson(const QJsonObject &object) {
    printLine(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, QString> openLibrary(const QCommandLineParser &parser) {
    auto library = std::make_unique<Library>(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    library->onBringingUpToDate([](const QString &said) { printLine(stderr, said); });
    QString error;
    if (!library->open(&error))
        return std::unexpected(error);
    return library;
}

void describe(QCommandLineParser &parser) {
    parser.setApplicationDescription(
        "List a book's annotations (highlights, margin notes, bookmarks and book ink), or the whole library's with \"all\"; add "
        "one on words of the book, change or remove one; or export them as Markdown, as Readwise's CSV, or written into a copy "
        "of a PDF book as PDF annotations.");
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
    parser.addPositionalArgument("book", "The book's number, or \"all\" for every book that has annotations.");
    parser.addOption({"kind", "List only these: highlight, margin-note, bookmark or ink.", "kind"});
    parser.addOption({"lost", "List only the ones whose words can no longer be found in the book's file."});
    parser.addOption({"color",
                      "List only the highlights of this colour; or the colour of one added or changed. By its name (yellow, "
                      "blue, green, orange, purple) or what you call it in the reader.",
                      "colour"});
    parser.addOption({"add", "Add a highlight, a margin-note or a bookmark.", "kind"});
    parser.addOption({"section", "Where the words of the new one are, or the bookmark is: the section, counted from 0.", "number"});
    parser.addOption({"fraction", "How far into that section, from 0 to 1.", "number"});
    parser.addOption({"words", "The words of the book a new highlight or margin note is on; a new bookmark's name.", "text"});
    parser.addOption({"note", "A margin note's words, or a note kept with a highlight or bookmark.", "text"});
    parser.addOption({"change", "Change the annotation with this number: its --note, --color or --name.", "annotation"});
    parser.addOption({"name", "A bookmark's new name.", "text"});
    parser.addOption({"remove", "Remove the annotation with this number.", "annotation"});
    parser.addOption({"export", "Export them: markdown, readwise (CSV) or pdf (a copy of a PDF book with them in it).", "format"});
    parser.addOption({"output", "The file an export is written to; printed when not given (a PDF needs one).", "file"});
    parser.addOption({"others",
                      "List the others' annotations instead: those of the people you share a space with whose annotations part is "
                      "on, as far as you have read the book."});
    parser.addOption({"fact",
                      "Make the words of this annotation (a number of yours, or the id of someone else's that --others lists) a fact "
                      "of the thing --thing names, learned where it is in the book.",
                      "annotation"});
    parser.addOption({"thing", "--fact: the thing of the book's graph, by its number or name.", "thing"});
    parser.addOption({"thing-kind", "--fact: the kind of thing to make when there is none of that name (character, place, ...).", "kind"});
}

[[nodiscard]] QString wordsOf(const Annotation &annotation) {
    if (annotation.kind == "bookmark" || annotation.kind == "ink")
        return annotation.text.simplified();
    const QString quote = annotation.passage().quote.simplified();
    return annotation.note.trimmed().isEmpty() ? quote : quote + " — " + annotation.note.simplified();
}

// A line of the list: its number, kind, place and words, a highlight's colour.
[[nodiscard]] QString lineOf(qint64 bookId, const Annotation &annotation) {
    const QString color = annotation.kind == Highlights::kind ? "\t" + HighlightColors::of(annotation.color).key : QString();
    return QString("%1\t%2\tbook %3, section %4, %5%\t%6%7%8")
        .arg(annotation.id)
        .arg(annotation.kind)
        .arg(bookId)
        .arg(annotation.place.section)
        .arg(qRound(annotation.place.fraction * 100))
        .arg(wordsOf(annotation), color, annotation.lost ? u"\t(lost)"_s : QString());
}

[[nodiscard]] bool listed(const Annotation &annotation, const QCommandLineParser &parser, const QString &color) {
    if (parser.isSet("kind") && annotation.kind != parser.value("kind"))
        return false;
    if (parser.isSet("lost") && !annotation.lost)
        return false;
    return color.isEmpty() || (annotation.kind == Highlights::kind && HighlightColors::of(annotation.color).key == color);
}

int list(const Library &library, const QList<qint64> &books, const QCommandLineParser &parser) {
    const QVariantMap names = Settings().highlightNames();
    const auto color =
        parser.isSet("color") ? AnnotationEdits::colorNamed(parser.value("color"), names) : std::expected<QString, QString>(QString());
    if (!color)
        return failLine(color.error());
    QJsonArray array;
    for (const qint64 book : books) {
        for (const Annotation &annotation : library.annotations(book)) {
            if (!listed(annotation, parser, *color))
                continue;
            array << annotation.toJson({{"id", annotation.id}, {"book_id", book}});
            if (!parser.isSet("json"))
                printLine(stdout, lineOf(book, annotation));
        }
    }
    if (parser.isSet("json"))
        printJson({{"annotations", array}});
    return 0;
}

[[nodiscard]] std::expected<QByteArray, QString> exported(Library &library, const QList<qint64> &books, bool all, const QString &format) {
    const QList<AnnotationExport::Marked> marked =
        all ? AnnotationExport::library(library) : QList<AnnotationExport::Marked>{AnnotationExport::of(library, books.first())};
    if (format == "markdown")
        return AnnotationExport::markdown(marked, Settings().highlightNames()).toUtf8();
    if (format == "readwise")
        return AnnotationExport::readwise(marked);
    return std::unexpected("There is no export called " + format + "; it is markdown, readwise or pdf.");
}

int exportThem(Library &library, const QList<qint64> &books, bool all, const QCommandLineParser &parser) {
    const QString format = parser.value("export").toLower();
    const QString output = parser.value("output");
    if (format == "pdf") {
        if (all || output.isEmpty())
            return failLine(u"A PDF is written for one book, to the file given with --output."_s);
        const auto written = PdfAnnotations::write(library, books.first(), output);
        if (!written)
            return failLine(written.error());
        if (parser.isSet("json"))
            printJson({{"written", output}, {"annotations", *written}});
        else
            printLine(stdout, QString("%1 annotations written into %2").arg(*written).arg(output));
        return 0;
    }
    const auto made = exported(library, books, all, format);
    if (!made)
        return failLine(made.error());
    if (output.isEmpty()) {
        if (parser.isSet("json"))
            printJson({{"format", format}, {"text", QString::fromUtf8(*made)}});
        else
            std::ignore = fwrite(made->constData(), 1, made->size(), stdout); // What a closed terminal misses is not ours to say.
        return 0;
    }
    QSaveFile file(output);
    if (!file.open(QIODevice::WriteOnly) || file.write(*made) != made->size() || !file.commit())
        return failLine("The export can't be written to " + output + ".");
    if (parser.isSet("json"))
        printJson({{"written", output}, {"format", format}});
    return 0;
}

[[nodiscard]] std::optional<QString> option(const QCommandLineParser &parser, const QString &name) {
    return parser.isSet(name) ? std::optional(parser.value(name)) : std::nullopt;
}

// The others' annotations of the books, as far as the reader has read each.
int others(const Library &library, const QList<qint64> &books, bool all, const QCommandLineParser &parser) {
    const AnnotationLayers::Layers layers = AnnotationLayers::read(library, all ? 0 : books.first());
    if (parser.isSet("json")) {
        printJson({{"people", AnnotationLayers::toJson(layers)}, {"refused", layers.refused}});
        return 0;
    }
    for (const AnnotationLayers::Person &person : layers.people)
        for (const AnnotationLayers::Annotation &one : person.annotations)
            printLine(stdout,
                      u"%1\t%2\t%3\tbook %4, section %5, %6%\t%7"_s.arg(one.id, AnnotationLayers::calledOf(person), one.kind)
                          .arg(one.book)
                          .arg(one.place.section)
                          .arg(qRound(one.place.fraction * 100))
                          .arg(one.note.isEmpty() ? one.quote.simplified() : one.quote.simplified() + u" — "_s + one.note.simplified()));
    if (layers.refused > 0)
        printLine(stderr, u"%1 of what others sent was over the limits, and left out."_s.arg(layers.refused));
    return 0;
}

// --fact: an annotation's words made a fact of a thing of the book's graph.
int fact(const Library &library, const QCommandLineParser &parser) {
    const auto source = AnnotationFacts::of(library, parser.value("fact"));
    if (!source)
        return failLine(source.error().text());
    const auto made = AnnotationFacts::make(library, *source, {.which = parser.value("thing"), .kind = parser.value("thing-kind")});
    if (!made)
        return failLine(made.error().text());
    if (parser.isSet("json"))
        printJson({{"node", made->node}, {"fact", made->fact}, {"made_node", made->madeNode}, {"book_id", source->book}});
    else
        printLine(stdout, u"Kept as fact %1 of thing %2, learned in book %3 at section %4."_s.arg(made->fact)
                              .arg(made->node)
                              .arg(source->book)
                              .arg(source->place.section));
    return 0;
}

// What --add, --change or --remove asks of one of the book's annotations, said as it is then.
int edit(Library &library, qint64 book, const QCommandLineParser &parser) {
    const QVariantMap names = Settings().highlightNames();
    if (const auto why = Numbers::problem(parser, {{.name = "remove"}, {.name = "change"}}))
        return failLine(*why);
    if (parser.isSet("remove")) {
        const qint64 id = parser.value("remove").toLongLong();
        if (const auto removed = AnnotationEdits::remove(library, book, id); !removed)
            return failLine(removed.error());
        if (parser.isSet("json"))
            printJson({{"removed", id}});
        else
            printLine(stdout, QString("Removed annotation %1.").arg(id));
        return 0;
    }
    std::expected<Annotation, QString> now = std::unexpected(QString());
    if (parser.isSet("change")) {
        now = AnnotationEdits::change(library, book, parser.value("change").toLongLong(),
                                      {.note = option(parser, "note"), .color = option(parser, "color"), .name = option(parser, "name")},
                                      names);
    } else {
        // The start of a section given without a fraction; where the book was read to when neither is given.
        if (const auto why = Numbers::problem(parser, {Numbers::section(), Numbers::fraction()}))
            return failLine(*why);
        if (parser.isSet("section"))
            if (const auto past = BookOps::checkSection(library, book, parser.value("section").toInt()); !past)
                return failLine(past.error().text());
        BookPlace place =
            parser.isSet("section") ? BookPlace{.section = parser.value("section").toInt(), .fraction = 0} : library.progress(book).place;
        if (parser.isSet("fraction"))
            place.fraction = parser.value("fraction").toDouble();
        const auto added = AnnotationEdits::add(library, book,
                                                {.kind = parser.value("add"),
                                                 .place = place,
                                                 .words = parser.value("words"),
                                                 .color = parser.value("color"),
                                                 .note = parser.value("note")},
                                                names);
        if (!added)
            return failLine(added.error());
        now = AnnotationEdits::find(library, book, *added).value_or(Annotation{});
    }
    if (!now)
        return failLine(now.error());
    if (parser.isSet("json"))
        printJson({{"annotation", now->toJson({{"id", now->id}, {"book_id", book}})}});
    else
        printLine(stdout, lineOf(book, *now));
    return 0;
}

// The book the word names, 0 for "all"; why there is none, when it names none.
[[nodiscard]] std::expected<qint64, QString> bookAsked(const Library &library, const QString &asked) {
    if (asked == "all")
        return 0;
    if (const auto why = Numbers::notNumbers({asked}))
        return std::unexpected(*why);
    if (!library.book(asked.toLongLong()))
        return std::unexpected("There is no book " + asked + " in the library.");
    return asked.toLongLong();
}

} // namespace

int runAnnotationsCli(const QStringList &arguments) {
    QCommandLineParser parser;
    describe(parser);
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = "omabook annotations";
    parser.process(rest);
    if (parser.positionalArguments().size() != 1) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const QString asked = parser.positionalArguments().first();
    const bool all = asked == "all";
    const auto named = bookAsked(**library, asked);
    if (!named)
        return failLine(named.error());
    const qint64 id = *named;
    if (parser.isSet("fact"))
        return fact(**library, parser);
    const bool editing = parser.isSet("add") || parser.isSet("change") || parser.isSet("remove");
    if (all && editing)
        return failLine(u"Say which book: an annotation is added, changed or removed in one."_s);
    if (editing)
        return edit(**library, id, parser);
    const QList<qint64> books = all ? AnnotationExport::booksWithAnnotations(**library) : QList<qint64>{id};
    if (parser.isSet("export"))
        return exportThem(**library, books, all, parser);
    if (parser.isSet("others"))
        return others(**library, books, all, parser);
    // Passages placed in another file of the book are found again in this one first (#80); never for a sweep of the library.
    if (!all)
        std::ignore = AnnotationAnchors::anchor(**library, id); // A book without a file Omabook reads is listed as it is.
    return list(**library, books, parser);
}
