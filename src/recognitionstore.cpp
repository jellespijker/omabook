#include "recognitionstore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

#include "library.h"
#include "plugindata.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto partsSet = "parts"_L1;
constexpr auto resultField = "result"_L1;
// Well inside what one write may hold (PluginData::maxValueBytes), with room
// for the change around the part.
constexpr qsizetype partBytes = qsizetype{48} * 1024;

[[nodiscard]] PluginData::Thing thingOf(const QString &owner, const QString &key) {
    return {.plugin = owner, .entity = QString::fromLatin1(RecognitionStore::entity), .id = key};
}

[[nodiscard]] qsizetype sizeOf(const QJsonObject &segment) { return QJsonDocument(segment).toJson(QJsonDocument::Compact).size(); }

// A segment too large for a part of its own (minutes of speech without a
// pause), as pieces of its words that each fit.
[[nodiscard]] QJsonArray piecesOf(const QJsonObject &segment) {
    if (sizeOf(segment) <= partBytes)
        return {segment};
    QJsonArray pieces;
    const QJsonArray words = segment.value("words"_L1).toArray();
    QJsonArray piece;
    QStringList text;
    const auto close = [&] {
        if (piece.isEmpty())
            return;
        pieces.append(QJsonObject{{"text", text.join(u' ')},
                                  {"start", piece.first().toObject().value("start"_L1)},
                                  {"end", piece.last().toObject().value("end"_L1)},
                                  {"words", piece}});
        piece = {};
        text.clear();
    };
    qsizetype bytes = 0;
    for (const QJsonValue &word : words) {
        const qsizetype size = sizeOf(word.toObject()) * 2; // its text is said twice, in the piece's own
        if (bytes + size > partBytes) {
            close();
            bytes = 0;
        }
        piece.append(word);
        text << word.toObject().value("text"_L1).toString();
        bytes += size;
    }
    close();
    return pieces;
}

// The segments in parts that each fit in one write.
[[nodiscard]] QList<QJsonArray> partsOf(const QJsonArray &segments) {
    QList<QJsonArray> parts{QJsonArray()};
    qsizetype bytes = 0;
    for (const QJsonValue &whole : segments)
        for (const QJsonValue &segment : piecesOf(whole.toObject())) {
            const qsizetype size = sizeOf(segment.toObject());
            if (bytes + size > partBytes && !parts.last().isEmpty()) {
                parts.append(QJsonArray());
                bytes = 0;
            }
            parts.last().append(segment);
            bytes += size;
        }
    return parts;
}

[[nodiscard]] QString partKey(qint64 at, qsizetype index) { return u"%1.%2"_s.arg(at).arg(index, 5, 10, QChar(u'0')); }

} // namespace

std::expected<void, QString> RecognitionStore::keep(const Library &library, const QString &owner, const QString &key,
                                                    const Recognition::Result &result) {
    QJsonObject whole = result.toJson();
    const QList<QJsonArray> parts = partsOf(whole.take("segments"_L1).toArray());
    whole.remove("text"_L1); // the segments' own, joined
    whole.remove("kept"_L1);
    whole.insert("parts", parts.size());
    // What an earlier recognition of the same input left goes with it.
    QStringList earlier;
    if (const QJsonValue before = PluginData::read(library, thingOf(owner, key)); before.isObject())
        earlier = before.toObject().value("sets"_L1).toObject().value(partsSet).toObject().keys();
    const Library::Batch batch(library);
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const QStringList removed = index == 0 ? earlier : QStringList();
        if (const auto written = PluginData::change(
                library, thingOf(owner, key), partsSet,
                {.added = {{partKey(result.at, index), QJsonObject{{"segments", parts.at(index)}}}}, .removed = removed});
            !written)
            return written;
    }
    return PluginData::set(library, thingOf(owner, key), {{resultField, whole}});
}

std::optional<Recognition::Result> RecognitionStore::find(const Library &library, const QString &owner, const QString &key) {
    const QJsonValue thing = PluginData::read(library, thingOf(owner, key));
    if (!thing.isObject())
        return std::nullopt;
    QJsonObject whole = thing.toObject().value("fields"_L1).toObject().value(resultField).toObject();
    const QJsonObject parts = thing.toObject().value("sets"_L1).toObject().value(partsSet).toObject();
    const qint64 at = whole.value("at"_L1).toInteger();
    const qsizetype count = whole.value("parts"_L1).toInteger();
    QJsonArray segments;
    QStringList texts;
    for (qsizetype index = 0; index < count; ++index) {
        const QJsonValue part = parts.value(partKey(at, index));
        if (!part.isObject())
            return std::nullopt; // not all of it has come yet
        for (const QJsonValue &segment : part.toObject().value("segments"_L1).toArray()) {
            segments.append(segment);
            texts << segment.toObject().value("text"_L1).toString();
        }
    }
    whole.insert("segments", segments);
    whole.insert("text", texts.join(whole.value("kind"_L1).toString() == "ink"_L1 ? u"\n"_s : u" "_s));
    whole.insert("kept", true);
    return Recognition::Result::fromJson(whole);
}
