#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QtTypes>
#include <optional>
#include <span>

#include "spacereplay.h"

class Library;

// The log of a published space (#3; ADR 0001, *Spaces*): what it has said,
// numbered once and for good, so that one who holds its operations up to a
// number needs only those after it. Kept in `published_ops`, apart from
// `ops`, since all of `ops` is read to make this library's own rows and a
// published space speaks of the same books.
//
// It follows the collection by being brought in step with what the space
// says now (PublishedSpace::operations): what differs is appended, after
// what is there. Nothing in it is ever changed or taken out.
namespace PublishedLog {

// Which collection a space publishes (its global id at home) and with which
// parts (PublishedSpace::Parts::names).
struct Record {
    QString collection;
    QStringList parts;
};

void remember(const Library &library, const QString &space, const Record &record);
[[nodiscard]] std::optional<Record> recorded(const Library &library, const QString &space);

// What this computer writes in the space as: a name of its own there, the
// same each time, which says nothing of the computer.
[[nodiscard]] QString deviceIn(const Library &library, const QString &space);

// Every operation of the space, by device and sequence.
[[nodiscard]] QList<SpaceOperation> operations(const Library &library, const QString &space);

// Appends what `now` says that the log doesn't yet, each written by
// `author`: a field's new value (with its own stamp, or a new one where the
// log has a later one), what only grows and is new, a thing back that was
// gone, and of a thing that is no longer in the space its removal, of a
// field no longer said its clearing. One transaction. How many it appended.
qint64 follow(const Library &library, const QString &space, std::span<const SpaceOperation> now, const QString &author);

} // namespace PublishedLog
