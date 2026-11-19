#include "signedops.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QtEndian>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>
#include <utility>

namespace {

using namespace Qt::StringLiterals;

constexpr std::string_view canonicalMagic{"omabook-op"};

// A JSON object or array in one form only: compact, with its keys in order
// (QJsonObject keeps them sorted). Anything else is kept as it is.
[[nodiscard]] QString canonicalValue(const QString &value) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(value.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !(document.isObject() || document.isArray()))
        return value;
    return QString::fromUtf8(document.toJson(QJsonDocument::Compact));
}

void appendNumber(QByteArray &bytes, qint64 number) {
    std::array<std::byte, sizeof(quint64)> big{};
    qToBigEndian(static_cast<quint64>(number), big.data());
    bytes.append(Crypto::toByteArray(big));
}

void appendText(QByteArray &bytes, const QString &text) {
    const QByteArray utf8 = text.toUtf8();
    Q_ASSERT(std::cmp_less_equal(utf8.size(), std::numeric_limits<quint32>::max()));
    std::array<std::byte, sizeof(quint32)> length{};
    qToBigEndian(static_cast<quint32>(utf8.size()), length.data());
    bytes.append(Crypto::toByteArray(length));
    bytes.append(utf8);
}

// Reads canonical bytes in order. A read past the end leaves it failed, and
// every read after that fails too.
class Reader {
public:
    explicit Reader(std::span<const std::byte> bytes) noexcept : m_rest(bytes) {}

    [[nodiscard]] std::optional<std::span<const std::byte>> take(std::size_t count) {
        m_failed = m_failed || count > m_rest.size();
        if (m_failed)
            return std::nullopt;
        const auto taken = m_rest.first(count);
        m_rest = m_rest.subspan(count);
        return taken;
    }
    [[nodiscard]] std::optional<qint64> number() {
        const auto big = take(sizeof(quint64));
        return big ? std::optional{static_cast<qint64>(qFromBigEndian<quint64>(big->data()))} : std::nullopt;
    }
    [[nodiscard]] std::optional<QString> text() {
        const auto length = take(sizeof(quint32));
        const auto utf8 = length ? take(qFromBigEndian<quint32>(length->data())) : std::nullopt;
        return utf8 ? std::optional{QString::fromUtf8(Crypto::toByteArray(*utf8))} : std::nullopt;
    }
    [[nodiscard]] bool atEnd() const noexcept { return !m_failed && m_rest.empty(); }

private:
    std::span<const std::byte> m_rest;
    bool m_failed = false;
};

[[nodiscard]] bool knownHeader(Reader &reader) {
    const auto magic = reader.take(canonicalMagic.size());
    const auto given = reader.take(1);
    return magic && given && std::ranges::equal(*magic, std::as_bytes(std::span(canonicalMagic)))
           && given->front() == std::byte{SignedOps::version};
}

// A signed operation in a batch: the signer's public key, the signature,
// then the canonical bytes.
[[nodiscard]] QByteArray encode(const SignedOperation &signedOp) {
    return Crypto::toByteArray(signedOp.signer) + Crypto::toByteArray(signedOp.signature) + SignedOps::canonicalBytes(signedOp.op);
}

[[nodiscard]] std::expected<SignedOperation, QString> decode(QByteArrayView bytes) {
    const auto all = Crypto::bytes(bytes);
    constexpr std::size_t keys = std::tuple_size_v<Person::PublicKey> + std::tuple_size_v<Person::Signature>;
    if (all.size() < keys)
        return std::unexpected(u"An operation in the batch is cut short."_s);
    SignedOperation signedOp;
    std::ranges::copy(all.first(signedOp.signer.size()), signedOp.signer.begin());
    std::ranges::copy(all.subspan(signedOp.signer.size(), signedOp.signature.size()), signedOp.signature.begin());
    auto op = SignedOps::fromCanonicalBytes(bytes.sliced(static_cast<qsizetype>(keys)));
    if (!op)
        return std::unexpected(op.error());
    signedOp.op = *op;
    return signedOp;
}

} // namespace

