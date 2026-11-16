#pragma once

#include <QString>
#include <expected>

class Library;

// The database's format history (ADR 0001, Migration step 1): each step takes
// a library from one schema version to the next inside a transaction, after
// backing the database up first.
class LibraryMigrations {
public:
    [[nodiscard]] static int currentFormat() noexcept;
    [[nodiscard]] static std::expected<void, QString> migrate(const Library &library);
};
