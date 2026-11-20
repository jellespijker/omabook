#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <expected>
#include <memory>
#include <optional>

#include "document.h"
#include "failure.h"

// A format a plugin Omabook knows of reads (the plugins' registry says
// which), whether that plugin is installed here or not. A file in it is
// taken into a library all the same and kept, saying which plugin reads it,
// so a book a plugin made on another computer is not lost on its way here;
// once the plugin is installed it reads the book.
struct PluginFormat {
    QString extension; // lower case, without the dot
    QString name;      // what the format is called: "Storyteller story"
    QString plugin;    // the plugin's name, as `omabook plugin add` takes it: "storyteller"
    QString title;     // what the plugin is called: "Storyteller"
    QString address;   // where the plugin is installed from: "jellespijker/omabook-storyteller"
};

// The one place that knows which formats exist. A format Omabook reads itself
// has an opener; one it only recognizes (Kindle files, Word documents, ...)
// is read after converting it to EPUB, see converter.h.
namespace Formats {

// Lower-case extension of a file, with "fb2.zip" as one extension.
QString extension(const QString &path);
// Every extension Omabook takes into a library.
QStringList extensions();
// Whether Omabook takes the file into a library: a format it reads or
// converts, one a plugin here reads, or one it keeps for a plugin it knows of
// that isn't installed (PluginFormat).
bool isBook(const QString &path);
// Whether Omabook reads this format itself, without converting it.
bool readable(const QString &extension);
// Whether the format is one of Omabook's own, read or converted, which a
// plugin's format never takes over.
bool ownFormat(const QString &extension);
// The display name, such as "EPUB" or "Kindle (AZW3)".
QString name(const QString &extension);
// The display names of a book's formats, each once: "EPUB, PDF".
QString names(const QStringList &extensions);
// The formats the plugins Omabook knows of read. One of Omabook's own is
// left out: it stays Omabook's.
void setPluginFormats(const QList<PluginFormat> &formats);
// The plugin Omabook knows of that reads a format, installed here or not.
std::optional<PluginFormat> pluginFormat(const QString &extension);
// The plugin a book in these formats needs to be read here: when it has a
// file and none it has is read here, the plugin of the first that has one.
std::optional<PluginFormat> neededPlugin(const QStringList &extensions);

// Formats of one book in the order the reader prefers them.
QStringList byPreference(QStringList extensions);

// Opens a book for reading, or says why it can't be.
[[nodiscard]] std::expected<std::unique_ptr<Document>, Failure> opened(const QString &path);
// The same for a caller that shows the reason as it is: on failure the result
// is null and `error` says why, worded in the language of whoever shows it.
std::unique_ptr<Document> open(const QString &path, QString *error = nullptr);

// What the file says about itself, with the file name standing in for a
// missing title ("Author - Title.epub" gives both).
BookMeta readMeta(const QString &path);
// "Author - Title" or "Title", from a file name.
BookMeta metaFromFileName(const QString &path);

} // namespace Formats
