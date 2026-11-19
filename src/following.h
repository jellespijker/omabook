#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <expected>
#include <optional>

#include "spacediff.h"
#include "spacetransport.h"
#include "subscriptions.h"

class Library;

// Sharing by link (#7): a shared collection sent somewhere a transport
// reaches (a gist, a repository's file; SpaceTransport), imported from
// there, and followed for its newer versions, the same for the command line
// and the MCP server.
//
// Importing from a link subscribes as importing a file does (Subscriptions)
// and remembers the link as where the subscription is followed from
// (SpaceLinks). Following it fetches what is there now and offers what is
// new: never taken in by itself, only when asked. What is new is what the
// subscriber lacks (SubscribedSpace::lacking): a newer snapshot keeps the
// numbers its operations had, so of one taken after the subscriber's only
// the operations after it are new, and only those are taken in.
namespace Following {

// The transport of those given that goes by `address`, or none.
[[nodiscard]] SpaceTransport *transportFor(const QList<SpaceTransport *> &transports, const QString &address);

// Subscribes to the space kept at `address` and, unless it is a dry run,
// remembers it as where the subscription is followed from.
[[nodiscard]] std::expected<Subscriptions::Report, QString> subscribe(Library &library, const QString &address, SpaceTransport &transport,
                                                                      const Subscriptions::Options &options);

// What a subscription's link holds that it doesn't yet.
struct Offer {
    SubscribedSpace::Subscription space;
    QString source;           // the link it follows
    QString problem;          // why it couldn't be checked; nothing below is set then
    qsizetype operations = 0; // new: what taking it in appends
    SpaceDiff::Changes changes;
    // Taken in, when asked and there was something new.
    std::optional<Subscriptions::Report> taken;
};

// Checks the subscription `which` names (Subscriptions::named), or every one
// followed from a link when it is empty, each by the transport of its link,
// and with `take` takes in what is new. Fails only when there is nothing to
// check; a link that can't be checked is said in its offer.
[[nodiscard]] std::expected<QList<Offer>, QString> check(Library &library, const QString &which, const QList<SpaceTransport *> &transports,
                                                         bool take);

[[nodiscard]] QJsonObject toJson(const QList<Offer> &offers);
[[nodiscard]] QStringList describe(const QList<Offer> &offers);

} // namespace Following
