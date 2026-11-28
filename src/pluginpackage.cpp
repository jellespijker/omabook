#include "pluginpackage.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include <array>

#include "scriptplugin.h"

namespace PluginPackage {

namespace {

// How much of an archive is read at a time.
constexpr std::size_t blockBytes = std::size_t{64} * 1024;

struct Closer {
    void operator()(archive *a) const noexcept { archive_read_free(a); }
};

QString entryName(archive_entry *entry) {
    if (const char *utf8 = archive_entry_pathname_utf8(entry))
        return QString::fromUtf8(utf8);
    if (const wchar_t *wide = archive_entry_pathname_w(entry))
        return QString::fromWCharArray(wide);
    return QString::fromLocal8Bit(archive_entry_pathname(entry));
}

// Writes the archive's current entry, a plain file, to `target`, counting
// its bytes as they come out.
[[nodiscard]] Unpacked writeEntry(archive *reader, archive_entry *entry, const QString &name, const QString &target, Tally &tally) {
    QFile file(target);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !file.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return std::unexpected("\"" + name + "\" can't be unpacked.");
    std::array<char, blockBytes> buffer{};
    qint64 size = 0;
    for (;;) {
        const la_ssize_t count = archive_read_data(reader, buffer.data(), buffer.size());
        if (count == 0)
            break;
        if (count < 0)
            return std::unexpected("\"" + name + "\" can't be read from the archive.");
        if (const auto room = tally.fits(name, size, count); !room)
            return room;
        if (file.write(buffer.data(), count) != count)
            return std::unexpected("\"" + name + "\" can't be unpacked.");
        size += count;
    }
    file.close();
    // A program keeps its right to be run, and nothing more than that.
    const bool runs = (archive_entry_perm(entry) & 0100U) != 0;
    file.setPermissions(runs ? QFile::Permissions(0x7755) : QFile::Permissions(0x6644));
    return {};
}

// Unpacks an archive into `into`, in one pass, refusing the whole of it for
// one entry that isn't a plain file inside the folder. The sizes an archive
// claims are not believed: the bytes are counted as they come out.
[[nodiscard]] Unpacked unpack(const QString &path, const QString &into, Tally &tally) {
    const std::unique_ptr<archive, Closer> reader(archive_read_new());
    archive_read_support_filter_all(reader.get());
    archive_read_support_format_all(reader.get());
    if (archive_read_open_filename(reader.get(), QFile::encodeName(path).constData(), blockBytes) != ARCHIVE_OK)
        return std::unexpected(path + " isn't an archive that can be read.");
    archive_entry *entry = nullptr;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        const QString name = entryName(entry);
        const auto type = archive_entry_filetype(entry);
        const auto clean = landsInside(name);
        if (!clean)
            return std::unexpected("\"" + name + "\" would land outside the plugin's folder, so nothing of this archive was installed.");
        if (archive_entry_hardlink(entry) || (type != AE_IFREG && type != AE_IFDIR))
            return std::unexpected("\"" + name
                                   + "\" is a link or a special file, which a plugin has no use for, so nothing of this "
                                     "archive was installed.");
        if (type == AE_IFDIR || clean->isEmpty())
            continue;
        if (const auto counted = tally.another(); !counted)
            return counted;
        if (const auto written = writeEntry(reader.get(), entry, name, into + "/" + *clean, tally); !written)
            return written;
    }
    if (status != ARCHIVE_EOF)
        return std::unexpected(path + " isn't an archive that can be read.");
    return {};
}

} // namespace

// A name inside a package as a path under the plugin's folder, or nothing for
// one that would land anywhere else: an absolute path, a step up, a
// backslash (which another system reads as a separator).
[[nodiscard]] std::optional<QString> landsInside(const QString &name) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(QChar(0)))
        return std::nullopt;
    QStringList parts;
    for (const QString &part : name.split('/', Qt::SkipEmptyParts)) {
        if (part == "..")
            return std::nullopt;
        if (part != ".")
            parts << part;
    }
    return parts.join('/');
}

