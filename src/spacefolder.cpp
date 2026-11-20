#include "spacefolder.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <optional>

#include "person.h"
#include "signedops.h"

using namespace Qt::StringLiterals;

namespace {

constexpr int digits = 12;
// The pieces a book's file is sealed in: each a part of the sealed stream.
constexpr qsizetype piece = qsizetype{1024} * 1024;
constexpr auto filesFolder = "files"_L1;

// What each piece of a book's file is sealed for: this space, this file.
[[nodiscard]] QString sealedFor(const QString &space, const QString &hash) { return space + u"\x1f"_s + u"file:"_s + hash; }

[[nodiscard]] QString sha256(QByteArrayView bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

struct Named {
    QString device;
    qint64 first = 0;
    qint64 last = 0;
};

// What a batch's name says of it, or none for a file that is not a batch.
[[nodiscard]] std::optional<Named> named(const QString &file) {
    static const QRegularExpression pattern(u"^([A-Za-z0-9]+)-(\\d{12})-(\\d{12})\\.omaseal$"_s);
    const QRegularExpressionMatch match = pattern.match(file);
    if (!match.hasMatch())
        return std::nullopt;
    return Named{.device = match.captured(1), .first = match.captured(2).toLongLong(), .last = match.captured(3).toLongLong()};
}

[[nodiscard]] QStringList batchFiles(const QString &folder) {
    return QDir(folder).entryList({u"*"_s + QLatin1StringView(SpaceFolder::suffix)}, QDir::Files, QDir::Name);
}

// An operation as a batch carries it: its signer's key and signature apart.
[[nodiscard]] std::optional<SignedOperation> signedOf(const SpaceOperation &op) {
    const auto signer = Person::publicKeyFrom(op.signer);
    if (!signer || std::cmp_not_equal(op.signature.size(), crypto_sign_BYTES))
        return std::nullopt;
    SignedOperation signedOp{.op = op, .signer = *signer, .signature = {}};
    signedOp.op.signer.clear();
    signedOp.op.signature.clear();
    std::ranges::copy(Crypto::bytes(op.signature), signedOp.signature.begin());
    return signedOp;
}

[[nodiscard]] SpaceOperation operationOf(const SignedOperation &signedOp) {
    SpaceOperation op = signedOp.op;
    op.signer = Person::idOf(signedOp.signer);
    op.signature = Crypto::toByteArray(signedOp.signature);
    return op;
}

[[nodiscard]] std::expected<QList<SpaceOperation>, QString> openBatch(const QString &path, const QString &space,
                                                                      std::span<const SpaceKey> keys, const Named &name) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(u"It can't be read: %1"_s.arg(file.errorString()));
    const QByteArray batch = file.readAll();
    std::expected<QList<SignedOperation>, QString> opened = std::unexpected(u"This computer holds no key of the space."_s);
    for (const SpaceKey &key : keys) {
        opened = SignedOps::open(key, space, batch);
        if (opened)
            break;
    }
    if (!opened)
        return std::unexpected(keys.size() > 1 ? u"It opens with none of the space's keys held here: %1"_s.arg(opened.error())
                                               : opened.error());
    QList<SpaceOperation> ops;
    for (const SignedOperation &signedOp : *opened) {
        if (signedOp.op.device != name.device || signedOp.op.seq < name.first || signedOp.op.seq > name.last)
            return std::unexpected(u"It holds operations its name doesn't say it holds."_s);
        ops << operationOf(signedOp);
    }
    return ops;
}

} // namespace

QString SpaceFolder::of(const QString &folder, const QString &space) {
    return QDir(folder).absoluteFilePath(space.startsWith("shared:"_L1) ? space.mid(7) : space);
}

qint64 SpaceFolder::lastWritten(const QString &folder, const QString &space, const QString &device) {
    qint64 last = 0;
    for (const QString &file : batchFiles(of(folder, space)))
        if (const auto name = named(file); name && name->device == device)
            last = std::max(last, name->last);
    return last;
}

std::expected<QString, QString> SpaceFolder::write(const QString &folder, const QString &space, const SpaceKey &key, const QString &device,
                                                   std::span<const SpaceOperation> ops) {
    if (ops.empty())
        return std::unexpected(u"There is nothing to write."_s);
    QList<SignedOperation> sealed;
    qint64 first = ops.front().seq;
    qint64 last = first;
    for (const SpaceOperation &op : ops) {
        const auto signedOp = signedOf(op);
        if (op.device != device || op.space != space || !signedOp)
            return std::unexpected(u"Only this computer's signed operations of the space go in its batches."_s);
        first = std::min(first, op.seq);
        last = std::max(last, op.seq);
        sealed << *signedOp;
    }
    const QString into = of(folder, space);
    if (!QDir().mkpath(into))
        return std::unexpected(u"Can't make the folder %1."_s.arg(into));
    QString path = QDir(into).absoluteFilePath(
        u"%1-%2-%3%4"_s.arg(device).arg(first, digits, 10, QChar(u'0')).arg(last, digits, 10, QChar(u'0')).arg(QLatin1StringView(suffix)));
    if (QFileInfo::exists(path))
        return std::unexpected(u"%1 is there already: a batch is written once."_s.arg(path));
    // Whole or not at all: something that syncs the folder never sees half a batch.
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(SignedOps::seal(key, space, sealed)) < 0 || !file.commit())
        return std::unexpected(u"Can't write %1: %2"_s.arg(path, file.errorString()));
    return path;
}

