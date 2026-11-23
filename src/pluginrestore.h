#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>
#include <expected>

#include "failure.h"
#include "plugindata.h"

class Library;

// A plugin's thing put back as an earlier copy of its library had it (#209):
// a notebook of the notebooks plugin with its pages and what is on them, or
// one page with what is on it, from a snapshot's database into the live
// library, as the plugin keeps them in the change log (ADR 0002). What writes
// it is the library's one writer, after a moment snapshot that undoes it
// (SnapshotRestore).
//
// A thing comes back with every thing that names it there: by its id as the
// value of a field (a page's notebook, a text block's page), or as its own id
// (a page's recognised text, a picture's bytes), and so on down. Nothing of
// the live library is lost on the way: of each, the fields that differ are
// set to the earlier ones (and stamped now, so a sync carries them on), and of
// its sets the items it lacks or has otherwise come back; a thing removed since
// comes back, and one made since, an item added since, stay.
namespace PluginRestore {

// What of one thing goes back: the fields it has otherwise now (all of them
// when it is gone), and of each set the items it lacks or has otherwise.
struct Put {
    PluginData::Thing thing;
    QJsonObject fields;
    QMap<QString, QJsonObject> items;
    bool back = false; // gone now, and comes back
};

struct Plan {
    QList<Put> puts;
    QMap<QString, int> comingBack;   // of each entity, the things gone now that come back
    QMap<QString, int> changingBack; // of each entity, the things there that change back
    int items = 0;                   // the items of sets that come back
    [[nodiscard]] bool empty() const noexcept { return puts.isEmpty(); }
};

// What putting `root` back in `live` as `from` has it does. Refused when
// `from` holds no such thing (RestoreThingNotFound).
[[nodiscard]] std::expected<Plan, Failure> plan(const Library &live, const Library &from, const PluginData::Thing &root);

struct Restored {
    int things = 0; // things put back: come back, or changed back
    int items = 0;  // items of their sets put back
};

// The plan written in `live`.
[[nodiscard]] std::expected<Restored, Failure> apply(const Library &live, const Plan &plan);

} // namespace PluginRestore
