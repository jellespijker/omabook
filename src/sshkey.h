#pragma once

#include <QByteArray>
#include <QString>
#include <QStringView>
#include <expected>

#include "failure.h"

// An ssh key of Omabook's own (#180): the key a computer opens its hubs with,
// ed25519, made with libsodium and kept in OpenSSH's own format, so that ssh
// takes it as one of its keys and no ssh-keygen is needed to make it. Only
// ed25519 public keys are read: a hub links no other kind.
namespace SshKey {

// A public key as ssh sends it: its type and its bytes (the "AAAA..." of a
// public line, decoded).
struct Public {
    QByteArray type;
    QByteArray blob;
};

// Makes a new key pair: the secret half in `file` (0600, never over a file
// there), the public half in `file`.pub, `comment` after it.
[[nodiscard]] std::expected<Public, Failure> generate(const QString &file, const QString &comment);

// The public half of the key in `file`, read from `file`.pub.
[[nodiscard]] std::expected<Public, Failure> publicOf(const QString &file);

// An ed25519 public line, "ssh-ed25519 AAAA... comment": refused when it is
// anything else (options before it, another type, a blob that isn't one key).
[[nodiscard]] std::expected<Public, Failure> parse(QStringView line);

// "ssh-ed25519 AAAA...", without a comment.
[[nodiscard]] QString line(const Public &key);

// Its fingerprint as ssh-keygen -l prints it: "SHA256:" and the hash in Base64.
[[nodiscard]] QString fingerprint(const Public &key);

} // namespace SshKey
