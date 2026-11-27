#include "bookfolder.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <algorithm>

#include "safename.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto inside = QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot;

// Whether a new book may not have the folder at `path`: something is there that
// isn't an empty folder, or can't be looked into.
[[nodiscard]] bool taken(const QString &path) {
    const QFileInfo there(path);
    return there.isSymLink() || (there.exists() && (!there.isDir() || !there.isReadable() || !QDir(path).isEmpty(inside)));
}

[[nodiscard]] bool holdsFiles(const QString &folder) {
    return QDirIterator(folder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories).hasNext();
}

} // namespace

QString BookFolder::named(const BookMeta &meta, qint64 id) {
    return SafeName::component(meta.authors.value(0), u"Unknown"_s) + u'/' + SafeName::component(meta.title, u"Untitled"_s)
           + u" (%1)"_s.arg(id);
}

QString BookFolder::forNew(const QString &root, const BookMeta &meta, qint64 id) {
    const QString name = named(meta, id);
    QString folder = name;
    for (int next = 2; taken(root + u'/' + folder); ++next)
        folder = u"%1 %2"_s.arg(name).arg(next);
    return folder;
}

bool BookFolder::anyIn(const QString &root) {
    if (!QDir(root + u"/.backups"_s).entryList({u"omabook-format-*.db"_s}, QDir::Files).isEmpty())
        return true;
    // As named() and forNew() name them.
    static const QRegularExpression numbered(uR"( \(\d+\)( \d+)?$)"_s);
    const auto folders = [](const QString &in) { return QDir(in).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot); };
    return std::ranges::any_of(folders(root), [&folders](const QFileInfo &author) {
        return std::ranges::any_of(folders(author.filePath()), [](const QFileInfo &book) {
            return numbered.match(book.fileName()).hasMatch() && holdsFiles(book.filePath());
        });
    });
}
