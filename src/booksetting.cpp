#include "booksetting.h"

#include <QCommandLineParser>
#include <QFile>
#include <QImageReader>
#include <QJsonDocument>
#include <QList>
#include <QPair>
#include <QStringList>
#include <array>
#include <optional>
#include <utility>

#include "bookops.h"
#include "graph.h"
#include "numberoptions.h"
#include "settings.h"

namespace {

// How each of the fields is set from its option's text.
using Setter = void (*)(BookMeta &meta, const QString &text);
constexpr std::array<std::pair<const char *, Setter>, 10> setters{{
    {"title", [](BookMeta &meta, const QString &text) { meta.title = text.simplified(); }},
    {"authors", [](BookMeta &meta, const QString &text) { meta.authors = BookMeta::splitAuthors(text); }},
    {"series", [](BookMeta &meta, const QString &text) { meta.series = text.simplified(); }},
    // Checked before (numbersOf): empty takes the number away.
    {"series-index", [](BookMeta &meta, const QString &text) { meta.seriesIndex = text.toDouble(); }},
    {"publisher", [](BookMeta &meta, const QString &text) { meta.publisher = text.simplified(); }},
    {"language", [](BookMeta &meta, const QString &text) { meta.language = text.simplified(); }},
    {"isbn", [](BookMeta &meta, const QString &text) { meta.isbn = text.simplified(); }},
    {"published", [](BookMeta &meta, const QString &text) { meta.published = text.simplified(); }},
    {"description", [](BookMeta &meta, const QString &text) { meta.description = text.trimmed(); }},
    {"tags", [](BookMeta &meta, const QString &text) { meta.tags = BookMeta::splitTags(text); }},
}};

// The fields of the details the options name, over `meta`.
[[nodiscard]] std::expected<BookMeta, Failure> metaOf(BookMeta meta, const QCommandLineParser &options) {
    if (options.isSet("meta")) {
        QFile details(options.value("meta"));
        if (!details.open(QIODevice::ReadOnly))
            return std::unexpected(Failure(Failure::Code::DetailsUnreadable, {options.value("meta")}));
        const QJsonDocument read = QJsonDocument::fromJson(details.readAll());
        if (!read.isObject())
            return std::unexpected(Failure(Failure::Code::DetailsNotAnObject, {options.value("meta")}));
        // Anything but a number from 0 reads as -1.
        if (const QJsonValue index = read.object().value("series_index"); !index.isUndefined() && index.toDouble(-1) < 0)
            return std::unexpected(Failure(Failure::Code::SeriesIndexNotANumber, {index.toVariant().toString()}));
        meta.applyJson(read.object());
    }
    for (const auto &[field, setter] : setters)
        if (options.isSet(QString::fromLatin1(field)))
            setter(meta, options.value(QString::fromLatin1(field)));
    if (options.isSet("cover")) {
        QImageReader reader(options.value("cover"));
        reader.setAutoTransform(true);
        meta.cover = reader.read();
        if (meta.cover.isNull())
            return std::unexpected(Failure(Failure::Code::PictureUnreadable, {options.value("cover")}));
    }
    if (meta.title.isEmpty())
        return std::unexpected(Failure(Failure::Code::BookNeedsTitle));
    return meta;
}

// Its shelf, stars, finished day and paper copy, as the options say them, stamped `at` (none for now).
[[nodiscard]] std::expected<void, Failure> readingOf(Library &library, const Book &book, const QCommandLineParser &options,
                                                     std::optional<qint64> at) {
    // One write, logged once whole: what a sync gives is taken as one (#243).
    const Library::Batch batch(library);
    if (options.isSet("status") || options.isSet("rating") || options.isSet("finished")) {
        const QString status = options.isSet("status") ? options.value("status").toLower() : book.shelf;
        if (!library.setReading(book.id, status == "none" ? QString() : status,
                                options.isSet("rating") ? options.value("rating").toInt() : book.rating,
                                options.isSet("finished") ? options.value("finished").simplified() : book.finished, at))
            return std::unexpected(Failure(Failure::Code::StatusUnknown));
    }
    if (options.isSet("paper"))
        library.setPaper(book.id, options.value("paper").toLower() == "yes", at);
    return {};
}

// The time the options stamp the details with (`option`, "details-at"), or the reading, none for now.
[[nodiscard]] std::expected<std::optional<qint64>, Failure> detailsAtOf(const QCommandLineParser &options,
                                                                        const QString &option = QStringLiteral("details-at")) {
    if (!options.isSet(option))
        return std::nullopt;
    bool number = false;
    const qint64 at = options.value(option).toLongLong(&number);
    if (!number || at < 0)
        return std::unexpected(Failure(Failure::Code::DetailsAtNotATime));
    return at;
}

// The numbers the options give, refused before anything is written (#230).
[[nodiscard]] std::expected<void, Failure> numbersOf(const QCommandLineParser &options) {
    const QString index = options.value("series-index").trimmed();
    if (options.isSet("series-index") && !index.isEmpty() && !Numbers::decimal(index))
        return std::unexpected(Failure(Failure::Code::SeriesIndexNotANumber, {options.value("series-index")}));
    if (options.isSet("rating") && !Numbers::whole(options.value("rating"), 0, 5))
        return std::unexpected(Failure(Failure::Code::RatingNotStars, {options.value("rating")}));
    return {};
}

[[nodiscard]] std::expected<QList<QPair<QString, QString>>, Failure> identifiersOf(const QCommandLineParser &options) {
    QList<QPair<QString, QString>> identifiers;
    for (const QString &given : options.values("identifier")) {
        if (!given.contains('='))
            return std::unexpected(Failure(Failure::Code::IdentifierUnformed, {given}));
        identifiers.append({given.section('=', 0, 0), given.section('=', 1)});
    }
    return identifiers;
}

// What --help says of a field's option.
[[nodiscard]] QString described(const QString &field) {
    if (field == "authors")
        return QStringLiteral("Separate several with \" & \".");
    if (field == "tags")
        return QStringLiteral("Separate several with commas.");
    return "Set the " + field + ".";
}

} // namespace

