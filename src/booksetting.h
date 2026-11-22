#pragma once

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

// The options of `omabook set`, beside the book's number.
void addOptions(QCommandLineParser &parser);

// Sets what `options` say of book `id`: the book as it is now, or why not.
// `said` is told what went wrong that is no failure (its EPUB not written).
[[nodiscard]] std::expected<Book, Failure> apply(Library &library, qint64 id, const QCommandLineParser &options,
                                                 const std::function<void(const QString &)> &said);

} // namespace BookSetting
