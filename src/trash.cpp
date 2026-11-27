#include "trash.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

using namespace Qt::StringLiterals;

namespace {

// The trash in the home folder, made when there is none yet, as the trash's
// specification allows anyone to: Qt only moves something to a trash that
// is there already.
void makeHomeTrash() {
    const QString trash = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/Trash"_s;
    QDir().mkpath(QFileInfo(trash).absolutePath());
    for (const QString &folder : {trash, trash + u"/files"_s, trash + u"/info"_s})
        if (!QFileInfo::exists(folder))
            QDir().mkdir(folder, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

} // namespace

bool Trash::take(const QString &path, QString *where) {
    makeHomeTrash();
    return QFile::moveToTrash(path, where);
}

std::expected<void, Failure> Trash::folder(const QString &root, const QString &folder) {
    const QString path = QDir::cleanPath(folder);
    if (!path.startsWith(QDir::cleanPath(root) + u'/') || !QFileInfo::exists(path) || QDir().rmdir(path) || take(path))
        return {};
    return std::unexpected(Failure(Failure::Code::FolderNotTrashed, {path}));
}
