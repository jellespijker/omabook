#pragma once

#include <QList>
#include <QString>
#include <QtTypes>
#include <expected>

#include "spacereplay.h"
#include "subscriptions.h"

class Library;

// What a shared space holds (#38): a collection, as a published space does
// (PublishedSpace: its tree, its books' identities, and the parts it was
// founded with), but written by each of its writers, and a member's own
// collection linked to it.
//
// Its operations never change a member's own rows. What comes of them in
// the member's library is only what subscribing makes (Subscriptions::file):
// the books of the space put in the linked collection, matched to the
// member's own or as wishlist entries, the rest of their library as it was.
//
// A writer's own edits go the other way (contribute): what their linked
// collection says now, read as publishing reads it, compared with what the
// space says, and of each field what they changed later than the space last
// did, written into the space as theirs. Their books go by the space's ids
// where the space knows them (matched by Subscriptions::file), and the
// space's collections by the paths they have below its top one.
namespace SharedCollection {

// Files the space's books into the collection linked to it, or into one
// named `as` (made here when it isn't, and linked); `before` is what was
// accepted of the space before what was taken now.
[[nodiscard]] std::expected<Subscriptions::Report, QString> file(Library &library, const QString &space,
                                                                 const QList<SpaceOperation> &before, const QString &as = {});

// Writes into the space what this writer's linked collection says that the
// space doesn't, later than it; `filed` is the last filing, which names the
// space's books here. How many operations it wrote.
[[nodiscard]] std::expected<qsizetype, QString> contribute(const Library &library, const QString &space,
                                                           const Subscriptions::Report &filed);

} // namespace SharedCollection
