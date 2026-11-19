#include "following.h"

#include <QJsonArray>
#include <algorithm>

#include "library.h"
#include "spacelinks.h"
#include "spacereducer.h"
#include "subscribedspace.h"

using namespace Qt::StringLiterals;

namespace {

using Following::Offer;

[[nodiscard]] SpaceTransport *byName(const QList<SpaceTransport *> &transports, const QString &name) {
    const auto found = std::ranges::find_if(transports, [&name](const SpaceTransport *transport) { return transport->name() == name; });
    return found != transports.cend() ? *found : nullptr;
}

// What the link of one subscription holds, and with `take` that taken in.
[[nodiscard]] Offer offerFor(Library &library, const SubscribedSpace::Subscription &space, const SpaceLinks::Link &link,
                             SpaceTransport &transport, bool take) {
    Offer offer{.space = space, .source = link.address, .problem = {}, .operations = 0, .changes = {}, .taken = std::nullopt};
    const auto snapshot = transport.fetch(link.address);
    if (!snapshot) {
        offer.problem = snapshot.error();
        return offer;
    }
    if (snapshot->space != space.space) {
        offer.problem = u"%1 holds another space now (%2), not the one subscribed to; import it to subscribe to that one."_s.arg(
            link.address, snapshot->space);
        return offer;
    }
    if (const auto verified = SubscribedSpace::verify(library, *snapshot); !verified) {
        offer.problem = verified.error();
        return offer;
    }
    const QList<SpaceOperation> lacking = SubscribedSpace::lacking(library, *snapshot);
    offer.operations = lacking.size();
    QList<SpaceOperation> held = SubscribedSpace::operations(library, space.space);
    const ReducedState before = SpaceReducer::reduce(held);
    held += lacking;
    offer.changes = SpaceDiff::between(before, SpaceReducer::reduce(held));
    if (take && !lacking.isEmpty()) {
        auto taken = Subscriptions::subscribe(library, *snapshot, {});
        if (taken)
            offer.taken = *taken;
        else
            offer.problem = taken.error();
    }
    return offer;
}

[[nodiscard]] QString owner(const SubscribedSpace::Subscription &space) {
    return space.ownerName.isEmpty() ? u"its owner"_s : space.ownerName;
}

} // namespace

SpaceTransport *Following::transportFor(const QList<SpaceTransport *> &transports, const QString &address) {
    const auto found =
        std::ranges::find_if(transports, [&address](const SpaceTransport *transport) { return transport->handles(address); });
    return found != transports.cend() ? *found : nullptr;
}

std::expected<Subscriptions::Report, QString> Following::subscribe(Library &library, const QString &address, SpaceTransport &transport,
                                                                   const Subscriptions::Options &options) {
    const auto snapshot = transport.fetch(address);
    if (!snapshot)
        return std::unexpected(snapshot.error());
    auto report = Subscriptions::subscribe(library, *snapshot, options);
    if (report && !options.dryRun)
        SpaceLinks::remember(library, SpaceLinks::Role::Followed,
                             {.space = snapshot->space, .transport = transport.name(), .address = address.trimmed()});
    return report;
}

std::expected<QList<Offer>, QString> Following::check(Library &library, const QString &which, const QList<SpaceTransport *> &transports,
                                                      bool take) {
    QList<SubscribedSpace::Subscription> meant;
    if (which.trimmed().isEmpty()) {
        meant = SubscribedSpace::list(library);
    } else {
        const auto named = Subscriptions::named(library, which);
        if (!named)
            return std::unexpected(named.error());
        meant << *named;
    }
    QList<Offer> offers;
    for (const SubscribedSpace::Subscription &space : std::as_const(meant)) {
        const auto link = SpaceLinks::of(library, space.space, SpaceLinks::Role::Followed);
        if (!link)
            continue;
        SpaceTransport *transport = byName(transports, link->transport);
        if (transport == nullptr)
            offers << Offer{.space = space,
                            .source = link->address,
                            .problem = u"This Omabook can't reach %1 (by %2)."_s.arg(link->address, link->transport),
                            .operations = 0,
                            .changes = {},
                            .taken = std::nullopt};
        else
            offers << offerFor(library, space, *link, *transport, take);
    }
    if (offers.isEmpty())
        return std::unexpected(which.trimmed().isEmpty()
                                   ? u"No shared collection here is followed from a link: import one by its link (a gist or a file "
                                     "of a GitHub repository) to follow it."_s
                                   : u"%1 was imported from a file, not a link, so there is nothing to follow: import it again by its "
                                     "link to follow it."_s.arg(which.trimmed()));
    return offers;
}

QJsonObject Following::toJson(const QList<Offer> &offers) {
    QJsonArray all;
    for (const Offer &offer : offers) {
        QJsonObject one{{"space", offer.space.space}, {"name", offer.space.name}, {"source", offer.source}};
        if (!offer.problem.isEmpty()) {
            one.insert("problem"_L1, offer.problem);
        } else {
            one.insert("new"_L1, offer.operations > 0);
            one.insert("operations"_L1, offer.operations);
            one.insert("changes"_L1, SpaceDiff::toJson(offer.changes));
            one.insert("taken"_L1, offer.taken.has_value());
            if (offer.taken)
                one.insert("import"_L1, Subscriptions::toJson(*offer.taken));
        }
        all << one;
    }
    return {{"followed", all}};
}

QStringList Following::describe(const QList<Offer> &offers) {
    QStringList lines;
    for (const Offer &offer : offers) {
        const QString head = u"%1, by %2, from %3"_s.arg(offer.space.name, owner(offer.space), offer.source);
        if (!offer.problem.isEmpty())
            lines << u"%1: not checked. %2"_s.arg(head, offer.problem);
        else if (offer.operations == 0)
            lines << u"%1: nothing new."_s.arg(head);
        else if (offer.taken)
            lines << u"%1: a newer version, taken in (%2)."_s.arg(head, SpaceDiff::describe(offer.changes))
                  << Subscriptions::describe(*offer.taken);
        else
            lines << u"%1: a newer version, %2 operations new (%3). Nothing was taken in yet: take it with "
                     "omabook collections import --follow \"%4\" --yes"_s.arg(head)
                         .arg(offer.operations)
                         .arg(SpaceDiff::describe(offer.changes), offer.space.name);
    }
    return lines;
}
