#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QList>
#include <QString>
#include <QStringView>
#include <expected>
#include <span>

#include "person.h"
#include "spacekey.h"
#include "spacereplay.h"

// An operation as its author signed it (ADR 0001, *People and keys*).
struct SignedOperation {
    SpaceOperation op;
    Person::PublicKey signer{};
    Person::Signature signature{};
};

// Signing operations and sealing batches of them, for the transports to come.
//
// What is signed is an operation's canonical bytes, the same on every computer
// whatever it stores or sends the operation as: "omabook-op" and a version
// byte (1), then the space, device, sequence number, HLC, author, entity, id,
// field and value, in that order. A text is its length in UTF-8 bytes (four
// bytes, big endian) and those bytes; a number is eight bytes, big endian.
// A value that is a JSON object or array is written compactly with its keys
// in order, so two computers that write the same JSON differently sign the
// same bytes.
namespace SignedOps {

inline constexpr char version = 1;

[[nodiscard]] QByteArray canonicalBytes(const SpaceOperation &op);
// The operation in canonical bytes (with its value in canonical form).
[[nodiscard]] std::expected<SpaceOperation, QString> fromCanonicalBytes(QByteArrayView bytes);

[[nodiscard]] SignedOperation sign(const SpaceOperation &op, const Person &author);
// Whether the operation is as its signer signed it.
[[nodiscard]] bool verify(const SignedOperation &signedOp);

// Signs `op` as the log keeps it (ChangeLog::append): `person` becomes its
// author and its signer, and its signature is of the bytes it is kept as.
void signAs(SpaceOperation &op, const Person &person);
// Whether `op` is signed, by its author, and is as they signed it.
[[nodiscard]] bool verify(const SpaceOperation &op);

// Seals signed operations as one batch of `space`.
[[nodiscard]] QByteArray seal(const SpaceKey &key, QStringView space, std::span<const SignedOperation> ops);
// The operations of a batch, refused as a whole when the batch doesn't open
// with this key for `space`, or any operation in it is of another space or
// isn't as its signer signed it.
[[nodiscard]] std::expected<QList<SignedOperation>, QString> open(const SpaceKey &key, QStringView space, QByteArrayView batch);

} // namespace SignedOps
