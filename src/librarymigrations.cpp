#include "librarymigrations.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStorageInfo>
#include <QStringList>
#include <algorithm>
#include <array>
#include <chrono>
#include <print>
#include <ranges>
#include <tuple>

#include "booklinks.h"
#include "changelog.h"
#include "keptops.h"
#include "library.h"
#include "libraryschema.h"
#include "log.h"
#include "replication.h"
#include "statements.h"
#include "tenancy.h"

int LibraryMigrations::currentFormat() noexcept { return static_cast<int>(LibrarySchema::steps().size()); }

std::expected<void, QString> Library::backUp(int schema) const {
    const QString folder = backupFolder();
    const QString file =
        QString("%1/omabook-format-%2-%3.db").arg(folder).arg(schema).arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
    if (!QDir().mkpath(folder))
        return std::unexpected(folder + " can't be made.");
    // SQLite writes the copy itself, whole and consistent, also while the
    // write-ahead log holds changes the main file doesn't have yet. On a
    // connection of its own: this one is in the transaction that sets the
    // library up, where VACUUM can't run, and has written nothing yet, so the
    // other reads what this one would.
    QFile::remove(file);
    const QString connection = m_connection + "-copy";
    QString failed;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(databasePath());
        if (!database.open())
            failed = database.lastError().text();
        else if (QSqlQuery copy(database); !copy.exec(QString("VACUUM INTO '%1'").arg(QString(file).replace('\'', "''"))))
            failed = copy.lastError().text();
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (!failed.isEmpty())
        return std::unexpected(failed);
    // The last five are kept; older ones go.
    const QFileInfoList kept = QDir(folder).entryInfoList({"omabook-format-*.db"}, QDir::Files, QDir::Time);
    for (const QFileInfo &old : kept | std::views::drop(5))
        QFile::remove(old.filePath());
    return {};
}

std::expected<void, QString> LibraryMigrations::migrate(const Library &library) {
    // Asked and done with before anything changes: a statement left open
    // would keep a migration from dropping a table.
    const int schema = library.formatVersion();
    const QList<QStringList> &steps = LibrarySchema::steps();
    if (library.m_keepFormat && schema > 0 && schema != steps.size())
        return std::unexpected(QString("This library is in format %1, and is opened here only as it is (this Omabook's is %2).")
                                   .arg(schema)
                                   .arg(steps.size()));
    if (schema > steps.size())
        return std::unexpected(QString("This library was last opened by a newer Omabook (its format is %1; this Omabook knows up to %2). "
                                       "Update Omabook on this computer, or restore a copy from before that from %3.")
                                   .arg(schema)
                                   .arg(steps.size())
                                   .arg(library.backupFolder()));
    // A library about to be changed to a newer format is copied first, so a
    // step that goes wrong, or a wish to go back to an older Omabook, never
    // costs the library. One that can't be copied is left as it is.
    if (schema > 0 && schema < steps.size()) {
        if (const auto kept = library.backUp(schema); !kept)
            return std::unexpected("The library needs bringing up to date, but a copy of it couldn't be kept first, so it was left alone: "
                                   + kept.error());
    }
    if (schema < steps.size()) {
        qCInfo(lcLibrary).noquote() << "bringing" << library.m_root << "from format" << schema << "to" << steps.size();
        sayBringingUpToDate(library, schema);
    }
    // Each step whole or not at all, inside the transaction of setUp.
    for (int step = schema; step < steps.size(); ++step) {
        library.exec("SAVEPOINT format_step");
        for (const QString &statement : steps.at(step) + QStringList{QString("PRAGMA user_version = %1").arg(step + 1)}) {
            if (const QSqlQuery run = library.query(statement); run.lastError().isValid()) {
                library.exec("ROLLBACK TO format_step");
                library.exec("RELEASE format_step");
                return std::unexpected("The library can't be set up: " + run.lastError().text());
            }
        }
        library.exec("RELEASE format_step");
    }
    // A library that took the collections' step in its first form, before
    // collections nested, is given what nesting needs.
    QSqlQuery columns = library.query("SELECT 1 FROM pragma_table_info('collections') WHERE name = 'parent'");
    const bool nested = columns.next() || library.formatVersion() < steps.size();
    columns.finish();
    if (!nested) {
        library.exec("ALTER TABLE collections ADD COLUMN parent INTEGER NOT NULL DEFAULT 0");
        library.exec("DROP INDEX IF EXISTS collections_named");
        library.exec("CREATE UNIQUE INDEX collections_named ON collections(parent, name COLLATE NOCASE)");
    }
    return {};
}

namespace {

// The databases this thread is setting up, each by its file. Another
// connection to one of them, from this thread, would wait for the lock this
// thread holds, which it gives back only once that connection is done
// waiting: a plugin that opens the library as it loads, say, loaded while
// it is set up (#60).
[[nodiscard]] QStringList &settingUp() {
    thread_local QStringList files;
    return files;
}

// The same for every name of the folder: it is there by the time a library is set up.
[[nodiscard]] QString databaseFile(const QString &root) { return QFileInfo(root).canonicalFilePath() + "/omabook.db"; }

} // namespace

