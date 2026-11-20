#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <optional>

class Library;

// What the database keeps beside the knowledge graph so that it is searched
// and followed without being read whole (#102, format 31), kept by triggers
// as the rows are written, by whoever writes them:
//
// - graph_words, a full-text index (SQLite FTS5) of each thing's name, what
//   is said of it, its data (its other names) and every summary it was given
//   as a fact. Its words are folded as the tokenizer unicode61 folds them,
//   case and accents aside ("mara" finds "Mára"), in any script that writes
//   words apart. It holds only the words: the rows stay where they are.
// - graph_touched, the things whose part of the graph changed (the thing, a
//   place it was met, a fact, a relation, which tenants see it), each with the
//   graph counter (Library::Changes::graph) then, so that what was read of
//   the graph at a count is brought up to date by reading those things again.
//
// Which tenants see a thing is not here: what reads the index keeps to a
// tenant as every reader of the graph does (Graph::within).
namespace GraphIndex {

// The format step's statements: the tables, filled from the graph there is, and the triggers.
[[nodiscard]] const QStringList &step();

// A word of a search as the index is asked for it (FTS5's query syntax): a
// word that starts one of the thing's words; a word that is several to the
// index ("o'brien") those in a row, the last begun. Nothing for a word with no
// letter or digit in it, which the index has no word for.
[[nodiscard]] std::optional<QString> phrase(const QString &word);

// Whether each of the words starts a word of `text`, folded as the index
// folds them: what the index answers, said again of a thing as a tenant was
// told it (Graph::told), which the index doesn't hold apart.
[[nodiscard]] bool holds(const QString &text, const QStringList &words);

// The things touched since the graph counter was at `since` (or at it).
[[nodiscard]] QSet<qint64> touchedSince(const Library &library, qint64 since);

// How many times Tenancy filled the tenants' tables whole, which says nothing
// of the things it touched: what was read before then is read again whole.
[[nodiscard]] qint64 tenancyRefills(const Library &library);

} // namespace GraphIndex
