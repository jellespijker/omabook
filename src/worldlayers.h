#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <optional>

#include "graph.h"

class Library;

// The worlds of plugins someone shared with this library (#65): of each
// space it subscribes to or is a member of, each world the space says it
// holds (SpaceWorlds), but those this library's person put there, as a layer
// beside the library's own graph and never in it.
//
// A layer has a scope of its own here, "<space>/<scope>" (SpaceWorlds::
// layerName): it is among the scopes, read whole and copied into a scope of
// one's own like any, and nothing writes to it. What it holds is cut at the
// reader's own place in the world's book (GraphContext::readerKnows, as the
// layers of a book's graph are): nothing of a chapter they haven't read, and
// nothing at all while they haven't begun it, or don't have it.
namespace WorldLayers {

struct Layer {
    QString scope;   // its name here
    QString space;   // the space it is from
    QString kind;    // "published" or "shared"
    QString role;    // this library's in the space: "reader" or "writer"
    QString owner;   // whose it is, as they go by in the space
    QString plugin;  // the plugin that keeps it
    QString of;      // its scope as its owner has it ("world:42")
    qint64 book = 0; // the world's book here, with its file; 0 when this library hasn't that
    QString title;   // the book's, as the space has it
    GraphView view;  // what the reader knows of it

    // {scope, space, kind, role, owner, plugin, of, book, title}, as
    // omabook.graph.layers() answers.
    [[nodiscard]] QJsonObject toJson() const;
};

// Every world shared with this library, each with what is known of it when
// `known` is true (reading it costs more than naming it).
[[nodiscard]] QList<Layer> list(const Library &library, bool known = true);
// The layer named `scope`, or none.
[[nodiscard]] std::optional<Layer> named(const Library &library, const QString &scope);

// The scopes there are in the graph's tenant (Graph::scopes) and the layers'.
[[nodiscard]] QStringList scopes(const Graph &graph);
// The scopes a reader that names no tenant reads, each in the tenant it is
// read in (Graph::ofScope): a world of one tenant's books is there from any
// other (#112). Those every tenant has, in their order, and the layers'.
[[nodiscard]] QStringList scopes(const Library &library);
// The graph a plugin writes a thing ({scope, book, ...}) in: a world's whole
// (Graph::ofScope), or for the general graph that of the book it was met in.
[[nodiscard]] Graph writing(const Library &library, const QJsonObject &thing);
// The graph a plugin relates or merges two things in: the world (a scope of
// its own) both are of, whole, whatever tenants the books they were met in
// are in (#112); else that of the book given, or of the first thing.
[[nodiscard]] Graph linking(const Library &library, qint64 one, qint64 other, qint64 bookId);
// Two things merged as a plugin merges them, in the graph linking() gives.
bool merge(const Library &library, qint64 keep, qint64 other);
// The things a plugin finds (GraphOps::find): in a world whole, or in every
// scope (`everywhere`) and the general graph in Tenancy::preferred's tenant.
[[nodiscard]] QJsonArray find(const Library &library, const QString &words, const QString &kind, const QString &scope, bool everywhere,
                              int limit);
// A scope whole as the graph's tenant sees it (Graph::whole), a layer's as known.
[[nodiscard]] GraphView whole(const Graph &graph, const QString &scope, int limit);
// Copies a scope whole under a new name (Graph::copyScope), a layer's as
// known, its things and how they relate. False when `to` is taken, or is a
// layer's name.
[[nodiscard]] bool copy(const Library &library, const QString &from, const QString &to);

} // namespace WorldLayers
