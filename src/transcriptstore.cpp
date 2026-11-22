#include "transcriptstore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <tuple>

using namespace Qt::StringLiterals;

namespace {

constexpr auto planFile = "plan.json"_L1;
constexpr auto wholeFile = "transcript.json"_L1;

[[nodiscard]] QString recordingFolder(const QString &folder, const QString &key) { return folder + u'/' + key; }
[[nodiscard]] QString pieceName(int index) { return u"piece-%1.json"_s.arg(index, 5, 10, QChar(u'0')); }

[[nodiscard]] std::optional<QJsonObject> readJson(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? std::optional(document.object()) : std::nullopt;
}

[[nodiscard]] std::expected<void, Failure> writeJson(const QString &path, const QJsonObject &json) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return std::unexpected(Failure(Failure::Code::FolderUnmade, {QFileInfo(path).absolutePath()}));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(json).toJson(QJsonDocument::Compact)) < 0 || !file.commit())
        return std::unexpected(Failure(Failure::Code::TranscriptUnwritten, {path, file.errorString()}));
    return {};
}

// Where the key of a file is remembered: by the hash of its path.
[[nodiscard]] QString memoOf(const QString &folder, const QString &file) {
    const QByteArray path = QFileInfo(file).absoluteFilePath().toUtf8();
    return folder + u"/files/"_s + QString::fromLatin1(QCryptographicHash::hash(path, QCryptographicHash::Sha1).toHex()) + u".json"_s;
}

[[nodiscard]] QJsonObject memo(const QFileInfo &info, const QString &key) {
    return {{"path", info.absoluteFilePath()}, {"size", info.size()}, {"modified", info.lastModified().toMSecsSinceEpoch()}, {"key", key}};
}

} // namespace

QString TranscriptStore::folder() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/omabook/transcripts"_s;
}

std::expected<QString, Failure> TranscriptStore::keyOf(const QString &folder, const QString &file) {
    const QFileInfo info(file);
    if (const auto known = readJson(memoOf(folder, file)); known && *known == memo(info, known->value("key"_L1).toString()))
        return known->value("key"_L1).toString();
    QFile bytes(file);
    if (!bytes.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::RecordingUnreadable, {info.fileName()}));
    if (bytes.size() == 0)
        return std::unexpected(Failure(Failure::Code::RecordingEmpty, {info.fileName()}));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&bytes))
        return std::unexpected(Failure(Failure::Code::RecordingUnreadable, {info.fileName()}));
    QString key = QString::fromLatin1(hash.result().toHex());
    remember(folder, file, key);
    return key;
}

void TranscriptStore::remember(const QString &folder, const QString &file, const QString &key) {
    // Only a time saved: a key not remembered is read again next time.
    std::ignore = writeJson(memoOf(folder, file), memo(QFileInfo(file), key));
}

TranscriptStore::Pieces TranscriptStore::pieces(const QString &folder, const QString &key) {
    Pieces found;
    const QString here = recordingFolder(folder, key);
    const auto planned = readJson(here + u'/' + planFile);
    if (!planned)
        return found;
    found.piece = planned->value("piece"_L1).toInteger();
    found.count = planned->value("count"_L1).toInt();
    for (int index = 0; index < found.count; ++index)
        if (const auto piece = readJson(here + u'/' + pieceName(index)))
            found.done.insert(index, *piece);
    return found;
}

std::expected<void, Failure> TranscriptStore::plan(const QString &folder, const QString &key, qint64 piece, int count) {
    const Pieces before = pieces(folder, key);
    if (before.piece == piece && before.count == count)
        return {};
    // Pieces of another length don't fit these: written out anew.
    forget(folder, key);
    return writeJson(recordingFolder(folder, key) + u'/' + planFile, {{"piece", piece}, {"count", count}});
}

std::expected<void, Failure> TranscriptStore::keepPiece(const QString &folder, const QString &key, int index, const QJsonObject &piece) {
    return writeJson(recordingFolder(folder, key) + u'/' + pieceName(index), piece);
}

std::expected<void, Failure> TranscriptStore::keepWhole(const QString &folder, const QString &key, const QJsonObject &transcript) {
    return writeJson(recordingFolder(folder, key) + u'/' + wholeFile, transcript);
}

std::optional<QJsonObject> TranscriptStore::whole(const QString &folder, const QString &key) {
    return readJson(recordingFolder(folder, key) + u'/' + wholeFile);
}

void TranscriptStore::forget(const QString &folder, const QString &key) {
    if (!key.isEmpty())
        QDir(recordingFolder(folder, key)).removeRecursively();
}
