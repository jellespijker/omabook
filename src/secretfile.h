#pragma once

#include <QString>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// A file that holds a secret (a person's key, a space's key): readable and
// writable by its owner alone (0600), put in place whole or not at all, and
// refused when read if anyone else may read it or it isn't the owner's.
namespace SecretFile {

enum class Placing : std::uint8_t { KeepWhatIsThere, Replace };

// Writes `bytes` to `file` through a temporary file made for its owner only,
// synced, then renamed into place: with KeepWhatIsThere never over a file
// that is there (another program's, kept first), with Replace in its place.
[[nodiscard]] std::expected<void, QString> keep(const QString &file, std::span<const std::byte> bytes, Placing placing);

// Reads `file` into `into`, which it must fill exactly. Refused when the file
// is not a regular file of this user's, may be read by anyone else, or is
// of another size (then it is not a key file of `kind`, as the message says).
[[nodiscard]] std::expected<void, QString> read(const QString &file, std::span<std::byte> into, const QString &kind);

} // namespace SecretFile
