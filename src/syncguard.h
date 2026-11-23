#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "library.h"
#include "sync.h"
#include "syncplan.h"

// The sync guard (#187, ADR 0011): a sync never loses what either side had.
//
// Before either side writes, the sync is planned (a dry run on both sides,
// each recording its SyncPlan) and the plans are checked against the rules
// below. A plan that breaks one is refused before anything is written, here
// or there, with the books and the rule named; `--accept <title>` lets one
// book's through, and a dry run shows the same checks. After the write, each
// side's books are compared with what was announced of them, and whatever
// differs is said.
//
// The rules (ADR 0009 says why the pairing ones):
// - No book is lost: a book is removed only when the other side holds it no
//   more, by its id nor by what names it.
// - No identifier is cleared but the ones the dry run announces as another
//   book's (StrayIdentifiers), and never a book's own: its ISBN, or what its id is made of.
// - No field that had a value is emptied: not a title, an author, an ISBN, a
//   description, a tag, a shelf, stars, a finished day, a paper copy or a cover.
// - No two books become one without the same global id: a book paired on
//   less than it takes no title, authors or ISBN of the other without a rename
//   agreed to (`--accept-renames`, `--details`), and no identifier of it.
// - Reading places and bookmarks only move forward: a place taken is never
//   older than the one here, and no bookmark or highlight goes to another book.
// - The counts are what the plan announced: no more books arrive and no more
//   go than it said (checked after the write).
namespace SyncGuard {

enum class Rule { BookLost, IdentifierCleared, FieldEmptied, BooksMerged, PlaceBack, AnnotationMoved };

struct Breach {
    QString title; // the book's, on the side it would happen
    Rule rule = Rule::FieldEmptied;
    QString detail; // which field or identifier, and what it had
};

// What a plan is checked against: the books of the side it is of, and of the
// other side; whether a rename was agreed to; and the identifiers its dry run
// announced as another book's ("<title>: <scheme> <value>").
struct Given {
    SyncPlan::States mine;
    SyncPlan::States other;
    bool consent = false;
    QStringList identifiersRemoved;
};

// The rules a plan breaks, each of a book.
[[nodiscard]] QList<Breach> check(const SyncPlan &plan, const Given &given);

// What one side's books, `before` and `after` a sync, show that its `plan`
// didn't announce: a book gone, a field changed, more books than `received`.
// Identifiers named in `identifiersRemoved` may go.
struct Announced {
    SyncPlan plan;
    int received = 0;
    QStringList identifiersRemoved;
};
[[nodiscard]] QStringList differences(const SyncPlan::States &before, const SyncPlan::States &after, const Announced &announced);

// A breach as a person reads it, of the side `where` ("here", "on laptop").
[[nodiscard]] QString worded(const Breach &breach, const QString &where);

// One sync's two sides, for the guard to plan, check and run it: `hub`, whether
// the other side is a hub (HubRemote); `run` syncs as the options say (a dry
// run or not), and `theirStates` asks the other side for its books (none from
// an Omabook that can't say them).
struct Sides {
    QString host;
    bool hub = false;
    std::function<SyncSummary(const SyncOptions &)> run;
    std::function<std::optional<SyncPlan::States>()> theirStates;
};

// Plans the sync of `library` with `sides` as `options` say, checks the plan, and, unless it
// breaks a rule of a book not in `options.accept` or is a dry run, runs it and
// compares both sides with what was announced: the summary says the breaches
// (`breaches`, and `error` when it was refused) and what differs (`unannounced`).
// An Omabook there from before the guard makes no plan: its side goes unchecked,
// and the summary says so (`uncheckedThere`); a hub that makes none is refused,
// saying to update it (#211).
[[nodiscard]] SyncSummary guarded(const Library &library, const Sides &sides, const SyncOptions &options);

} // namespace SyncGuard
