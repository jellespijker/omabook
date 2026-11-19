#include "librarymigrations.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <chrono>
#include <ranges>

#include "changelog.h"
#include "library.h"
#include "libraryschema.h"
#include "log.h"

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
    if (schema < steps.size())
        qCInfo(lcLibrary).noquote() << "bringing" << library.m_root << "from format" << schema << "to" << steps.size();
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

std::expected<void, QString> LibraryMigrations::setUp(const Library &library) {
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
    if (!library.m_keepFormat)
        ChangeLog::open(library);
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
