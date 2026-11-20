// The part of the plugin API table (scriptapi.cpp) that is the knowledge
// graph, `omabook.graph`, what of a plugin's goes into spaces,
// `omabook.sharing` and `omabook.spaces` (#65), and the plugin's own data in
// the change log, which goes between one's computers, `omabook.log` (#76).

#include "scriptapi.h"

namespace ScriptApi {

QList<Entry> graphEntries() {
    return {{"graph.find", "graph.find(words, {kind, scope, everywhere, limit})", "",
             "Searches the knowledge graph: the nodes whose name, other names or summary hold every word, as `[{id, kind, "
             "name, summary, data, scope, book}]`. Kinds are `character`, `place`, `group`, `concept`, `item`, `event`, "
             "`thread`, `theme`, `topic`, `question`, `book`, `author`, `series`, `tag`, or a plugin's own. `scope` is a "
             "graph of its own (`\"world:42\"`); the general graph of what was read when not given."},
            {"graph.node", "graph.node(id)", "", "One node, or `null`."},
            {"graph.around", "graph.around(id, depth)", "",
             "A node and what is within `depth` relations of it (1 when not given): `{nodes, edges: [{id, source, target, "
             "kind, note, learned: {book, section, fraction}}]}`."},
            {"graph.ofBook", "graph.ofBook(id, {section, fraction})", "",
             "What a book's reader has met, as `{nodes, edges}`: never ahead of where the book was read, and with a place, "
             "only what was met up to it."},
            {"graph.beyond", "graph.beyond(id, {section, fraction})", "",
             "What a book connects to elsewhere, as `{nodes, edges}`: things of other books tied to this book's by a "
             "connection across books (an edge whose `across` is true and whose `kind` starts with `across: `, such as "
             "`across: echoes`), and the other books that share a thing with it."},
            {"graph.connect", "graph.connect(id)", "",
             "Has the model compare a book's themes, ideas, threads and main characters with those of the other books "
             "read, and adds what connects them as relations across books; a promise of how many it added. One request, "
             "and none when the same comparison was made before."},
            {"graph.whole", "graph.whole(scope, limit)", "",
             "A whole graph as `{nodes, edges}`, the most connected nodes first: the general one, a scope, or a layer (`graph.layers`) as "
             "far as the reader knows it."},
            {"graph.upsert", "graph.upsert({kind, name, summary, aliases, data, scope, book, section, fraction})", "",
             "Adds a node, or brings the one of that name (or alias) in the same scope and of the same kind up to date; "
             "answers its number. With `book` it is marked as met there, at `section` or where the book was read to."},
            {"graph.link", "graph.link(source, target, relation, {note, book, section, fraction})", "",
             "Relates two nodes with a short verb phrase read from the first to the second (`\"mentor of\"`); answers the "
             "relation's number. Saying the same again only updates the note."},
            {"graph.unlink", "graph.unlink(edge)", "", "Removes one relation, by its number."},
            {"graph.merge", "graph.merge(keep, other)", "", "Folds one node into another: its relations, places and names move over."},
            {"graph.remove", "graph.remove(id)", "", "Removes a node and its relations."},
            {"graph.scopes", "graph.scopes()", "", "The graphs there are: `\"\"` for the general one, every scope, and every layer."},
            {"graph.copyScope", "graph.copyScope(from, to)", "",
             "Copies a whole graph under a new scope name (from `\"\"` for the general one, or from a layer as far as the "
             "reader knows it), so a world of the plugin's own can start from what the books say. False when the name is taken."},
            {"graph.layers", "graph.layers()", "",
             "The worlds plugins keep that were shared with this library, by a collection someone publishes (and it "
             "subscribes to) or a shared space it is in: `[{scope, space, kind, role, owner, plugin, of, book, title}]`. "
             "`scope` is the layer's name here, which `whole`, `copyScope` and `scopes` know and nothing writes to; `of` is "
             "the world's scope as its owner keeps it, `plugin` the plugin that keeps it, `kind` `published` or `shared`, "
             "`role` this library's there (`reader` or `writer`), `owner` whose it is, and `book` the world's book here "
             "with its file (0 without it) and `title` its title. What a layer holds is cut at the reader's place in that "
             "book: nothing of a chapter they haven't read, nothing while they haven't begun it or don't have it."},
            {"graph.removeScope", "graph.removeScope(scope)", "", "Throws a scope away with everything in it; never the general graph."},
            {"graph.exported", "graph.exported(format, {book, upTo, scope})", "",
             "A book's graph or a scope as text other tools read: `json`, `graphml`, `dot` or `cypher`."}};
}

QList<Entry> spaceEntries() {
    return {{"sharing.register", "sharing.register({worlds(context)})", "",
             "Says what of the plugin's goes when a collection is published or shared: `worlds` is given `{space, kind, "
             "books, parts}` (the space's id, `published` or `shared`, the collection's book numbers, its parts such as "
             "`graph`) and answers `[{book, scope, file}]`, at once. Of that only a scope the plugin made (it wrote to it "
             "first) goes, of a book it made (`generated`), with the graph; never a book by a person, never a scope it "
             "didn't name (a plan kept beside a world stays home). Each thing of it goes as met in the book where its "
             "owner has read, so mark what a world holds as met there (`graph.upsert` with `book` and `section`). "
             "`file: true` sends the book's file too, as a space's book files go, also when the space's files switch is "
             "off: a member gets it with the space, at their sync or import."},
            {"spaces.add", "spaces.add(space, id)", "",
             "Puts a book the plugin made into the collection linked to a shared space this library writes, with what "
             "`worlds` answers of it: it goes into the space, signed as the person, at the space's next sync. Throws for a "
             "reader of the space, a space that isn't shared here, or a book the plugin didn't make."},
            {"log.set", "log.set(entity, id, fields)", "",
             "Keeps data of the plugin's own in the change log, which goes to the person's other computers at a sync and is "
             "never a book, a collection or the graph: of the thing `id` (some text) of `entity` (lower-case letters, digits, - "
             "and _, such as `page`), each field of `fields` (`{title: \"Monday\", paper: \"lined\"}`), any JSON value. Of a "
             "field written on two computers the later stays. Only this plugin reads it. A value of more than 64 KiB throws."},
            {"log.add", "log.add(entity, id, set, items, removed)", "",
             "Adds `items` (`{key: value}`) to a set of the thing, such as the strokes of one pen pause, and removes the keys "
             "listed in `removed` in the same write, such as a stroke erased in part: one removed, the pieces left added. What two "
             "computers add to a set is all kept; of one item the latest word holds. At most 1000 items in one write."},
            {"log.remove", "log.remove(entity, id, set, keys)", "",
             "Removes the thing, or with `set` only the items of it named in `keys`. A thing removed comes back when it is "
             "written to later, anywhere."},
            {"log.read", "log.read(entity, id)", "",
             "A thing as the log says it now, `{id, fields, sets: {name: {key: value}}, updated}` (`updated` in seconds), or "
             "`null` when there is none."},
            {"log.list", "log.list(entity)", "", "Every thing of an entity, as `log.read` answers each, ordered by id."}};
}

} // namespace ScriptApi
