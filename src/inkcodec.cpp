#include "inkcodec.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

using namespace Qt::StringLiterals;

namespace Ink {

namespace {

constexpr quint8 strokeVersion = 1;
constexpr quint8 snapshotVersion = 1;
constexpr quint8 pressuredFlag = 0x01;
constexpr quint8 tiltedFlag = 0x02;
// A bit of the flags and no byte more (#130): a stroke of before reads as it did.
constexpr quint8 highlighterFlag = 0x04;
// Every flag above.
constexpr quint8 knownFlags = 0x07;
constexpr double positionSteps = 8.0;
constexpr double pressureSteps = 255.0;
// Far beyond any page, and well inside what the differences can say.
constexpr qint64 largestPosition = 1'073'741'824;
constexpr qint64 largestWidth = 8000;
constexpr qsizetype largestId = 256;

// Small numbers either side of nought as small unsigned ones: 0, -1, 1, -2… as 0, 1, 2, 3…
quint64 zigzag(qint64 value) {
    const auto bits = static_cast<quint64>(value);
    return value < 0 ? (~bits << 1U) | 1U : bits << 1U;
}
qint64 unzigzag(quint64 value) { return static_cast<qint64>((value & 1U) != 0 ? ~(value >> 1U) : value >> 1U); }

class Writer {
public:
    void byte(quint8 value) { m_bytes.append(static_cast<char>(value)); }
    void varint(quint64 value) {
        while (value >= 0x80U) {
            byte(static_cast<quint8>((value & 0x7fU) | 0x80U));
            value >>= 7U;
        }
        byte(static_cast<quint8>(value));
    }
    void signedVarint(qint64 value) { varint(zigzag(value)); }
    void bytes(QByteArrayView view) { m_bytes.append(view); }
    [[nodiscard]] QByteArray take() && { return std::move(m_bytes); }

private:
    QByteArray m_bytes;
};

class Reader {
public:
    explicit Reader(QByteArrayView bytes) : m_bytes(bytes) {}

