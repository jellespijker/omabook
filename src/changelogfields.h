#pragma once

#include <QHash>
#include <QString>
#include <QtTypes>
#include <optional>

class Library;

// The fields of one thing as the change log holds them, and writing a row's
// field into it: what ChangeLog and ChangeLogGraph share (ADR 0001).
namespace ChangeLogFields {

// The latest operation of a field, and the value this device last wrote of it.
struct Logged {
    qint64 hlc = -1;
    QString device;
    QString value;
    std::optional<QString> own;
};

using Map = QHash<QString, Logged>;

// One thing being written into the log: where its operations go, and what the log has of it.
struct Context {
    const Library *library = nullptr;
    QString space;
    QString entity;
    QString gid;
    Map existing;
    bool baseline = false;
};

[[nodiscard]] Map logged(const Library &library, const QString &entity, const QString &globalId);

// What the row says now of `field`, unless this device already wrote it: an
// operation for every change of a row here, whatever another computer wrote
// of it, so this device's operations are always what its rows are
// (SpaceReducer::reduce). Stamped after what the log has of it.
void write(Context &context, const QString &field, const QString &value, qint64 rowHlc);

} // namespace ChangeLogFields
