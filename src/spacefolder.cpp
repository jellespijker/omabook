#include "spacefolder.h"

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

[[nodiscard]] std::expected<QList<SpaceOperation>, QString> openBatch(const QString &path, const QString &space, const SpaceKey &key,
                                                                      const Named &name) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(u"It can't be read: %1"_s.arg(file.errorString()));
    const auto opened = SignedOps::open(key, space, file.readAll());
    if (!opened)
        return std::unexpected(opened.error());
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

SpaceFolder::Read SpaceFolder::read(const QString &folder, const QString &space, const SpaceKey &key, const QHash<QString, qint64> &held) {
    Read read;
    const QString from = of(folder, space);
    for (const QString &file : batchFiles(from)) {
        const auto name = named(file);
        if (!name) {
            read.refused << u"%1: its name is not one of a batch."_s.arg(file);
            continue;
        }
        if (name->last <= held.value(name->device))
            continue;
        const auto ops = openBatch(QDir(from).absoluteFilePath(file), space, key, *name);
        if (!ops) {
            read.refused << u"%1: %2"_s.arg(file, ops.error());
            continue;
        }
        ++read.batches;
        read.ops += *ops;
    }
    return read;
}