    [[nodiscard]] std::optional<quint8> byte() {
        if (m_at >= m_bytes.size()) {
            return std::nullopt;
        }
        return static_cast<quint8>(m_bytes.at(m_at++));
    }
    [[nodiscard]] std::optional<quint64> varint() {
        quint64 value = 0;
        for (unsigned shift = 0; shift < 64; shift += 7) {
            const auto next = byte();
            if (!next) {
                return std::nullopt;
            }
            value |= static_cast<quint64>(*next & 0x7fU) << shift;
            if ((*next & 0x80U) == 0) {
                return value;
            }
        }
        return std::nullopt;
    }
    [[nodiscard]] std::optional<qint64> signedVarint() {
        const auto value = varint();
        return value ? std::optional<qint64>{unzigzag(*value)} : std::nullopt;
    }
    [[nodiscard]] std::optional<QByteArrayView> bytes(quint64 count) {
        if (std::cmp_greater(count, m_bytes.size() - m_at)) {
            return std::nullopt;
        }
        const QByteArrayView piece = m_bytes.sliced(m_at, static_cast<qsizetype>(count));
        m_at += static_cast<qsizetype>(count);
        return piece;
    }
    [[nodiscard]] bool atEnd() const { return m_at == m_bytes.size(); }

private:
    QByteArrayView m_bytes;
    qsizetype m_at = 0;
};

// A point as whole steps: what is written, and what a point read back is made of.
struct Steps {
    qint64 x = 0;
    qint64 y = 0;
    qint64 pressure = 0;
    qint64 xTilt = 0;
    qint64 yTilt = 0;
    qint64 ms = 0;
};

Steps stepsOf(const Point &point) {
    return Steps{.x = std::llround(point.at.x() * positionSteps),
                 .y = std::llround(point.at.y() * positionSteps),
                 .pressure = std::llround(std::clamp(static_cast<double>(point.pressure), 0.0, 1.0) * pressureSteps),
                 .xTilt = std::llround(std::clamp(static_cast<double>(point.xTilt), -90.0, 90.0)),
                 .yTilt = std::llround(std::clamp(static_cast<double>(point.yTilt), -90.0, 90.0)),
                 .ms = point.ms};
}

Point pointOf(const Steps &steps) {
    return Point{.at = QPointF(static_cast<double>(steps.x) / positionSteps, static_cast<double>(steps.y) / positionSteps),
                 .pressure = static_cast<float>(static_cast<double>(steps.pressure) / pressureSteps),
                 .xTilt = static_cast<float>(steps.xTilt),
                 .yTilt = static_cast<float>(steps.yTilt),
                 .ms = static_cast<quint32>(steps.ms)};
}

bool sane(const Steps &steps) {
    return std::abs(steps.x) <= largestPosition && std::abs(steps.y) <= largestPosition && steps.pressure >= 0
           && steps.pressure <= static_cast<qint64>(pressureSteps) && std::abs(steps.xTilt) <= 90 && std::abs(steps.yTilt) <= 90
           && steps.ms >= 0 && std::cmp_less_equal(steps.ms, std::numeric_limits<quint32>::max());
}

void writePoint(Writer &out, const Steps &now, const Steps &before, quint8 flags) {
    out.signedVarint(now.x - before.x);
    out.signedVarint(now.y - before.y);
    if ((flags & pressuredFlag) != 0) {
        out.signedVarint(now.pressure - before.pressure);
    }
    if ((flags & tiltedFlag) != 0) {
        out.signedVarint(now.xTilt - before.xTilt);
        out.signedVarint(now.yTilt - before.yTilt);
    }
    // Moments only go forward; one that doesn't is kept as the one before.
    out.varint(static_cast<quint64>(std::max<qint64>(0, now.ms - before.ms)));
}

std::optional<Steps> readPoint(Reader &in, const Steps &before, quint8 flags) {
    Steps now = before;
    const auto dx = in.signedVarint();
    const auto dy = in.signedVarint();
    if (!dx || !dy) {
        return std::nullopt;
    }
    now.x += *dx;
    now.y += *dy;
    if ((flags & pressuredFlag) != 0) {
        const auto dp = in.signedVarint();
        if (!dp) {
            return std::nullopt;
        }
        now.pressure += *dp;
    }
    if ((flags & tiltedFlag) != 0) {
        const auto tx = in.signedVarint();
        const auto ty = in.signedVarint();
        if (!tx || !ty) {
            return std::nullopt;
        }
        now.xTilt += *tx;
        now.yTilt += *ty;
    }
    const auto dms = in.varint();
    if (!dms || *dms > std::numeric_limits<quint32>::max()) {
        return std::nullopt;
    }
    now.ms += static_cast<qint64>(*dms);
    return sane(now) ? std::optional<Steps>{now} : std::nullopt;
}

std::expected<void, QString> readPoints(Reader &in, Stroke &stroke, quint8 flags) {
    const auto count = in.varint();
    if (!count || *count == 0 || *count > maxPoints) {
        return std::unexpected(u"A stroke has from 1 to %1 points."_s.arg(maxPoints));
    }
    stroke.points.reserve(static_cast<std::size_t>(*count));
    Steps before{};
    for (quint64 i = 0; i < *count; ++i) {
        const auto now = readPoint(in, before, flags);
        if (!now) {
            return std::unexpected(u"Point %1 of a stroke is cut short or out of bounds."_s.arg(i));
        }
        stroke.points.push_back(pointOf(*now));
        if ((flags & pressuredFlag) == 0) {
            // Unused without pressure; as a stroke without it is made.
            stroke.points.back().pressure = Point{}.pressure;
        }
        before = *now;
    }
    return {};
}

} // namespace

QByteArray encode(const Stroke &stroke) {
    Writer out;
    const quint8 flags =
        (stroke.pressured ? pressuredFlag : 0U) | (stroke.tilted ? tiltedFlag : 0U) | (stroke.highlighter ? highlighterFlag : 0U);
    out.byte(strokeVersion);
    out.byte(flags);
    const QRgb argb = stroke.color.rgba();
    for (const unsigned shift : {24U, 16U, 8U, 0U}) {
        out.byte(static_cast<quint8>((argb >> shift) & 0xffU));
    }
    out.varint(static_cast<quint64>(std::clamp<qint64>(std::llround(stroke.width * positionSteps), 1, largestWidth)));
    out.signedVarint(stroke.began);
    const std::size_t count = std::min(stroke.points.size(), maxPoints);
    out.varint(count);
    Steps before{};
    for (std::size_t i = 0; i < count; ++i) {
        const Steps now = stepsOf(stroke.points.at(i));
        writePoint(out, now, before, flags);
        before = now;
    }
    return std::move(out).take();
}

std::expected<Stroke, QString> decode(QByteArrayView bytes, const QString &id) {
    Reader in(bytes);
    const auto version = in.byte();
    if (!version || *version != strokeVersion) {
        return std::unexpected(u"A stroke of an unknown version."_s);
    }
    const auto flags = in.byte();
    if (!flags || (*flags | knownFlags) != knownFlags) {
        return std::unexpected(u"A stroke with flags this Omabook doesn't know."_s);
    }
    QRgb argb = 0;
    for (int i = 0; i < 4; ++i) {
        const auto part = in.byte();
        if (!part) {
            return std::unexpected(u"A stroke cut short in its colour."_s);
        }
        argb = (argb << 8U) | *part;
    }
    const auto width = in.varint();
    const auto began = in.signedVarint();
    if (!width || *width == 0 || std::cmp_greater(*width, largestWidth) || !began) {
        return std::unexpected(u"A stroke's width or moment is cut short or out of bounds."_s);
    }
    Stroke stroke{.id = id,
                  .color = QColor::fromRgba(argb),
                  .width = static_cast<double>(*width) / positionSteps,
                  .pressured = (*flags & pressuredFlag) != 0,
                  .tilted = (*flags & tiltedFlag) != 0,
                  .highlighter = (*flags & highlighterFlag) != 0,
                  .began = *began,
                  .points = {}};
    if (const auto read = readPoints(in, stroke, *flags); !read) {
        return std::unexpected(read.error());
    }
    if (!in.atEnd()) {
        return std::unexpected(u"A stroke with bytes after its last point."_s);
    }
    return stroke;
}

QByteArray encodeAll(std::span<const Stroke> strokes) {
    Writer out;
    out.byte(snapshotVersion);
    out.varint(strokes.size());
    for (const Stroke &stroke : strokes) {
        const QByteArray id = stroke.id.toUtf8();
        const QByteArray bytes = encode(stroke);
        out.varint(static_cast<quint64>(id.size()));
        out.bytes(id);
        out.varint(static_cast<quint64>(bytes.size()));
        out.bytes(bytes);
    }
    return std::move(out).take();
}

std::expected<std::vector<Stroke>, QString> decodeAll(QByteArrayView bytes) {
    Reader in(bytes);
    const auto version = in.byte();
    if (!version || *version != snapshotVersion) {
        return std::unexpected(u"Ink of an unknown version."_s);
    }
    const auto count = in.varint();
    // Each stroke takes a few bytes at the least, so a count past the bytes there are is a lie.
    if (!count || std::cmp_greater(*count, bytes.size())) {
        return std::unexpected(u"Ink cut short."_s);
    }
    std::vector<Stroke> strokes;
    strokes.reserve(static_cast<std::size_t>(*count));
    for (quint64 i = 0; i < *count; ++i) {
        const auto idLength = in.varint();
        const auto id = idLength && *idLength <= largestId ? in.bytes(*idLength) : std::nullopt;
        const auto length = id ? in.varint() : std::nullopt;
        const auto body = length ? in.bytes(*length) : std::nullopt;
        if (!body) {
            return std::unexpected(u"Ink cut short at stroke %1."_s.arg(i));
        }
        auto stroke = decode(*body, QString::fromUtf8(*id));
        if (!stroke) {
            return std::unexpected(stroke.error());
        }
        strokes.push_back(std::move(*stroke));
    }
    if (!in.atEnd()) {
        return std::unexpected(u"Ink with bytes after its last stroke."_s);
    }
    return strokes;
}

std::vector<QJsonObject> batches(const Change &change) {
    std::vector<QJsonObject> out;
    QJsonObject added;
    QJsonArray removed;
    qsizetype bytes = 0;
    const auto full = [&](qsizetype more) { return added.size() + removed.size() + 1 > maxBatchItems || bytes + more > maxBatchBytes; };
    const auto close = [&] {
        if (!added.isEmpty() || !removed.isEmpty()) {
            out.push_back(QJsonObject{{u"add"_s, added}, {u"remove"_s, removed}});
        }
        added = {};
        removed = {};
        bytes = 0;
    };
    // What is added first, then the removals, so an erasing in part goes in
    // one batch with its pieces unless it is too large for one; and a change
    // too large for one (the lasso moving a page's every stroke, #137) adds
    // its strokes before it takes the old ones away: kept batch by batch,
    // stopped half way, a stroke is there twice, never lost.
    for (const Stroke &stroke : change.added) {
        const QString value = QString::fromLatin1(encode(stroke).toBase64());
        const qsizetype more = stroke.id.size() + value.size() + 6;
        if (full(more)) {
            close();
        }
        added.insert(stroke.id, value);
        bytes += more;
    }
    for (const QString &id : change.removed) {
        const qsizetype more = id.size() + 4;
        if (full(more)) {
            close();
        }
        removed.append(id);
        bytes += more;
    }
    close();
    return out;
}

QJsonObject items(std::span<const Stroke> strokes) {
    QJsonObject out;
    for (const Stroke &stroke : strokes) {
        out.insert(stroke.id, QString::fromLatin1(encode(stroke).toBase64()));
    }
    return out;
}

Read fromItems(const QJsonObject &items) {
    Read read;
    read.strokes.reserve(static_cast<std::size_t>(items.size()));
    for (auto item = items.begin(); item != items.end(); ++item) {
        auto stroke = decode(QByteArray::fromBase64(item.value().toString().toLatin1()), item.key());
        if (stroke) {
            read.strokes.push_back(std::move(*stroke));
        } else {
            read.problems.append(u"Stroke %1: %2"_s.arg(item.key(), stroke.error()));
        }
    }
    return read;
}

std::expected<Change, QString> fromBatch(const QJsonObject &batch) {
    Read added = fromItems(batch.value(u"add"_s).toObject());
    if (!added.problems.isEmpty()) {
        return std::unexpected(added.problems.first());
    }
    Change change{.added = std::move(added.strokes), .removed = {}};
    for (const auto &id : batch.value(u"remove"_s).toArray()) {
        if (!id.isString()) {
            return std::unexpected(u"A stroke removed by something other than its id."_s);
        }
        change.removed.append(id.toString());
    }
    return change;
}

} // namespace Ink
