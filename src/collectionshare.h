#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>

#include "library.h"
#include "publishedspace.h"

class QCommandLineParser;

// Sharing a collection (#3): publishing it as a space (PublishedSpace) and
// writing its snapshot for others, as `omabook collections share` and the
// MCP tool collection_share do it. Before anything is written it says what
// goes in and what is left out.
namespace CollectionShare {

// What publishing `collection` with `parts` puts in the space, counted by
// kind, and what it leaves out: {"collection", "parts", "in": {"collections",
// "books", "things", "facts", "relations", "questions", "ratings",
// "reading_places", "reading_days"}, "left_out": [{"part", counts…,
// "switch"}]}. The books' files are always left out; each part not switched
// on says what it would add. Writes nothing.
[[nodiscard]] std::expected<QJsonObject, QString> preview(const Library &library, const Collection &collection,
                                                          const PublishedSpace::Parts &parts);

// The options of `omabook collections share` that are its own, on the parser of
// `omabook collections`, which has --as and --dry-run for import and share both.
void addOptions(QCommandLineParser &parser);

// `omabook collections share <path> [--with-graph] [--with questions,ratings,progress] [--as <name>]
// [--output <file>] [--dry-run]`, with the options Collections::run read.
int run(const Library &library, const QCommandLineParser &parser, const QString &path, bool json);

} // namespace CollectionShare
