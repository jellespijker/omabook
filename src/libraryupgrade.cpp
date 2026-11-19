#include "libraryupgrade.h"

#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QtConcurrent>
#include <tuple>

#include "library.h"
#include "librarymigrations.h"

LibraryUpgrade::LibraryUpgrade(QObject *parent) : QObject(parent) {
    connect(&m_work, &QFutureWatcher<void>::finished, this, [this] {
        m_running = false;
        emit runningChanged();
        emit finished();
    });
}

LibraryUpgrade::~LibraryUpgrade() { m_work.waitForFinished(); }

bool LibraryUpgrade::due(const QString &root) {
    const QString file = Library(root).databasePath();
    if (!QFileInfo::exists(file))
        return false;
    // Only asked, on a connection of its own that can't write: the format is
    // the database's user_version, as Library::formatVersion() reads it.
    const QString connection = "upgrade-" + QUuid::createUuid().toString(QUuid::Id128);
    int format = 0;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(file);
        database.setConnectOptions("QSQLITE_OPEN_READONLY");
        if (database.open()) {
            QSqlQuery version(database);
            if (version.exec("PRAGMA user_version") && version.next())
                format = version.value(0).toInt();
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return format > 0 && format < LibraryMigrations::currentFormat();
}

bool LibraryUpgrade::start(const QString &root) {
    if (m_running || !due(root))
        return false;
    m_running = true;
    emit runningChanged();
    m_work.setFuture(QtConcurrent::run([root] {
        Library library(root);
        // Opening it is what brings it up to date; what goes wrong is said
        // when the window opens it in its turn.
        std::ignore = library.open();
    }));
    return true;
}
