#pragma once

#include <QString>

class Library;

// What the change log is told of books that are gone (ADR 0001): the part of
// ChangeLog that writes their tombstones.
namespace ChangeLogRemovals {

// A book that is gone: its tombstone, unless it already has one.
void gone(const Library &library, const QString &gid);

// On opening: every removal the library has on record, and of every book this
// computer had rows of and has no more, its tombstone.
void detect(const Library &library);

} // namespace ChangeLogRemovals
