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
    // format and its change log up to date, and what the log kept of a kind
    // this Omabook now knows taken in (KeptOps), by one program at a time.
    // One this thread is setting up already is not waited for (#60).
    [[nodiscard]] static std::expected<void, QString> setUp(Library &library);
    // What a command passes to Library::onBringingUpToDate: the line, on standard error.
    static void sayOnStandardError(const QString &said);
    // As a library is closed: what the planner knows of its tables brought up
    // to date where that is worth it (PRAGMA optimize), from a sample of each.
    static void closing(const Library &library);

private:
    // How the connection writes: the journal the folder's file system can
    // keep, how often it waits for the disk, and the foreign keys kept.
    static void configure(const Library &library);
    [[nodiscard]] static std::expected<void, QString> migrate(const Library &library);
    static void sayBringingUpToDate(const Library &library, int schema);
    // The repair of format 43 (#158): the links that are not of one book,
    // each recorded as such once (BookLinks::record), and said.
    static void judgeLinks(const Library &library);
    // The repair of format 44 (#164): the identifiers books have of a book
    // kept apart from them, removed once (StrayIdentifiers::remove), and said.
    static void unstrayIdentifiers(Library &library);
};