SpaceFolder::Read SpaceFolder::read(const QString &folder, const QString &space, std::span<const SpaceKey> keys,
                                    const QHash<QString, qint64> &held) {
    Read read;
    const QString from = of(folder, space);
    QHash<QString, QString> waiting; // device → its batch that didn't open
    for (const QString &file : batchFiles(from)) {
        const auto name = named(file);
        if (!name) {
            read.refused << u"%1: its name is not one of a batch."_s.arg(file);
            continue;
        }
        if (name->last <= held.value(name->device))
            continue;
        if (waiting.contains(name->device)) {
            read.refused << u"%1: it waits for %2, before it, which didn't open."_s.arg(file, waiting.value(name->device));
            continue;
        }
        const auto ops = openBatch(QDir(from).absoluteFilePath(file), space, keys, *name);
        if (!ops) {
            read.refused << u"%1: %2"_s.arg(file, ops.error());
            waiting.insert(name->device, file);
            continue;
        }
        ++read.batches;
        read.ops += *ops;
    }
    return read;
}

QString SpaceFolder::fileOf(const QString &folder, const QString &space, std::span<const SpaceKey> keys, const QString &hash) {
    if (keys.empty() || hash.isEmpty())
        return {};
    return QDir(of(folder, space))
        .absoluteFilePath(filesFolder + u'/' + keys.front().nameFor(u"file:"_s + hash) + QLatin1StringView(suffix));
}

std::expected<bool, QString> SpaceFolder::writeFile(const QString &folder, const QString &space, std::span<const SpaceKey> keys,
                                                    const SpaceKey &sealing, const std::pair<QString, QString> &hashAndPath) {
    const auto &[hash, path] = hashAndPath;
    const QString target = fileOf(folder, space, keys, hash);
    if (target.isEmpty())
        return std::unexpected(u"This computer holds no key of the space."_s);
    if (QFileInfo::exists(target))
        return false;
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly))
        return std::unexpected(u"Can't read %1: %2"_s.arg(path, source.errorString()));
    const QByteArray bytes = source.readAll();
    if (sha256(bytes) != hash)
        return std::unexpected(u"%1 is not the file it should be: its content changed."_s.arg(path));
    QList<QByteArray> pieces;
    for (qsizetype at = 0; at < bytes.size(); at += piece)
        pieces << bytes.mid(at, piece);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return std::unexpected(u"Can't make the folder %1."_s.arg(QFileInfo(target).absolutePath()));
    // Whole or not at all, as a batch.
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || file.write(sealing.seal(sealedFor(space, hash), pieces)) < 0 || !file.commit())
        return std::unexpected(u"Can't write %1: %2"_s.arg(target, file.errorString()));
    return true;
}

std::expected<void, QString> SpaceFolder::readFile(const QString &folder, const QString &space, std::span<const SpaceKey> keys,
                                                   const QString &hash, const QString &into) {
    const QString from = fileOf(folder, space, keys, hash);
    QFile file(from);
    if (from.isEmpty() || !file.open(QIODevice::ReadOnly))
        return std::unexpected(
            u"The space's folder %1 doesn't hold the file yet: whoever has it puts it there at their next sync."_s.arg(of(folder, space)));
    const QByteArray sealed = file.readAll();
    std::expected<QList<QByteArray>, QString> opened = std::unexpected(u"This computer holds no key of the space."_s);
    for (const SpaceKey &key : keys) {
        opened = key.open(sealedFor(space, hash), sealed);
        if (opened)
            break;
    }
    if (!opened)
        return std::unexpected(u"The file in the space's folder doesn't open: %1"_s.arg(opened.error()));
    QByteArray bytes;
    for (const QByteArray &part : std::as_const(*opened))
        bytes += part;
    if (sha256(bytes) != hash)
        return std::unexpected(u"The file in the space's folder is not the one the space names: its hash differs."_s);
    QSaveFile out(into);
    if (!out.open(QIODevice::WriteOnly) || out.write(bytes) < 0 || !out.commit())
        return std::unexpected(u"Can't write %1: %2"_s.arg(into, out.errorString()));
    return {};
}
