#pragma once

#include <QtGlobal>
#include <optional>

class Library;
struct Collection;

// A collection into the change log: its name, parent, rule, shelf and
// reading format, and its books put in or taken out by hand.
namespace ChangeLogCollections {

// Writes what of the collection differs from what the log has of it; with
// `explicitHlc`, at that time (a collection a sync took).
void syncToLog(const Library &library, const Collection &col, bool baseline, std::optional<qint64> explicitHlc = std::nullopt);

// Once, the books put back in a collection by hand before a computer logged
// that (#300): put back after another computer, or a hub (#280), took them
// out, they were never logged, and the next sync took them out again. Not a
// repair of a format of its own (LibraryRepairs): a newer format is refused
// by a hub of the older one, and a point release keeps syncing with it.
void relogPutBack(const Library &library);
// That nothing is left for relogPutBack: a library whose log was just begun.
void markPutBackLogged(const Library &library);

} // namespace ChangeLogCollections
