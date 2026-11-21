#include "pluginfiles.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>
#include <tuple>

#include "library.h"
#include "plugindata.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto fileField = "file"_L1;

// The hashes of files read before, by path, with the size and time they had
// then: a sync reads again only what changed. Losing it costs reading them all
// once more, so it is a cache, not data.
class Hashes {
public:
    Hashes() : m_path(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/omabook/plugin-file-hashes.json"_s) {
        QFile file(m_path);
        if (file.open(QIODevice::ReadOnly))
            m_kept = QJsonDocument::fromJson(file.readAll()).object();
    }
    Hashes(const Hashes &) = delete;
    Hashes &operator=(const Hashes &) = delete;
    Hashes(Hashes &&) = delete;
    Hashes &operator=(Hashes &&) = delete;
    ~Hashes() {
        if (!m_changed || !QDir().mkpath(QFileInfo(m_path).absolutePath()))
            return;
        QSaveFile file(m_path);
        if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(m_kept).toJson(QJsonDocument::Compact)) > 0)
            std::ignore = file.commit(); // a cache that wasn't kept is read again next time
    }

    [[nodiscard]] QString of(const QFileInfo &file) {
        const QString path = file.absoluteFilePath();
        const QJsonArray kept = m_kept.value(path).toArray();
        const qint64 modified = file.lastModified().toMSecsSinceEpoch();
        if (kept.size() == 3 && kept.at(0).toInteger() == file.size() && kept.at(1).toInteger() == modified)
            return kept.at(2).toString();
        const QString hash = Library::hashFile(path);
        m_kept.insert(path, QJsonArray{file.size(), modified, hash});
        m_changed = true;
        return hash;
    }

private:
    QString m_path;
    QJsonObject m_kept;
    bool m_changed = false;
};

[[nodiscard]] PluginData::Thing thingOf(const QString &plugin, const QString &path) {
    return {.plugin = plugin, .entity = QString::fromLatin1(PluginFiles::entity), .id = path};
}

[[nodiscard]] bool isHash(const QString &hash) {
    return hash.size() == 64 && std::ranges::all_of(hash, [](QChar c) { return c.isDigit() || (c >= u'a' && c <= u'f'); });
}

[[nodiscard]] bool record(const Library &library, const QString &plugin, const QString &path, const QJsonObject &file) {
    return PluginData::set(library, thingOf(plugin, path), {{fileField, file}}).has_value();
}

} // namespace

bool PluginFiles::travels(const QString &path) {
    if (path.isEmpty() || path.size() > PluginData::maxIdLength || path.startsWith(u'/') || path.contains(u'\\')
        || path == "settings.json"_L1)
        return false;
    const QStringList names = path.split(u'/');
    return std::ranges::none_of(names, [](const QString &name) { return name.isEmpty() || name == "."_L1 || name == ".."_L1; });
}

QString PluginFiles::inside(const QString &dataFolder, const QString &path) {
    const QString root = QFileInfo(dataFolder).canonicalFilePath();
    if (root.isEmpty() || !travels(path))
        return {};
    // A link is followed before it is judged; what isn't there yet is judged
    // by the nearest folder that is.
    QString existing = QDir::cleanPath(root + u'/' + path);
    QString rest;
    while (!QFileInfo::exists(existing) && existing != root) {
        rest = u'/' + QFileInfo(existing).fileName() + rest;
        existing = QFileInfo(existing).absolutePath();
    }
    const QString real = QFileInfo(existing).canonicalFilePath() + rest;
    return real.startsWith(root + u'/') ? real : QString();
}

qint64 PluginFiles::used(const QString &dataFolder) {
    qint64 bytes = 0;
    for (QDirIterator it(dataFolder, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories); it.hasNext();)
        bytes += it.nextFileInfo().size();
    return bytes;
}

PluginFiles::Survey PluginFiles::survey(const QString &dataFolder) {
    Survey survey;
    const QDir root(dataFolder);
    QStringList paths;
    for (QDirIterator it(dataFolder, QDir::Files, QDirIterator::Subdirectories); it.hasNext();)
        if (const QString path = root.relativeFilePath(it.next()); travels(path) && !it.fileInfo().isSymLink())
            paths << path;
    paths.sort();
    Hashes hashes;
    qint64 total = 0;
    for (const QString &path : std::as_const(paths)) {
        const QFileInfo file(root.filePath(path));
        if (file.size() > maxFileBytes || total + file.size() > maxPluginBytes) {
            survey.held << path;
            continue;
        }
        if (const QString hash = hashes.of(file); !hash.isEmpty()) {
            total += file.size();
            survey.files.insert(path, {.hash = hash, .size = file.size(), .removed = false});
        }
    }
    return survey;
}

PluginFiles::Files PluginFiles::recorded(const Library &library, const QString &plugin) {
    Files files;
    for (const auto &thing : PluginData::list(library, plugin, QString::fromLatin1(entity))) {
        const QString path = thing["id"_L1].toString();
        const QJsonObject file = thing["fields"_L1][fileField].toObject();
        const QString hash = file.value("hash"_L1).toString();
        if (travels(path) && isHash(hash))
            files.insert(path, {.hash = hash, .size = file.value("size"_L1).toInteger(), .removed = file.value("removed"_L1).toBool()});
    }
    return files;
}

int PluginFiles::note(const Library &library, const QString &plugin, const Files &disk) {
    const Files known = recorded(library, plugin);
    int noted = 0;
    for (auto file = disk.cbegin(); file != disk.cend(); ++file) {
        const auto said = known.constFind(file.key());
        if (said != known.cend() && said->hash == file->hash)
            continue;
        if (record(library, plugin, file.key(), {{"hash", file->hash}, {"size", file->size}}))
            ++noted;
    }
    return noted;
}

bool PluginFiles::forget(const Library &library, const QString &plugin, const QString &path, const QString &hash) {
    return travels(path) && isHash(hash) && record(library, plugin, path, {{"hash", hash}, {"removed", true}});
}

int PluginFiles::settle(const Library &library, const QString &plugin, const QString &dataFolder) {
    const Files known = recorded(library, plugin);
    int removed = 0;
    for (auto file = known.cbegin(); file != known.cend(); ++file) {
        const QString path = file->removed ? inside(dataFolder, file.key()) : QString();
        if (!path.isEmpty() && QFileInfo(path).isFile() && Library::hashFile(path) == file->hash && QFile::remove(path))
            ++removed;
    }
    return removed;
}

bool PluginFiles::take(const Library &library, const QString &plugin, const QString &dataFolder, const QString &path, const QString &file) {
    const Files known = recorded(library, plugin);
    const auto said = known.constFind(path);
    const qint64 size = QFileInfo(file).size();
    if (said == known.cend() || said->removed || size > maxFileBytes || Library::hashFile(file) != said->hash)
        return false;
    if (!QDir().mkpath(dataFolder))
        return false;
    const QString target = inside(dataFolder, path);
    // What it replaces is no longer counted against the limit.
    const qint64 before = QFileInfo(target).isFile() ? QFileInfo(target).size() : 0;
    if (target.isEmpty() || QFileInfo(target).isDir() || used(dataFolder) - before + size > maxPluginBytes
        || !QDir().mkpath(QFileInfo(target).absolutePath()))
        return false;
    QFile source(file);
    QSaveFile saved(target);
    saved.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !saved.open(QIODevice::WriteOnly))
        return false;
    while (!source.atEnd())
        if (const QByteArray piece = source.read(qint64{1024} * 1024); saved.write(piece) != piece.size())
            return false;
    return saved.commit();
}