QByteArray SignedOps::canonicalBytes(const SpaceOperation &op) {
    QByteArray bytes(canonicalMagic.data(), canonicalMagic.size());
    bytes.append(version);
    appendText(bytes, op.space);
    appendText(bytes, op.device);
    appendNumber(bytes, op.seq);
    appendNumber(bytes, op.hlc);
    appendText(bytes, op.author);
    appendText(bytes, op.entity);
    appendText(bytes, op.id);
    appendText(bytes, op.field);
    appendText(bytes, canonicalValue(op.value));
    return bytes;
}

std::expected<SpaceOperation, QString> SignedOps::fromCanonicalBytes(QByteArrayView bytes) {
    Reader reader(Crypto::bytes(bytes));
    if (!knownHeader(reader))
        return std::unexpected(u"Not an operation of a kind this Omabook knows."_s);
    const auto space = reader.text();
    const auto device = reader.text();
    const auto seq = reader.number();
    const auto hlc = reader.number();
    const auto author = reader.text();
    const auto entity = reader.text();
    const auto id = reader.text();
    const auto field = reader.text();
    const auto value = reader.text();
    if (!space || !device || !seq || !hlc || !author || !entity || !id || !field || !value || !reader.atEnd())
        return std::unexpected(u"An operation is cut short, or goes on after its end."_s);
    return SpaceOperation{.space = *space,
                          .device = *device,
                          .seq = *seq,
                          .hlc = *hlc,
                          .author = *author,
                          .entity = *entity,
                          .id = *id,
                          .field = *field,
                          .value = *value};
}

SignedOperation SignedOps::sign(const SpaceOperation &op, const Person &author) {
    return {.op = op, .signer = author.publicKey(), .signature = author.sign(Crypto::bytes(canonicalBytes(op)))};
}

bool SignedOps::verify(const SignedOperation &signedOp) {
    return Person::verify(signedOp.signer, Crypto::bytes(canonicalBytes(signedOp.op)), signedOp.signature);
}

void SignedOps::signAs(SpaceOperation &op, const Person &person) {
    op.author = person.id();
    op.signer = op.author;
    op.signature = Crypto::toByteArray(person.sign(Crypto::bytes(canonicalBytes(op))));
}

bool SignedOps::verify(const SpaceOperation &op) {
    if (op.signer.isEmpty() || op.signer != op.author || std::cmp_not_equal(op.signature.size(), crypto_sign_BYTES))
        return false;
    const auto signer = Person::publicKeyFrom(op.signer);
    if (!signer)
        return false;
    Person::Signature signature{};
    std::ranges::copy(Crypto::bytes(op.signature), signature.begin());
    return Person::verify(*signer, Crypto::bytes(canonicalBytes(op)), signature);
}

QByteArray SignedOps::seal(const SpaceKey &key, QStringView space, std::span<const SignedOperation> ops) {
    QList<QByteArray> messages;
    messages.reserve(static_cast<qsizetype>(ops.size()));
    for (const SignedOperation &signedOp : ops)
        messages << encode(signedOp);
    return key.seal(space, messages);
}

std::expected<QList<SignedOperation>, QString> SignedOps::open(const SpaceKey &key, QStringView space, QByteArrayView batch) {
    const auto messages = key.open(space, batch);
    if (!messages)
        return std::unexpected(messages.error());
    QList<SignedOperation> ops;
    ops.reserve(messages->size());
    for (const QByteArray &message : *messages) {
        auto signedOp = decode(message);
        if (!signedOp)
            return std::unexpected(signedOp.error());
        if (signedOp->op.space != space)
            return std::unexpected(u"The batch of %1 holds an operation of %2."_s.arg(space, signedOp->op.space));
        if (!verify(*signedOp))
            return std::unexpected(u"Operation %1 of %2 is not as its signer signed it."_s.arg(signedOp->op.seq).arg(signedOp->op.device));
        ops << *signedOp;
    }
    return ops;
}