void LibraryMigrations::configure(const Library &library) {
    // The write-ahead log needs memory shared between processes, which a
    // folder on another machine (sshfs, NFS, SMB) can't give; there the
    // older rollback journal is the one that works.
    static constexpr std::array<QByteArrayView, 9> remote{"fuse.sshfs", "sshfs", "nfs", "nfs4",       "cifs",
                                                          "smb3",       "smbfs", "9p",  "fuse.rclone"};
    const QByteArray fileSystem = QStorageInfo(library.m_root).fileSystemType();
    const bool local = !std::ranges::contains(remote, QByteArrayView(fileSystem));
    library.exec(local ? "PRAGMA journal_mode = WAL" : "PRAGMA journal_mode = DELETE");
    // With the write-ahead log a commit that waits for the disk only at a
    // checkpoint is as safe from a crash of the program (a loss of power can
    // take the last commits back, never break the file) and half the waits.
    if (local)
        library.exec("PRAGMA synchronous = NORMAL");
    library.exec("PRAGMA foreign_keys = ON");
}

void LibraryMigrations::closing(const Library &library) {
    // Only of a library large enough to need them: what a small one's tables
    // say (one row, say) would lead the planner astray as it grows, while
    // the log runs a statement for each of its rows (#104). A library brought
    // up to date had none; one that grows 25 times over is looked at again.
    static constexpr int worthIt = 10000;
    if (QSqlDatabase::database(library.m_connection, false).isOpen()
        && library.m_statements->first("SELECT 1 FROM global_ids LIMIT 1 OFFSET ?", {worthIt - 1})) {
        // Never waited for: a library another program is writing is left to
        // that one, which does the same as it closes, so whether this could
        // is of no matter.
        std::ignore = library.m_statements->run("PRAGMA busy_timeout = 0");
        std::ignore = library.m_statements->run("PRAGMA analysis_limit = 1000");
        std::ignore = library.m_statements->run("PRAGMA optimize");
    }
    library.m_statements->clear();
}

std::expected<void, QString> LibraryMigrations::setUp(Library &library) {
    // Opened again on the thread that is setting it up, it is not waited for:
    // that wait could only end when it runs out.
    const QString file = databaseFile(library.m_root);
    if (settingUp().contains(file))
        return std::unexpected("The library is still being set up here; it can be opened once that is done.");
    settingUp() << file;
    const auto done = qScopeGuard([&file] { settingUp().removeOne(file); });
    configure(library);
    // Two programs opening a library at once (the window and a command), the
    // first time or the first after an update, would otherwise both see it
    // unready and both take the same steps, copy and baseline (#11). So it is
    // set up holding the database alone, and the other, waiting the longer
    // a migration takes, then finds it ready.
    QSqlQuery wait = library.query("PRAGMA busy_timeout");
    const int usualWait = wait.next() ? wait.value(0).toInt() : 0;
    wait.finish();
    static constexpr std::chrono::milliseconds setUpWait = std::chrono::minutes(2);
    library.exec(QString("PRAGMA busy_timeout = %1").arg(setUpWait.count()));
    const Library::Batch setUp(library, Library::Batch::Lock::AtOnce);
    library.exec(QString("PRAGMA busy_timeout = %1").arg(usualWait));
    // When it can't be had (a library that can only be read, or another
    // program at it still after the wait), one that is ready opens as it is,
    // and one that isn't is not changed.
    if (!setUp.begun() && library.formatVersion() != currentFormat())
        return std::unexpected("The library is being set up by another program; try again when it is done.");
    if (auto migrated = migrate(library); !migrated)
        return migrated;
    if (!library.m_keepFormat) {
        ChangeLog::open(library);
        // What was kept until this Omabook knew its kind (#118), taken in
        // now that it does: after the copy an upgrade keeps, in the same
        // transaction as the upgrade.
        KeptOps::takeKnown(library);
        judgeLinks(library);
        // The books' tenants as the collections and the user's choice say
        // now: the first time after the step that adds tenants, the whole
        // graph's (#94); after that, what changed while it was closed.
        Tenancy::follow(library);
    }
    return {};
}

Library::Changes Library::changes() const {
    Changes changes;
    for (QSqlQuery query = this->query("SELECT key, value FROM library_state WHERE key IN ('books', 'graph', 'format')"); query.next();) {
        const QString key = query.value(0).toString();
        if (key == "books")
            changes.books = query.value(1).toLongLong();
        else if (key == "graph")
            changes.graph = query.value(1).toLongLong();
        else
            changes.format = query.value(1).toLongLong();
    }
    return changes;
}

void LibraryMigrations::sayBringingUpToDate(const Library &library, int schema) {
    // A new library is only made; one from before is brought up to date, which takes a while when it is large.
    if (schema > 0 && library.m_bringingUpToDate)
        library.m_bringingUpToDate(QString("Bringing your library up to date (from format %1 to %2); this is done once, and can take "
                                           "a minute for a large library…")
                                       .arg(schema)
                                       .arg(currentFormat()));
}

void LibraryMigrations::judgeLinks(const Library &library) {
    QSqlQuery due = library.query("SELECT 1 FROM library_state WHERE key = 'judge_links'");
    const bool judge = due.next();
    due.finish();
    if (!judge)
        return;
    const QStringList apart = BookLinks::record(library, Replication::operations(library));
    library.exec("DELETE FROM library_state WHERE key = 'judge_links'");
    if (apart.isEmpty())
        return;
    qCInfo(lcLibrary).noquote() << apart.size() << "links an older Omabook made between books that are not one book were dropped";
    if (library.m_bringingUpToDate)
        library.m_bringingUpToDate(
            QString("An older Omabook took %1 for one book; a sync keeps them apart now: %2.")
                .arg(apart.size() == 1 ? QString("1 pair of books") : QString("%1 pairs of books").arg(apart.size()), apart.join("; ")));
}

void LibraryMigrations::sayOnStandardError(const QString &said) { std::println(stderr, "{}", said.toStdString()); }