void BookSetting::addOptions(QCommandLineParser &parser) {
    for (const auto &[name, setter] : setters) {
        const QString field = QString::fromLatin1(name);
        parser.addOption({field, described(field), "text"});
    }
    parser.addOption({"cover", "Use this picture as the cover.", "file"});
    parser.addOption({"status",
                      "Where the book stands: to-read, reading, read, or none to go by how far it was "
                      "read here.",
                      "status"});
    parser.addOption({"rating", "Your stars for it, 1 to 5, or 0 for none.", "stars"});
    parser.addOption({"finished", "The day you finished it, as 2026-03-01.", "date"});
    parser.addOption({"paper", "Whether there is a copy on a real shelf: yes or no.", "yes|no"});
    parser.addOption({"identifier",
                      "What the book is called elsewhere, as scheme=value (goodreads=4671); scheme= "
                      "with nothing after it forgets it. Give it once per scheme.",
                      "scheme=value"});
    parser.addOption({"meta",
                      "Take the details this JSON file names (as omabook show --json prints them), leaving "
                      "the rest as they are; the options above still win over it.",
                      "file"});
    parser.addOption({"details-at",
                      "Stamp the details with this time, in seconds since 1970, instead of now: what "
                      "omabook sync gives a book, so the details keep the time they had where they "
                      "came from.",
                      "seconds"});
    parser.addOption({"reading-at",
                      "Stamp a change of the shelf, stars, finished day or paper copy with this time, in "
                      "seconds since 1970, instead of now: what omabook sync gives a book, so its reading "
                      "keeps the time it had where it came from.",
                      "seconds"});
}

std::expected<Book, Failure> BookSetting::apply(Library &library, qint64 id, const QCommandLineParser &options,
                                                const std::function<void(const QString &)> &said) {
    const auto detailsAt = detailsAtOf(options);
    if (!detailsAt)
        return std::unexpected(detailsAt.error());
    const auto readingAt = detailsAtOf(options, QStringLiteral("reading-at"));
    if (!readingAt)
        return std::unexpected(readingAt.error());
    if (const auto numbers = numbersOf(options); !numbers)
        return std::unexpected(numbers.error());
    const auto identifiers = identifiersOf(options);
    if (!identifiers)
        return std::unexpected(identifiers.error());
    const auto book = library.book(id);
    if (!book)
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(id)}));
    const auto meta = metaOf(book->meta, options);
    if (!meta)
        return std::unexpected(meta.error());
    // First, since they are all or nothing: a scheme refused changes nothing.
    if (const auto set = BookOps::setIdentifiers(library, id, *identifiers, *detailsAt); !set)
        return std::unexpected(Failure::said(set.error()));
    // The details kept, and written into its EPUB too where the setting says so (#74).
    QString error;
    if (!library.setMeta(id, *meta, &error, *detailsAt))
        return std::unexpected(Failure::said(error));
    if (const auto written = BookOps::writeDetailsWhenAsked(library, id, Settings().writeDetails()); !written && said)
        said(written.error().text());
    if (const auto read = readingOf(library, *book, options, *readingAt); !read)
        return std::unexpected(read.error());
    // The book's node in the knowledge graph says the same as the library.
    Graph(library).bookNode(id);
    const auto now = library.book(id);
    if (!now)
        return std::unexpected(Failure(Failure::Code::NoSuchBook, {QString::number(id)}));
    return *now;
}
