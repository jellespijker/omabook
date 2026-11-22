# 15. Timelines branch at an event

- Status: accepted (#171)
- Date: 2026-11-22
- Builds on: #40–#45 (the chronology: times, orders and timelines as graph facts), ADR 0003 (tenants)

## Context

A timeline belongs to a book, a series, a world (by name), real history, or one the reader names. Real
history is one timeline for the whole library, and nothing says that a timeline goes another way than one
it starts from. So a novel in which the war ends otherwise puts its events on real history, among real
events and other books; a world that is ours up to an event and different after it can't be told; and
two versions of one story (a reboot, a Storyteller branch) share one timeline or none.

## Decision

- **A branch is two relations of the graph**, from the branching timeline: `branches from` its parent
  timeline and `splits at` the event where it departs. Both are learned where the split is revealed, like
  every graph fact, so the reader's cut holds: before that place neither is known, and the branch is not
  drawn, proposed or listed. Any timeline can branch (a book's own does, for a story's branch); one made
  only to be a branch is of kind `branch` ("Alternate history from <event>" of real history). A branch can
  branch again; a timeline is never its own ancestor, and a timeline branched anew loses its old split.
- **Shared through the parent, not copied.** The events before the split are not placed on the branch: the
  chronicle gives a branch's lane its parent's events up to and at the time of the split (in the parent's
  order) and marks them shared, then the branch's own. The window draws them once, on the parent's lane;
  the branch's lane begins at the split, beside its parent's, with the split marked and named.
- **The AI only proposes.** When it reads an alternate history or another universe, the answer gives a
  timeline that `branches_from` another `at` an event, and the events that happen otherwise on it by name.
  It is kept as a branch with a fact `branch` = `proposed`, at the split's place. A book that departs from
  real history puts nothing on real history by its date alone; only what the model names as real history
  (the events before the split) is there. A proposed or refused branch is no lane: its events stay in their
  book's own order. The reader's word (`accepted`, `refused`) is another `branch` fact, revealed at the
  split's place with the time it was said; the latest counts. A branch with no word on it (made by hand or
  by a plugin) is simply there, and is never proposed again by a later build.
- **It travels as graph facts do.** Relations and facts go through the change log to one's other computers
  and into published and shared spaces, cut at each reader's place there; tenant membership follows the
  split's book (ADR 0003). A proposal in someone else's layer is not one's own to decide, and is not shown.

## Consequences

- The command line (`omabook chronology timeline branch|accept|refuse`), the MCP tools
  (`chronology_timeline_branch`, `chronology_timeline_decide`) and plugins (`omabook.graph.timeline`,
  `omabook.graph.branch`) write the same relations; the chronicle's JSON has `branch` on a lane, `shared` on
  an item, and `proposals`.
- A branch's lane repeats its parent's early events in the JSON and on the command line, where each lane is
  read alone; the picture does not.
- A reader's word on a branch rides in a fact whose value is a small JSON object, the first graph fact
  that is neither text nor a time.

## Alternatives considered

- **Copy the parent's events onto the branch**: every later event of the parent would have to be copied
  again, and a reader would meet the same event as two.
- **Keep the parent in the branch's data**: data has no moment, so a split could not be cut at the reader's
  place, and would be a spoiler.
- **Let the AI write branches outright**: it would decide for the reader what is history and what is not;
  a wrong guess would take real events off real history.
