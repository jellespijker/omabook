#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>

#include "library.h"
#include "publishedspace.h"
#include "spacetransport.h"

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

// Sends the snapshot of `space`, published here, by `transport` (#7): its log
// brought in step first, to where that transport sent it before (SpaceLinks),
// brought up to date there, else somewhere new, remembered for the next time;
// `open` asks for a place anyone may find. {"address", "transport",
// "created", "public", "note", "operations"}.
[[nodiscard]] std::expected<QJsonObject, QString> send(const Library &library, const QString &space, SpaceTransport &transport, bool open);

// The options of `omabook collections share` that are its own, on the parser of
// `omabook collections`, which has --as and --dry-run for import and share both
// (CollectionImport::addOptions).
void addOptions(QCommandLineParser &parser);

// `omabook collections share <path> [--with-graph] [--with questions,ratings,progress] [--as <name>]
// [--output <file>] [--gist [--public]] [--dry-run]`, with the options Collections::run read.
int run(const Library &library, const QCommandLineParser &parser, const QString &path, bool json);

} // namespace CollectionShare
