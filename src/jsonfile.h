#pragma once

#include <QJsonObject>
#include <QString>
#include <expected>
#include <optional>

#include "failure.h"

// A file of one JSON object that is read, changed and written back whole:
// config.json, a plugin's settings, the plugins' sources. One that is there
// and can't be read is never written over, since what it held would be lost.
namespace JsonFile {

// What the file holds, or nothing where there is no file. A file that can't
// be opened or isn't one JSON object fails, with the line of a mistake; the
// log says so once.
[[nodiscard]] std::expected<std::optional<QJsonObject>, Failure> read(const QString &path);
// The same, with an empty object where there is no file.
[[nodiscard]] std::expected<QJsonObject, Failure> object(const QString &path);
// Writes the file whole and atomically, making its folder; refused, with why,
// over a file that can't be read.
[[nodiscard]] std::expected<void, Failure> write(const QString &path, const QJsonObject &object);

} // namespace JsonFile
