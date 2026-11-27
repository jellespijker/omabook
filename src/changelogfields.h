#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QtTypes>
#include <optional>
#include <utility>

class Library;
struct Book;

// The fields of one thing as the change log holds them, and writing a row's
// field into it: what ChangeLog and ChangeLogGraph share (ADR 0001).
namespace ChangeLogFields {

// The latest operation of a field, and the value this device last wrote of it.
struct Logged {
    qint64 hlc = -1;
    QString device;
    QString value;
    std::optional<QString> own;
    qint64 ownHlc = -1; // and the stamp it wrote it with
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
// (SpaceReducer::reduce). Stamped after what the log has of it, but for a
// value taken from another device's operation, which keeps that one's stamp.
void write(Context &context, const QString &field, const QString &value, qint64 rowHlc);

// A book's fields as its row says them, by their names in the log: its
// details (stamped as its details are), and what is said of its reading.
using Fields = QList<std::pair<QString, QString>>;
[[nodiscard]] Fields bookDetails(const Library &library, const Book &book);
[[nodiscard]] Fields bookReading(const Book &book);

// A value another device wrote, the latest the log has of `field`, that this
// device's operations don't say: written by this device too, at that stamp,
// as write() does of a book's (#210). At the same stamp and value it changes
// no outcome. Whether it was written.
bool echo(Context &context, const QString &field, const QString &value);

// Of every book, each field its row holds as another device wrote it and this
// one's operations don't say (a book got by a sync before #210), echoed: the
// one-off repair of format 45 (#213). The number of books written.
[[nodiscard]] qsizetype echoTaken(const Library &library);

} // namespace ChangeLogFields
