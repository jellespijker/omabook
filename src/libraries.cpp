#include "libraries.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "library.h"

using namespace Qt::StringLiterals;

namespace {

bool fail(QString *error, const QString &why) {
    if (error)
        *error = why;
    return false;
}

// How many books the Omabook library in a folder holds; -1 when there is none.
[[nodiscard]] qsizetype bookCount(const QString &folder) {
    if (!QFileInfo::exists(folder + "/omabook.db"))
        return -1;
    Library library(folder);
    return library.open() ? library.books().size() : -1;
}

} // namespace

bool Libraries::isCalibreOnly(const QString &folder) {
    return QFileInfo::exists(folder + "/metadata.db") && bookCount(folder) <= 0;
}

bool Libraries::forget(Settings &settings, const QString &folder, QString *error) {
    const QString path = QDir(folder).absolutePath();
    QStringList known = settings.libraries();
    if (!known.contains(path))
        return fail(error, path + " is not one of your libraries.");
    if (path == settings.libraryPath()) {
        known.removeAll(path);
        if (known.isEmpty())
            return fail(error, "This is the only library, so it can't be forgotten. Open another one first.");
        settings.setLibraryPath(known.first());
    }
    settings.forgetLibrary(path);
    // An empty database of Omabook's own among someone else's files is litter.
    if (bookCount(path) == 0 && QFileInfo::exists(path + "/metadata.db"))
        for (const auto file : {"/omabook.db"_L1, "/omabook.db-wal"_L1, "/omabook.db-shm"_L1})
            QFile::remove(path + file);
    return true;
}

bool Libraries::trash(Settings &settings, const QString &folder, QString *error) {
    const QString path = QDir(folder).absolutePath();
    if (QFileInfo::exists(path + "/metadata.db"))
        return fail(error, path + " is also a Calibre library, so Omabook won't throw it away. Forget it instead.");
    if (!forget(settings, path, error))
        return false;
    // To the trash, where a mistake can be undone; a filesystem with no trash
    // gets the folder removed, since the user asked for it to go.
    if (QFileInfo::exists(path) && !QFile::moveToTrash(path) && !QDir(path).removeRecursively())
        return fail(error, "The folder couldn't be removed: " + path);
    return true;
}
