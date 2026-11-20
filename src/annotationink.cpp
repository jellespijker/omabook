#include "annotationink.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

#include "changelog.h"
#include "inkcodec.h"
#include "library.h"
#include "log.h"
#include "plugindata.h"
#include "spaces.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto entity = "annotation"_L1;
constexpr auto setName = "strokes"_L1;
constexpr auto setField = "set:strokes"_L1;

// The operations of the annotation's set, every computer's, as the log holds them.
[[nodiscard]] QList<SpaceOperation> setOps(const Library &library, const QString &gid) {
    QList<SpaceOperation> ops;
    for (QSqlQuery query = ChangeLog::query(library,
                                            u"SELECT d.name, o.seq, o.hlc, o.value FROM live_ops AS o JOIN log_ids AS d ON d.id = o.device "
                                            "WHERE o.entity = ? AND o.id = (SELECT id FROM log_ids WHERE name = ?) AND o.field = ?"_s,
                                            {QString(entity), gid, QString(setField)});
         query.next();)
        ops << SpaceOperation{.space = {},
                              .device = query.value(0).toString(),
                              .seq = query.value(1).toLongLong(),
                              .hlc = query.value(2).toLongLong(),
                              .author = {},
                              .entity = QString(entity),
                              .id = gid,
                              .field = QString(setField),
                              .value = query.value(3).toString()};
    return ops;
}

} // namespace

std::vector<Ink::Stroke> AnnotationInk::strokes(const Library &library, qint64 annotationId) {
    const QString gid = Spaces::globalId(library, QString(entity), annotationId);
    if (gid.isEmpty())
        return {};
    const QList<SpaceOperation> ops = setOps(library, gid);
    if (ops.isEmpty())
        return {};
    // A set as a plugin's is (ADR 0002): of each stroke the latest word holds.
    const QJsonArray things = PluginData::reduce(ops);
    const QJsonObject items = things.isEmpty() ? QJsonObject() : things.first()["sets"][setName].toObject();
    Ink::Read read = Ink::fromItems(items);
    for (const QString &problem : std::as_const(read.problems))
        qCWarning(lcLibrary).noquote() << "A stroke of an annotation left out:" << problem;
    std::ranges::stable_sort(
        read.strokes, [](const Ink::Stroke &a, const Ink::Stroke &b) { return a.began != b.began ? a.began < b.began : a.id < b.id; });
    return std::move(read.strokes);
}

bool AnnotationInk::has(const Library &library, qint64 annotationId) { return !strokes(library, annotationId).empty(); }

std::expected<void, QString> AnnotationInk::write(const Library &library, qint64 annotationId, const Ink::Change &change) {
    if (change.empty())
        return {};
    QSqlQuery found = ChangeLog::query(library, u"SELECT 1 FROM annotations WHERE id = ?"_s, {annotationId});
    if (!found.next())
        return std::unexpected(u"There is no annotation %1 in the library."_s.arg(annotationId));
    found.finish();
    const Library::Batch batch(library);
    const QString gid = Spaces::annotationId(library, annotationId);
    if (gid.isEmpty())
        return std::unexpected(u"The annotation %1 has no place in the change log."_s.arg(annotationId));
    // Each within what one write may hold (Ink::batches cuts a large change).
    for (const QJsonObject &one : Ink::batches(change))
        ChangeLog::append(library, {.space = Spaces::personalSpace(library),
                                    .entity = QString(entity),
                                    .id = gid,
                                    .field = QString(setField),
                                    .value = QString::fromUtf8(QJsonDocument(one).toJson(QJsonDocument::Compact)),
                                    .hlc = {}});
    return {};
}
