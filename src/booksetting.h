#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>
#include <functional>

#include "failure.h"
#include "library.h"

class QCommandLineParser;

// What `omabook set <id>` changes of a book, written once for the command
// line and for a hub, which takes it from a computer's sync (#188): each
// option it was given, and nothing else.
namespace BookSetting {

// The details a JSON file names, as `omabook show --json` prints them;
// refused when it can't be read, holds no object, or its number in the series
// is none.
[[nodiscard]] std::expected<QJsonObject, Failure> detailsIn(const QString &file);
// What --meta and --cover say of a book being added, over `meta`: the file's
// details in its place, and the picture as its cover. Refused as `set` refuses
// them (#286).
[[nodiscard]] std::expected<BookMeta, Failure> ofNewBook(BookMeta meta, const QCommandLineParser &options);

// The options of `omabook set`, beside the book's number.
void addOptions(QCommandLineParser &parser);

// Sets what `options` say of book `id`: the book as it is now, or why not.
// `said` is told what went wrong that is no failure (its EPUB not written).
[[nodiscard]] std::expected<Book, Failure> apply(Library &library, qint64 id, const QCommandLineParser &options,
                                                 const std::function<void(const QString &)> &said);

} // namespace BookSetting
