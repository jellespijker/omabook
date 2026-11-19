#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <algorithm>
#include <array>
#include <cstddef>
#include <sodium.h>
#include <span>

// What the person's key pair and the space keys share (ADR 0001, *People and
// keys*): libsodium made ready once, a holder that wipes secret bytes when it
// goes, and the conversions between Qt's bytes and libsodium's.
namespace Crypto {

// Makes libsodium ready the first time it is asked (sodium_init is safe to
// call again, and a function-local static is made once even across threads).
// False only when the system has no source of randomness to give it.
[[nodiscard]] inline bool ready() noexcept {
    static const bool initialised = sodium_init() >= 0;
    return initialised;
}

// libsodium takes unsigned char; std::byte is the same size and alignment
// and may alias any object, so the view is only spelled differently.
[[nodiscard]] inline const unsigned char *uchars(std::span<const std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a byte view as libsodium spells it, as said above.
    return reinterpret_cast<const unsigned char *>(bytes.data()); // ast-grep-ignore: no-reinterpret-cast
}

[[nodiscard]] inline unsigned char *writableUchars(std::span<std::byte> bytes) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): a byte view as libsodium spells it, as said above.
    return reinterpret_cast<unsigned char *>(bytes.data()); // ast-grep-ignore: no-reinterpret-cast
}

[[nodiscard]] inline std::span<const std::byte> bytes(QByteArrayView data) noexcept {
    return std::as_bytes(std::span(data.data(), static_cast<std::size_t>(data.size())));
}

[[nodiscard]] inline QByteArray toByteArray(std::span<const std::byte> bytes) {
    QByteArray data(static_cast<qsizetype>(bytes.size()), Qt::Uninitialized);
    std::ranges::copy(bytes, std::as_writable_bytes(std::span(data.data(), bytes.size())).begin());
    return data;
}

// Secret bytes that are wiped with sodium_memzero (which the compiler may not
// leave out) when they go, and on being moved from. Never copied: a copy is
// one more place a secret would be left behind.
template <std::size_t N> class SecretBytes {
public:
    SecretBytes() noexcept = default;
    ~SecretBytes() { wipe(); }
    SecretBytes(const SecretBytes &) = delete;
    SecretBytes &operator=(const SecretBytes &) = delete;
    SecretBytes(SecretBytes &&other) noexcept : m_bytes(other.m_bytes) { other.wipe(); }
    SecretBytes &operator=(SecretBytes &&other) noexcept {
        if (this != &other) {
            m_bytes = other.m_bytes;
            other.wipe();
        }
        return *this;
    }

    [[nodiscard]] static constexpr std::size_t size() noexcept { return N; }
    [[nodiscard]] std::span<std::byte, N> span() noexcept { return m_bytes; }
    [[nodiscard]] std::span<const std::byte, N> span() const noexcept { return m_bytes; }
    [[nodiscard]] unsigned char *data() noexcept { return writableUchars(m_bytes); }
    [[nodiscard]] const unsigned char *data() const noexcept { return uchars(m_bytes); }

    void wipe() noexcept { sodium_memzero(m_bytes.data(), m_bytes.size()); }

private:
    std::array<std::byte, N> m_bytes{};
};

} // namespace Crypto
