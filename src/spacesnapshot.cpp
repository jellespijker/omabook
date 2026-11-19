#include "spacesnapshot.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSaveFile>
#include <utility>

using namespace Qt::StringLiterals;

namespace {

constexpr auto formatName = "omabook-space"_L1;
// The length of an operation as major version 1 writes it; a later minor
// version may add to its end.
constexpr qsizetype operationLength = 8;

// One value as compact JSON: a document holds only an object or an array,
// so it is written as an array of one and that array's brackets cut off.
[[nodiscard]] QByteArray compact(const QJsonValue &value) {
    const QByteArray wrapped = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return wrapped.mid(1, wrapped.size() - 2);
}

[[nodiscard]] QJsonArray operationJson(const SpaceOperation &op) {
    return {op.device, op.seq, op.hlc, op.author, op.entity, op.id, op.field, op.value};
}

[[nodiscard]] std::expected<void, QString> knownVersion(const QString &version) {
    bool isNumber = false;
    const int found = version.section(u'.', 0, 0).toInt(&isNumber);
    if (!isNumber || found < 1)
        return std::unexpected(u"This space file is of a format Omabook doesn't know (version \"%1\")."_s.arg(version));
    if (found > SpaceSnapshotFile::major)
        return std::unexpected(u"This space was written by a newer Omabook, in format %1, and this Omabook reads format %2. "
                               "Update Omabook to open it."_s.arg(version)
                                   .arg(SpaceSnapshotFile::major));
    return {};
}

[[nodiscard]] bool isText(const QJsonValue &value, bool mayBeEmpty) {
    return value.isString() && (mayBeEmpty || !value.toString().isEmpty());
}

[[nodiscard]] std::expected<SpaceOperation, QString> operationFrom(const QJsonValue &value, const QString &space, qsizetype index) {
    const QJsonArray op = value.toArray();
    const bool whole = op.size() >= operationLength && isText(op.at(0), false) && op.at(1).isDouble() && op.at(1).toInteger() > 0
                       && op.at(2).isDouble() && isText(op.at(3), true) && isText(op.at(4), false) && isText(op.at(5), false)
                       && isText(op.at(6), true) && isText(op.at(7), true);
    if (!whole)
        return std::unexpected(u"Operation %1 of this space file is not whole, so the file was not read."_s.arg(index + 1));
    return SpaceOperation{.space = space,
                          .device = op.at(0).toString(),
                          .seq = op.at(1).toInteger(),
                          .hlc = op.at(2).toInteger(),
                          .author = op.at(3).toString(),
                          .entity = op.at(4).toString(),
                          .id = op.at(5).toString(),
                          .field = op.at(6).toString(),
                          .value = op.at(7).toString()};
}

} // namespace

QByteArray SpaceSnapshotFile::write(const SpaceSnapshot &snapshot) {
    const QList<std::pair<QString, QJsonValue>> header{
        {u"format"_s, QString(formatName)},
        {u"version"_s, u"%1.%2"_s.arg(major).arg(minor)},
        {u"written_by"_s, snapshot.writtenBy},
        {u"space"_s, snapshot.space},
        {u"kind"_s, snapshot.kind},
        {u"name"_s, snapshot.name},
        {u"owner"_s, QJsonObject{{"name", snapshot.owner.name}, {"person", snapshot.owner.person}}},
        {u"clock"_s, snapshot.clock},
        {u"parts"_s, QJsonArray::fromStringList(snapshot.parts)},
    };
    QByteArray out = "{\n";
    for (const auto &[key, value] : header)
        out += "  " + compact(key) + ": " + compact(value) + ",\n";
    out += "  \"operations\": [";
    for (qsizetype at = 0; at < snapshot.operations.size(); ++at)
        out += (at == 0 ? "\n    " : ",\n    ") + compact(operationJson(snapshot.operations.at(at)));
    out += snapshot.operations.isEmpty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

std::expected<SpaceSnapshot, QString> SpaceSnapshotFile::read(QByteArrayView bytes) {
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes.toByteArray(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::unexpected(u"This is not an Omabook space file: it can't be read (%1)."_s.arg(error.errorString()));
    const QJsonObject file = document.object();
    if (file.value("format").toString() != formatName)
        return std::unexpected(u"This is not an Omabook space file."_s);
    if (const auto known = knownVersion(file.value("version").toString()); !known)
        return std::unexpected(known.error());
    const QJsonObject owner = file.value("owner").toObject();
    SpaceSnapshot snapshot{.space = file.value("space").toString(),
                           .kind = file.value("kind").toString(),
                           .name = file.value("name").toString(),
                           .owner = {.name = owner.value("name").toString(), .person = owner.value("person").toString()},
                           .parts = {},
                           .clock = file.value("clock").toInteger(),
                           .writtenBy = file.value("written_by").toString(),
                           .operations = {}};
    if (snapshot.space.isEmpty() || snapshot.kind.isEmpty() || !file.value("operations").isArray())
        return std::unexpected(u"This space file doesn't say which space it is of, or holds no operations."_s);
    for (const auto &part : file.value("parts").toArray())
        if (part.isString())
            snapshot.parts << part.toString();
    const QJsonArray operations = file.value("operations").toArray();
    snapshot.operations.reserve(operations.size());
    for (qsizetype at = 0; at < operations.size(); ++at) {
        auto op = operationFrom(operations.at(at), snapshot.space, at);
        if (!op)
            return std::unexpected(op.error());
        snapshot.operations << std::move(*op);
    }
    return snapshot;
}

std::expected<void, QString> SpaceSnapshotFile::save(const SpaceSnapshot &snapshot, const QString &path) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return std::unexpected(u"%1 can't be written: %2"_s.arg(path, file.errorString()));
    const QByteArray bytes = write(snapshot);
    if (file.write(bytes) != bytes.size() || !file.commit())
        return std::unexpected(u"%1 can't be written: %2"_s.arg(path, file.errorString()));
    return {};
}

std::expected<SpaceSnapshot, QString> SpaceSnapshotFile::load(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(u"%1 can't be read: %2"_s.arg(path, file.errorString()));
    return read(file.readAll());
}