bool writtenAsPath(const QString &name) {
    const bool control = std::ranges::any_of(name, [](QChar c) { return c.category() == QChar::Other_Control; });
    const QStringList parts = name.split('/');
    return control || name.contains('\\') || name.startsWith('/') || name.startsWith("~/") || parts.contains("..") || parts.contains(".");
}

std::optional<Failure> whyRefused(const QString &root, const QString &title, bool update) {
    if (writtenAsPath(title))
        return Failure(Failure::Code::PluginNameAPath, {title});
    if (!update)
        if (const auto why = ScriptManifest::read(root).whyNotLoaded())
            return Failure(Failure::Code::PluginNotInstalled, {*why});
    return std::nullopt;
}

// Copies a plugin's folder into `into`, by the same rules. What a
// repository keeps of itself stays behind.
[[nodiscard]] Unpacked copyFolder(const QString &from, const QString &into, Tally &tally) {
    const QDir source(from);
    QDirIterator walk(from, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (walk.hasNext()) {
        const QFileInfo entry(walk.next());
        const QString name = source.relativeFilePath(entry.filePath());
        if (name == ".git" || name.startsWith(".git/"))
            continue;
        if (entry.isSymLink() || (!entry.isFile() && !entry.isDir()))
            return std::unexpected("\"" + name + "\" is a link or a special file, which a plugin has no use for.");
        if (entry.isDir()) {
            QDir().mkpath(into + "/" + name);
            continue;
        }
        if (const auto room = tally.another().and_then([&] { return tally.fits(name, 0, entry.size()); }); !room)
            return room;
        const QString target = into + "/" + name;
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::copy(entry.filePath(), target))
            return std::unexpected("\"" + name + "\" can't be copied.");
        QFile::setPermissions(target, entry.isExecutable() ? QFile::Permissions(0x7755) : QFile::Permissions(0x6644));
    }
    return {};
}

Staged::Staged(const QString &source, const QString &under, const PluginTools::Limits &limits, const QString &subfolder) {
    tally.limits = limits;
    const QFileInfo given(source);
    if (source.isEmpty() || !given.exists()) {
        error = "There is no " + (source.isEmpty() ? QString("plugin named") : source) + ".";
        return;
    }
    QDir().mkpath(under);
    dir = std::make_unique<QTemporaryDir>(under + "/.installing-XXXXXX");
    if (!dir->isValid()) {
        error = "The plugin can't be unpacked in " + under + ".";
        return;
    }
    const auto unpacked =
        given.isDir() ? copyFolder(given.absoluteFilePath(), dir->path(), tally) : unpack(given.absoluteFilePath(), dir->path(), tally);
    if (!unpacked) {
        error = unpacked.error();
        return;
    }
    locate(given, source, subfolder);
}

void Staged::locate(const QFileInfo &given, const QString &source, const QString &subfolder) {
    // Archives are often one folder with the plugin inside it.
    const QDir top(dir->path());
    const QStringList folders = top.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    const QStringList files = top.entryList(QDir::Files | QDir::Hidden);
    if (!subfolder.isEmpty()) {
        // The plugin is a folder of the package: that folder, under the
        // one folder an archive of a repository is wrapped in.
        const QString base = folders.size() == 1 && files.isEmpty() ? top.filePath(folders.first()) : dir->path();
        const auto clean = landsInside(subfolder);
        if (!clean || !QFileInfo(base + "/" + *clean + "/plugin.json").isFile()) {
            error = "There is no plugin.json in \"" + subfolder + "\" of " + source + ", so that folder isn't a plugin.";
            return;
        }
        root = base + "/" + *clean;
        suggested = clean->section('/', -1);
    } else if (files.contains("plugin.json")) {
        root = dir->path();
        suggested = given.isDir() ? given.fileName() : given.completeBaseName();
    } else if (folders.size() == 1 && files.isEmpty() && QFileInfo(top.filePath(folders.first() + "/plugin.json")).isFile()) {
        root = top.filePath(folders.first());
        suggested = folders.first();
    } else {
        error = "There is no plugin.json in " + source + ", so it isn't a plugin.";
    }
}

} // namespace PluginPackage
