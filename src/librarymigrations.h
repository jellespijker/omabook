#pragma once

#include <QString>
#include <expected>
#include <functional>

#include "failure.h"

class Library;

// The database's format history (ADR 0001, Migration step 1): each step takes
// a library from one schema version to the next, after backing the database
// up first.
class LibraryMigrations {
public:
    // What is done just before a library is brought from an older format up
    // to date: a moment snapshot (#208). It answers a line to say (none when
    // empty), or why not, and then the library is left as it was.
    using BeforeUpgrade = std::function<std::expected<QString, Failure>(Library &library)>;

    [[nodiscard]] static int currentFormat() noexcept;
    // Sets what this program does before every upgrade of a library it opens
    // (a computer's moment snapshot, set by main(); a hub's, while its keeper
    // opens its libraries), and answers what was set before.
    static BeforeUpgrade beforeUpgrading(BeforeUpgrade before);
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
    [[nodiscard]] static std::expected<void, QString> migrate(Library &library);
    // What the program does before an upgrade (beforeUpgrading), its line said.
    [[nodiscard]] static std::expected<void, QString> beforeUpgrade(Library &library);
    static void sayBringingUpToDate(const Library &library, int schema);
};
