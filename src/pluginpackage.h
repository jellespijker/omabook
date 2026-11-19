#pragma once

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <expected>
#include <memory>
#include <optional>

#include "plugintools.h"

// A plugin's package (a folder, or an archive such as a .zip or a GitHub
// tarball) laid out where it can be looked at before it is installed: copied
// or unpacked, held to the limits, and refused whole for anything in it that
// would land outside the plugin's folder. What it is and where it goes are
// PluginTools's to say.
namespace PluginPackage {

// A name inside a package as a path under the plugin's folder, or nothing for
// one that would land anywhere else: an absolute path, a step up, a
// backslash (which another system reads as a separator).
[[nodiscard]] std::optional<QString> landsInside(const QString &name);

// Whether a step of unpacking went well, or why not.
using Unpacked = std::expected<void, QString>;

// What was unpacked so far, held to the limits.
struct Tally {
    PluginTools::Limits limits;
    int files = 0;
    qint64 bytes = 0;

    [[nodiscard]] Unpacked another() {
        if (++files <= limits.files)
            return {};
        return std::unexpected(QString("It holds more than %1 files, which is more than a plugin is allowed.").arg(limits.files));
    }
    // Whether `count` more bytes of one file, `soFar` of which were taken, fit.
    [[nodiscard]] Unpacked fits(const QString &name, qint64 soFar, qint64 count) {
        if (soFar + count > limits.fileBytes)
            return std::unexpected(QString("\"%1\" is larger than %2 MB, which is more than a plugin's file is allowed.")
                                       .arg(name)
                                       .arg(limits.fileBytes / (qint64{1024} * 1024)));
        if (bytes + count > limits.totalBytes)
            return std::unexpected(QString("It unpacks to more than %1 MB, which is more than a plugin is allowed.")
                                       .arg(limits.totalBytes / (qint64{1024} * 1024)));
        bytes += count;
        return {};
    }
};

// Copies a plugin's folder into `into`, by the same rules. What a
// repository keeps of itself stays behind.
[[nodiscard]] Unpacked copyFolder(const QString &from, const QString &into, Tally &tally);

// A package laid out in a folder of its own, where it can be looked at
// before any of it is where a running Omabook would find it.
struct Staged {
    std::unique_ptr<QTemporaryDir> dir;
    QString root;      // where its plugin.json is
    QString suggested; // the name the package gives itself
    Tally tally;
    QString error;

    Staged(const QString &source, const QString &under, const PluginTools::Limits &limits, const QString &subfolder = {});

private:
    // Finds where in what was laid out the plugin.json is, and the name it gives itself.
    void locate(const QFileInfo &given, const QString &source, const QString &subfolder);
};

} // namespace PluginPackage
