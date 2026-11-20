#pragma once

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

// The scopes there are (Graph::scopes) and the layers'.
[[nodiscard]] QStringList scopes(const Library &library);
// A scope whole (Graph::whole), a layer's as known.
[[nodiscard]] GraphView whole(const Library &library, const QString &scope, int limit);
// Copies a scope whole under a new name (Graph::copyScope), a layer's as
// known, its things and how they relate. False when `to` is taken, or is a
// layer's name.
[[nodiscard]] bool copy(const Library &library, const QString &from, const QString &to);

} // namespace WorldLayers
