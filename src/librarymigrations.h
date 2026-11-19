#pragma once

#include <QString>
#include <expected>

class Library;

// The database's format history (ADR 0001, Migration step 1): each step takes
// a library from one schema version to the next, after backing the database
// up first.
class LibraryMigrations {
public:
    [[nodiscard]] static int currentFormat() noexcept;
    // Makes a library that was just opened ready: brought to the newest
    // format and its change log up to date, by one program at a time.
    [[nodiscard]] static std::expected<void, QString> setUp(const Library &library);
    // What a command passes to Library::onBringingUpToDate: the line, on standard error.
    static void sayOnStandardError(const QString &said);

private:
    [[nodiscard]] static std::expected<void, QString> migrate(const Library &library);
    static void sayBringingUpToDate(const Library &library, int schema);
};
