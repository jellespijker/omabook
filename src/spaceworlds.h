#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <QtTypes>
#include <functional>
#include <span>

#include "spacereplay.h"

class Library;

// A plugin's worlds in the spaces a collection is published or shared as
// (#65). A plugin that makes books (a story) keeps what it makes up in a
// scope of the graph of its own, a world, which the general graph a space
// takes never holds. When a collection goes into a space its plugins are
// asked what of theirs goes with it (Ask), and of what they answer only
// this is given to the space:
//
//   - a scope the plugin made here (madeBy), and no other: never one a
//     person or another plugin made, never one it didn't name (a plan kept
//     beside a world stays home unless it is named itself);
//   - with a book the plugin made (`generated`), which the collection
//     holds: never a person's book, nor anything of one;
//   - with the graph, only when the space takes the graph.
//
// What was given is kept here by space (granted), checked again each time
// the space is written, and said in the space as a world (entity "world")
// beside its things, which go as the general graph's do: only what the owner
// has met in the book, nothing past where they are in it. Asking for the
// book's file is kept with it (`file`); it travels as the space's book files
// do (#63).
namespace SpaceWorlds {

// Tells that `plugin` writes to `scope`: the plugin made it when it isn't
// there yet. The general graph, and a layer's name, are no plugin's.
void madeBy(const Library &library, const QString &scope, const QString &plugin);
// The plugin that made a scope here, or empty.
[[nodiscard]] QString maker(const Library &library, const QString &scope);

// What a plugin is asked when a collection goes into a space.
struct Asked {
    QString space;
    QString kind; // "published" or "shared"
    QList<qint64> books;
    QStringList parts; // PublishedSpace::Parts::names
    // {space, kind, books, parts}, as the plugin is given it.
    [[nodiscard]] QJsonObject toJson() const;
};

// What a plugin offers of one of the books.
struct Offer {
    QString plugin;
    qint64 book = 0;
    QString scope; // empty when only the book's file is asked for
    bool file = false;
};
using Ask = std::function<QList<Offer>(const Asked &)>;

struct Left {
    Offer offer;
    QString why;
};
struct Granted {
    QList<Offer> worlds;
    QList<Left> left;
    // {"worlds": [{plugin, book, scope, file}], "left": [{plugin, book, scope, why}]}.
    [[nodiscard]] QJsonObject toJson() const;
};

// Gives the space what the rules above allow of `offered`, in place of what
// it was given before of the books asked about.
Granted grant(const Library &library, const Asked &asked, std::span<const Offer> offered);
// Asks `ask` (when there is one) and grants what it answers.
Granted ask(const Library &library, const Asked &asked, const Ask &ask);

// A world given to a space.
struct World {
    QString book; // its global id
    QString scope;
    QString plugin;
    bool file = false;
    qint64 hlc = 0; // when it was given
};
// What `space` was given that holds still: its book is still the plugin's
// own, and its scope too. Without the graph, only the files asked for.
[[nodiscard]] QList<World> granted(const Library &library, const QString &space, bool graph = true);
// The scopes of `worlds`, each the log its world is kept in (Spaces::spaceForScope).
[[nodiscard]] QStringList scopesOf(std::span<const World> worlds);
// The operations that say so in the space, as `device` writes them: of each,
// entity "world", fields "scope", "plugin", "book" and "file".
[[nodiscard]] QList<SpaceOperation> said(std::span<const World> worlds, const QString &space, const QString &device);

// A world as a space says it, to whoever receives the space.
struct Declared {
    QString id;
    QString scope; // as its owner calls it
    QString plugin;
    QString book;   // the book's id in the space
    QString author; // who said it: the person who signed it, else its author
    bool file = false;
};
// The worlds `ops` say and haven't taken back.
[[nodiscard]] QList<Declared> declared(std::span<const SpaceOperation> ops);

// The name a layer of a world goes by here, "<space>/<scope>", and whether a
// scope is one: no plugin writes to it.
[[nodiscard]] QString layerName(const QString &space, const QString &scope);
[[nodiscard]] bool isLayerName(QStringView scope);

} // namespace SpaceWorlds
