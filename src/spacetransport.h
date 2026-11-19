#pragma once

#include <QString>
#include <expected>

#include "spacesnapshot.h"

// A transport is how a space's operations get from one place to another
// (ADR 0001, *Transports are adapters*; #7): a gist or a repository
// (GitHubLink), a folder something else syncs (#38), and later others. What
// it carries is a space's snapshot, the space compacted with each operation's
// number kept, so whoever already holds an older one needs only what came
// after; which operations those are is decided by whoever takes it in
// (SubscribedSpace::lacking), never by the transport. A transport is trusted
// with nothing: what it brings is verified before it is kept
// (SubscribedSpace::verify), and it never reads or writes the library.
//
// Its calls return when they are done; the window runs them off its thread.
class SpaceTransport {
public:
    // How a space is sent.
    struct Sending {
        // Where this transport sent the space before (Sent::address), to be
        // brought up to date in place; empty to send it somewhere new.
        QString previous;
        // Whether it may be found by anyone (a public gist), or only by who
        // is given the address (a secret one): the latter unless asked.
        bool open = false;
    };
    // Where a space was sent.
    struct Sent {
        QString address;      // what a follower fetches from, and `previous` the next time
        bool created = false; // a new place, not the previous one brought up to date
        bool open = false;
        QString note; // what the person should know of it, if anything
    };

    virtual ~SpaceTransport() = default;
    SpaceTransport(const SpaceTransport &) = delete;
    SpaceTransport &operator=(const SpaceTransport &) = delete;
    SpaceTransport(SpaceTransport &&) = delete;
    SpaceTransport &operator=(SpaceTransport &&) = delete;

    // The word it is remembered by (SpaceLinks): "github", "folder".
    [[nodiscard]] virtual QString name() const = 0;
    // Whether `address` is one of its own.
    [[nodiscard]] virtual bool handles(const QString &address) const = 0;
    // The snapshot kept at `address`, as it is there now.
    [[nodiscard]] virtual std::expected<SpaceSnapshot, QString> fetch(const QString &address) = 0;
    // Sends `snapshot`. The caller brings the space's log in step first
    // (PublishedSpace::snapshotOf does), so what goes is the space as it is now.
    [[nodiscard]] virtual std::expected<Sent, QString> publish(const SpaceSnapshot &snapshot, const Sending &sending) = 0;

protected:
    SpaceTransport() = default;
};
