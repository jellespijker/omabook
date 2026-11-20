#include "bookmarkcommands.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>
#include <expected>
#include <memory>
#include <print>
#include <tuple>

#include "library.h"
#include "settings.h"

namespace {

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

[[nodiscard]] std::expected<std::unique_ptr<Library>, QString> openLibrary(const QCommandLineParser &parser) {
    auto library = std::make_unique<Library>(parser.isSet("library") ? parser.value("library") : Settings().libraryPath());
    library->onBringingUpToDate([](const QString &said) { printLine(stderr, said); });
    QString error;
    if (!library->open(&error))
        return std::unexpected(error);
    return library;
}

[[nodiscard]] bool hasBookmark(const Library &library, qint64 book, qint64 bookmark) {
    const QList<Annotation> marks = library.annotations(book);
    return std::ranges::any_of(marks, [bookmark](const Annotation &mark) { return mark.id == bookmark; });
}

// A new bookmark, where `parser` says or else where the book was read to.
[[nodiscard]] Annotation placed(const Library &library, qint64 book, const QCommandLineParser &parser) {
    const Progress place = library.progress(book);
    Annotation bookmark;
    bookmark.kind = "bookmark";
    bookmark.section = parser.isSet("section") ? qMax(0, parser.value("section").toInt()) : place.section;
    // The start of a section given without a fraction; where it was read to when neither is given.
    bookmark.fraction = parser.isSet("section") ? 0.0 : place.fraction;
    if (parser.isSet("fraction"))
        bookmark.fraction = qBound(0.0, parser.value("fraction").toDouble(), 1.0);
    bookmark.text = parser.value("add").simplified();
    return bookmark;
}

// What the options ask of the book's bookmarks; why not, when it can't be done.
[[nodiscard]] std::expected<void, QString> change(Library &library, qint64 book, const QCommandLineParser &parser) {
    if (parser.isSet("remove")) {
        const qint64 bookmark = parser.value("remove").toLongLong();
        if (!hasBookmark(library, book, bookmark))
            return std::unexpected("That book has no bookmark " + parser.value("remove") + ".");
        library.removeAnnotation(bookmark);
    }
    if (parser.isSet("rename")) {
        const qint64 bookmark = parser.value("rename").toLongLong();
        if (!parser.isSet("as"))
            return std::unexpected(QStringLiteral("Say what to call it with --as."));
        if (!hasBookmark(library, book, bookmark) || !library.renameAnnotation(bookmark, parser.value("as")))
            return std::unexpected("That book has no bookmark " + parser.value("rename") + ".");
    }
    if (parser.isSet("add"))
        library.addAnnotation(book, placed(library, book, parser));
    return {};
}

} // namespace

int runBookmarksCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription("List a book's bookmarks, add one, rename one, or remove one.");
    parser.addHelpOption();
    parser.addOption({"json", "Print the result as JSON."});
    parser.addOption({"library",
                      "Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use).",
                      "folder"});
    parser.addPositionalArgument("id", "The book's number.");
    parser.addOption({"add", "Bookmark a place, with these words to know it by.", "text"});
    parser.addOption({"section", "The section of the new bookmark, counted from 0; where the book was read to when not given.", "number"});
    parser.addOption({"fraction", "How far into that section, from 0 to 1.", "number"});
    parser.addOption({"rename", "Give the bookmark with this number the words given with --as.", "bookmark"});
    parser.addOption({"as", "The words a renamed bookmark is known by.", "text"});
    parser.addOption({"remove", "Remove the bookmark with this number.", "bookmark"});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = "omabook bookmarks";
    parser.process(rest);
    if (parser.positionalArguments().size() != 1) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const auto library = openLibrary(parser);
    if (!library)
        return failLine(library.error());
    const QString number = parser.positionalArguments().first();
    const qint64 id = number.toLongLong();
    if (!(*library)->book(id))
        return failLine("There is no book " + number + " in the library.");
    if (const auto changed = change(**library, id, parser); !changed)
        return failLine(changed.error());
    QJsonArray array;
    for (const Annotation &note : (*library)->annotations(id)) {
        array << QJsonObject{
            {"id", note.id}, {"kind", note.kind}, {"section", note.section}, {"fraction", note.fraction}, {"text", note.text}};
        if (!parser.isSet("json"))
            printLine(stdout,
                      QString("%1\tsection %2, %3%\t%4").arg(note.id).arg(note.section).arg(qRound(note.fraction * 100)).arg(note.text));
    }
    if (parser.isSet("json"))
        printLine(stdout, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Indented)).trimmed());
    return 0;
}
