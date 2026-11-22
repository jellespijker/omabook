#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>
#include <optional>

#include "failure.h"
#include "graph.h"

// Timelines that branch (#171, ADR 0015): a timeline that is the same as
// another up to an event and goes its own way after it. Alternate history
// is a branch of real history; an alternative universe, a reboot or a
// story's branch is a branch of a world's, a series' or a book's timeline.
// A branch can branch again.
//
// It is kept as the rest of the chronology is (chronology.h), as relations
// and a fact of the graph, each with the moment it was revealed, so a
// reader is never shown a split before they read it, and it travels with
// the graph through the log and into spaces:
//
// - the branch is "branches from" its parent timeline and "splits at" the
//   event it departs at, both learned where the split is revealed;
// - the events before the split are shared, through the parent: they are
//   on the branch too, without being placed on it;
// - what the AI recognises is only proposed: a fact "branch" of the
//   timeline says so, and the reader's word on it (accepted or refused) is
//   another, the latest of which counts. A proposed or refused branch is
//   not drawn, and its events stay in their book's own order, never on its
//   parent. A branch made by hand, or by a plugin, has no word on it and is
//   simply there.
namespace Chronology {

[[nodiscard]] inline QString branchesFrom() { return QStringLiteral("branches from"); }
[[nodiscard]] inline QString splitsAt() { return QStringLiteral("splits at"); }
[[nodiscard]] inline QString standingFact() { return QStringLiteral("branch"); }

// Whether a relation is one of a branch's.
[[nodiscard]] bool ofBranch(const QString &relation);

// The reader's word on a branch.
enum class Standing { Proposed, Accepted, Refused };
[[nodiscard]] QString name(Standing standing);

// The latest word on a branch, of its facts revealed at a known place;
// accepted when there is none (it was not proposed).
[[nodiscard]] Standing standingOf(const QList<GraphFact> &facts, const GraphKnown &known);
// Whether any fact is a word on a branch.
[[nodiscard]] bool hasStanding(const QList<GraphFact> &facts);

// Where a timeline departs from its parent, as the reader knows it.
struct Split {
    GraphNode parent; // the timeline it branches off
    GraphNode at;     // the event it departs at
    GraphMoment revealed;
    Standing standing = Standing::Accepted;

    [[nodiscard]] QJsonObject toJson() const;
};

// A branch the AI proposed that the reader has not said yes or no to.
struct Proposal {
    GraphNode timeline;
    Split split;

    [[nodiscard]] QJsonObject toJson() const;
};

// The split of each timeline that branches, of these relations (cut where
// the reader is already) between the things known, with each one's word.
// Only a timeline with both its relations, to a timeline and to an event,
// branches.
[[nodiscard]] QHash<qint64, Split> splitsOf(const QList<GraphEdge> &edges, const QHash<qint64, GraphNode> &things,
                                            const QHash<qint64, Standing> &standing);

// The timeline a timeline branches off, as the graph has it now; 0 for none.
[[nodiscard]] qint64 parentOf(const Graph &graph, qint64 timeline);

// The name of a branch none was given: "Alternate history from <event>" of
// real history, "<parent> from <event>" of another.
[[nodiscard]] QString branchName(const GraphNode &parent, const GraphNode &at);

// A timeline that branches off another at an event.
struct Branching {
    qint64 timeline = 0; // an existing timeline, such as a book's own; 0 to make one
    QString name;        // of the one to make, or found by; branchName() when empty
    qint64 from = 0;     // the timeline it branches off
    qint64 at = 0;       // the event it splits at
    // Where the split is revealed; nowhere, for one put in by hand.
    std::optional<GraphMoment> revealed;
    bool proposed = false; // the AI's, for the reader to accept or refuse
};

// Writes it, checked: `from` a timeline, `at` an event, and no timeline its
// own ancestor. A timeline that branched before branches anew: its old
// split is taken away. One that is there without a word on it (made by
// hand) is not proposed again. Answers the branch's number.
[[nodiscard]] std::expected<qint64, Failure> branch(Graph &graph, const Branching &branching);
// The reader's word on a proposed branch, revealed where the split is, so
// that it travels and is cut as the split is.
[[nodiscard]] std::expected<qint64, Failure> decide(Graph &graph, qint64 branch, bool accept);

} // namespace Chronology
