#pragma once

#include <QList>
#include <QStringList>

// The database's formats (ADR 0001, Migration step 1): each step the SQL
// that takes a library from the format of its index to the next. Taking the
// steps, a copy kept first, is LibraryMigrations'.
namespace LibrarySchema {

[[nodiscard]] const QList<QStringList> &steps();

} // namespace LibrarySchema
