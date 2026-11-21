#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <expected>

#include "library.h"
#include "publishedspace.h"
#include "spacetransport.h"
#include "spaceworlds.h"

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
// "switch"}]}, "files" (how many the books have), "kept_private" (the books
// a plugin keeps private, PrivateBooks::toJson). Each part not switched on
// says what it would add. Writes nothing.
[[nodiscard]] std::expected<QJsonObject, QString> preview(const Library &library, const Collection &collection,
                                                          const PublishedSpace::Parts &parts);

// Sends the snapshot of `space`, published here, by `transport` (#7): its log
// brought in step first, to where that transport sent it before (SpaceLinks),
// brought up to date there, else somewhere new, remembered for the next time;
// `open` asks for a place anyone may find. {"address", "transport",
// "created", "public", "note", "operations"}.
[[nodiscard]] std::expected<QJsonObject, QString> send(const Library &library, const QString &space, SpaceTransport &transport, bool open);

// What publishing or sharing says of the books a plugin keeps private
// (PrivateBooks::toJson), a line each, in English: why each stays out (the
// collection's rule takes it in, or it was put in by hand), and how to let it
// go; or that it goes as the user chose, and how to take that back (#127).
[[nodiscard]] QStringList keptPrivate(const QJsonArray &kept);

// The name the user went by when they published the collection before;
// empty the first time.
[[nodiscard]] QString nameGoneBy(const Library &library, const Collection &collection);

struct Publishing {
    QString as;     // the name the owner goes by in the space; the one gone by before when empty
    QString output; // where to write the space's snapshot; nothing is written when empty
    // A transport to send it by as well (send), and whether to a place anyone may find.
    SpaceTransport *link = nullptr;
    bool open = false;
    // The plugins to ask what of theirs goes with it (SpaceWorlds, #65); what
    // was given before stays when there are none to ask.
    SpaceWorlds::Ask worlds;
};

// What the plugins are asked of `collection` when `person` publishes it with `parts`.
[[nodiscard]] SpaceWorlds::Asked asked(const Library &library, const Collection &collection, const PublishedSpace::Parts &parts,
                                       const QString &person);

// Publishes the collection as the user's space, with `parts`, and writes its
// snapshot where `how` says: {"space", "owner", "person", "operations",
// "new", with a file "output" and "snapshot_operations", and with a link
// "sent", as send says it}. What the command line and the window both do
// once the summary has been shown. With plugins to ask, also {"worlds": what
// they were given and what was left out (SpaceWorlds::Granted)}.
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
