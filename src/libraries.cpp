#include "libraries.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "library.h"
#include "trash.h"

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

namespace {

// Why the library in `path` can't be taken off the list; empty when it can.
[[nodiscard]] QString whyKept(const Settings &settings, const QString &path) {
    const QStringList known = settings.libraries();
    if (!known.contains(path))
        return path + " is not one of your libraries.";
    if (path == settings.libraryPath() && known.size() < 2)
        return u"This is the only library, so it can't be forgotten. Open another one first."_s;
    return {};
}

} // namespace

bool Libraries::forget(Settings &settings, const QString &folder, QString *error) {
    const QString path = QDir(folder).absolutePath();
    if (const QString why = whyKept(settings, path); !why.isEmpty())
        return fail(error, why);
    if (path == settings.libraryPath()) {
        QStringList others = settings.libraries();
        others.removeAll(path);
        settings.setLibraryPath(others.first());
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
    if (const QString why = whyKept(settings, path); !why.isEmpty())
        return fail(error, why);
    // To the trash before it leaves the list: where it can't go there, it stays as it was.
    if (QFileInfo::exists(path) && !QDir().rmdir(path) && !Trash::take(path))
        return fail(error, Failure(Failure::Code::LibraryNotTrashed, {path}).text());
    return forget(settings, path, error);
}
