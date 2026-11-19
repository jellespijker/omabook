#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QtTypes>

class Library;

// What one's own computers carry to each other of the spaces one is in, as
// the ssh sync replicates their logs (#51; ADR 0001, *People and keys*): the
// shared spaces (#38), each with its key and the operations the other lacks,
// and the subscriptions (#4), each with its operations, the collection it is
// linked to and the place it is followed from. Joining a space, or
// subscribing, on one of them is then joining, or subscribing, on all.
//
// Only between computers of one person: what is carried goes only to a side
// that says it is this library's person, sealed to that person (a sealed
// box, Person::sealFor) and signed by them, so that it opens only where the
// person's key is, and is taken only from where it is (what pairing hands
// over, over the same ssh). Nothing of it goes to anyone else, nor to an
// Omabook from before, which doesn't say it carries.
//
// What arrives is checked as if it came from anywhere: a shared space's
// operations by its role rules and signatures (SharedSpace::take), a
// subscription's as a snapshot of the space is (SubscribedSpace::verify).
// A key arrives for its owner alone and whole (SecretFile), and never takes
// the place of one there.
namespace SpaceCarry {

// What a side says of itself in its manifest and in a replication request
// and its answer: that it carries ("carries": 1), the person it is now
// ("carrier"), and what it holds of the spaces it carries, sealed to that
// person ("carry"), so that the other side sends only what it lacks. Empty
// without a person.
[[nodiscard]] QJsonObject introduction(const Library &library);

// Whether the side whose introduction is `theirs` carries and is this
// library's person.
[[nodiscard]] bool isOneself(const Library &library, const QJsonObject &theirs);
// Whether it is, and either side is in a space or subscribes to one: whether
// a sync with it has anything to carry.
[[nodiscard]] bool worthCarrying(const Library &library, const QJsonObject &theirs);

// What to carry to the other side, whose introduction is `theirs`, when it is
// oneself (isOneself): `carried` (the sealed spaces) and `carried_signature`.
// Empty otherwise, or when it lacks nothing.
[[nodiscard]] QJsonObject offer(Library &library, const QJsonObject &theirs);

struct Taken {
    qsizetype spaces = 0;        // shared spaces new here
    qsizetype keys = 0;          // their keys, kept here
    qsizetype operations = 0;    // of the shared spaces, taken in
    qsizetype refused = 0;       // of them, refused by the role rules
    qsizetype subscriptions = 0; // subscriptions new here
    qsizetype dropped = 0;       // subscriptions given up on another of one's computers
    QStringList problems;        // what couldn't be taken, and why
};

// Takes in what the other side carried here (`theirs`, its request or
// answer), once its signature shows it is from this library's person, and
// drops the subscriptions the person gave up on another of their computers.
[[nodiscard]] Taken take(Library &library, const QJsonObject &theirs);

} // namespace SpaceCarry
