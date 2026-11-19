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

// The name the user went by when they published the collection before;
// empty the first time.
[[nodiscard]] QString nameGoneBy(const Library &library, const Collection &collection);

struct Publishing {
    QString as;     // the name the owner goes by in the space; the one gone by before when empty
    QString output; // where to write the space's snapshot; nothing is written when empty
    // A transport to send it by as well (send), and whether to a place anyone may find.
    SpaceTransport *link = nullptr;
    bool open = false;
};

// Publishes the collection as the user's space, with `parts`, and writes its
// snapshot where `how` says: {"space", "owner", "person", "operations",
// "new", with a file "output" and "snapshot_operations", and with a link
// "sent", as send says it}. What the command line and the window both do
// once the summary has been shown.
[[nodiscard]] std::expected<QJsonObject, QString> publish(const Library &library, const Collection &collection,
                                                          const PublishedSpace::Parts &parts, const Publishing &how);

// The options of `omabook collections share` that are its own, on the parser of
// `omabook collections`, which has --as and --dry-run for import and share both
// (CollectionImport::addOptions).
void addOptions(QCommandLineParser &parser);

// `omabook collections share <path> [--with-graph] [--with questions,ratings,progress] [--as <name>]
// [--output <file>] [--gist [--public]] [--dry-run]`, with the options Collections::run read.
int run(const Library &library, const QCommandLineParser &parser, const QString &path, bool json);

} // namespace CollectionShare
