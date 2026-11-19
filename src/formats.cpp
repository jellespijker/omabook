#include "formats.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>

#include <QReadWriteLock>
#include <QScopedValueRollback>
#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

#include "formatprovider.h"
#include "formatreaders.h"

using namespace Qt::StringLiterals;

namespace {

using Opener = OpenedDocument (*)(const QString &);

struct Format {
    QLatin1StringView extension;
    QLatin1StringView name;
    Opener open = nullptr; // null for a format that is read after converting it
};

// In the order the reader prefers when a book has several: what Omabook
// reads itself before what it converts, flowing text before fixed pages.
constexpr auto formats = std::to_array<Format>({
    {"epub"_L1, "EPUB"_L1, openEpub},
    {"fb2"_L1, "FictionBook"_L1, openFb2},
    {"fb2.zip"_L1, "FictionBook"_L1, openFb2},
    {"fbz"_L1, "FictionBook"_L1, openFb2},
    {"md"_L1, "Markdown"_L1, openText},
    {"markdown"_L1, "Markdown"_L1, openText},
    {"html"_L1, "HTML"_L1, openText},
    {"htm"_L1, "HTML"_L1, openText},
    {"xhtml"_L1, "HTML"_L1, openText},
    {"txt"_L1, "Plain text"_L1, openText},
    {"pdf"_L1, "PDF"_L1, openPdf},
    {"cbz"_L1, "Comic (CBZ)"_L1, openComic},
    {"cbr"_L1, "Comic (CBR)"_L1, openComic},
    {"cb7"_L1, "Comic (CB7)"_L1, openComic},
    {"cbt"_L1, "Comic (CBT)"_L1, openComic},
    {"azw3"_L1, "Kindle (AZW3)"_L1},
    {"azw"_L1, "Kindle (AZW)"_L1},
    {"mobi"_L1, "Kindle (MOBI)"_L1},
    {"prc"_L1, "Kindle (PRC)"_L1},
    {"kfx"_L1, "Kindle (KFX)"_L1},
    {"docx"_L1, "Word"_L1},
    {"odt"_L1, "OpenDocument"_L1},
    {"rtf"_L1, "Rich text"_L1},
    {"lit"_L1, "Microsoft Reader"_L1},
    {"pdb"_L1, "Palm"_L1},
    {"lrf"_L1, "Sony (LRF)"_L1},
    {"djvu"_L1, "DjVu"_L1},
    {"chm"_L1, "Help (CHM)"_L1},
});

// The Kindle formats, which say who they are without being converted.
constexpr std::array kindle{"mobi"_L1, "azw"_L1, "azw3"_L1, "prc"_L1};

// One of Omabook's own formats, or null for an extension that isn't one.
[[nodiscard]] const Format *find(const QString &extension) {
    const auto found = std::ranges::find_if(formats, [&extension](const Format &format) { return extension == format.extension; });
    return found != formats.end() ? &*found : nullptr;
}

// The formats added as providers: Omabook's own first, then plugins'.
struct Registry {
    QReadWriteLock lock;
    QList<FormatProvider> providers{markdownBookProvider()};
    std::function<void()> loader;
    QList<PluginFormat> pluginFormats;
};
Registry &registry() {
    static Registry registry;
    return registry;
}

[[nodiscard]] std::optional<FormatProvider> providerWith(const QString &extension) {
    const QList<FormatProvider> providers = Formats::providers();
    const auto found = std::ranges::find_if(providers, [&extension](const FormatProvider &provider) {
        return provider.extensions.contains(extension);
    });
    return found != providers.end() ? std::optional(*found) : std::nullopt;
}

[[nodiscard]] QString fileName(const QString &path) { return QFileInfo(path).fileName(); }

// Opens a book, or says why it can't be.
[[nodiscard]] OpenedDocument opened(const QString &path) {
    if (!QFileInfo::exists(path))
        return std::unexpected("There is no such file: " + path);
    // A format added as a provider: by the file's extension, or for a folder
    // or a plain archive by what is in it.
    if (const auto provider = Formats::providerFor(path)) {
        const BookSource source(path);
        QString why;
        const auto description = provider->describe ? provider->describe(source, &why) : std::nullopt;
        if (!description || description->chapters.isEmpty())
            return std::unexpected(why.isEmpty() ? provider->name + " found no chapters in " + fileName(path) + "." : why);
        return Formats::fromDescription(*description, source);
    }
    if (!QFileInfo(path).isFile())
        return std::unexpected("Omabook doesn't know what kind of book the folder " + fileName(path) + " is.");
    const Format *format = find(Formats::extension(path));
    if (const auto kept = format ? std::nullopt : Formats::pluginFormat(Formats::extension(path)))
        return std::unexpected(
            QString("%1 is a %2, which the %3 plugin reads, and it isn't installed here. Install it: omabook plugin add %4")
                .arg(fileName(path), kept->name, kept->title, kept->plugin));
    if (!format)
        return std::unexpected("Omabook doesn't know the format of " + fileName(path) + ".");
    if (!format->open)
        return std::unexpected(QString(format->name) + " files are read by converting them to EPUB first.");
    return format->open(path);
}

} // namespace

void Formats::addProvider(const FormatProvider &provider) {
    removeProvider(provider.id);
    const QWriteLocker locker(&registry().lock);
    registry().providers.append(provider);
}

void Formats::removeProvider(const QString &id) {
    const QWriteLocker locker(&registry().lock);
    registry().providers.removeIf([&id](const FormatProvider &provider) { return provider.id == id; });
}

void Formats::setPluginFormats(const QList<PluginFormat> &formats) {
    QList<PluginFormat> kept;
    for (const PluginFormat &format : formats)
        if (!find(format.extension) && !markdownBookProvider().extensions.contains(format.extension))
            kept << format;
    const QWriteLocker locker(&registry().lock);
    registry().pluginFormats = std::move(kept);
}

std::optional<PluginFormat> Formats::pluginFormat(const QString &extension) {
    const QReadLocker locker(&registry().lock);
    const QList<PluginFormat> &known = registry().pluginFormats;
    const auto found = std::ranges::find(known, extension, &PluginFormat::extension);
    return found != known.end() ? std::optional(*found) : std::nullopt;
}

std::optional<PluginFormat> Formats::neededPlugin(const QStringList &extensions) {
    if (std::ranges::any_of(extensions, Formats::readable))
        return std::nullopt;
    for (const QString &extension : extensions)
        if (auto format = pluginFormat(extension))
            return format;
    return std::nullopt;
}

void Formats::setProviderLoader(std::function<void()> loader) {
    const QWriteLocker locker(&registry().lock);
    registry().loader = std::move(loader);
}

QList<FormatProvider> Formats::providers() {
    // The loader adds and removes providers, which asks for them again:
    // once is enough, so it is not called from within itself.
    static thread_local bool loading = false;
    std::function<void()> loader;
    {
        const QReadLocker locker(&registry().lock);
        loader = registry().loader;
    }
    if (loader && !loading) {
        const QScopedValueRollback<bool> once(loading, true);
        loader();
    }
    const QReadLocker locker(&registry().lock);
    return registry().providers;
}

std::optional<FormatProvider> Formats::providerFor(const QString &path) {
    const QFileInfo info(path);
    if (info.isFile() && find(extension(path)))
        return std::nullopt; // one of Omabook's own formats
    if (const auto byName = info.isFile() ? providerWith(extension(path)) : std::nullopt)
        return byName;
    // A folder, or an archive that is called nothing in particular: whoever
    // recognizes what is in it.
    if (!info.exists())
        return std::nullopt;
    const BookSource source(path);
    const QList<FormatProvider> known = providers();
    const auto claimant = std::ranges::find_if(known, [&source](const FormatProvider &provider) {
        return provider.claims && provider.claims(source);
    });
    return claimant != known.end() ? std::optional(*claimant) : std::nullopt;
}

QString Formats::extension(const QString &path) {
    const QString lower = QFileInfo(path).fileName().toLower();
    return lower.endsWith(".fb2.zip"_L1) ? u"fb2.zip"_s : QFileInfo(lower).suffix();
}

QStringList Formats::extensions() {
    QStringList extensions = formats | std::views::transform(&Format::extension) | std::ranges::to<QStringList>();
    for (const FormatProvider &provider : providers())
        for (const QString &extension : provider.extensions)
            if (!extensions.contains(extension))
                extensions << extension;
    return extensions;
}

bool Formats::isBook(const QString &path) {
    return find(extension(path)) != nullptr || providerWith(extension(path)) || pluginFormat(extension(path)).has_value();
}

bool Formats::readable(const QString &extension) {
    const Format *format = find(extension);
    return format ? format->open != nullptr : providerWith(extension).has_value();
}

bool Formats::ownFormat(const QString &extension) {
    static const QStringList provided = markdownBookProvider().extensions;
    return find(extension) != nullptr || provided.contains(extension);
}

QString Formats::name(const QString &extension) {
    if (const Format *format = find(extension))
        return format->name;
    if (const auto provider = providerWith(extension))
        return provider->name;
    return pluginFormat(extension).transform([](const PluginFormat &format) { return format.name; }).value_or(extension.toUpper());
}

QString Formats::names(const QStringList &extensions) {
    QStringList names;
    for (const QString &extension : extensions)
        if (const QString named = name(extension); !names.contains(named))
            names << named;
    return names.join(", ");
}

QStringList Formats::byPreference(QStringList extensions) {
    const QStringList order = Formats::extensions();
    // An extension that isn't known comes after all that are.
    std::ranges::stable_sort(extensions, {}, [&order](const QString &extension) {
        const qsizetype place = order.indexOf(extension);
        return place < 0 ? order.size() : place;
    });
    return extensions;
}

std::unique_ptr<Document> Formats::open(const QString &path, QString *error) {
    auto document = opened(path);
    if (document)
        return std::move(*document);
    if (error)
        *error = document.error();
    return nullptr;
}

// Calibre's convention, which most collections follow: "Title - Author".
BookMeta Formats::metaFromFileName(const QString &path) {
    QString base = QFileInfo(path).fileName();
    base.chop(extension(path).size() + 1);
    base.replace('_', ' ');
    // What download sites append: "(z-lib.org)", "[retail]".
    static const QRegularExpression noise("\\s*[\\(\\[][^\\)\\]]*[\\)\\]]\\s*$");
    while (noise.match(base).hasMatch() && noise.match(base).capturedStart() > 0)
        base.remove(noise);
    BookMeta meta;
    const qsizetype dash = base.lastIndexOf(" - ");
    if (dash > 0) {
        meta.title = base.left(dash).simplified();
        meta.authors = BookMeta::splitAuthors(base.mid(dash + 3));
    } else {
        meta.title = base.simplified();
    }
    return meta;
}

BookMeta Formats::readMeta(const QString &path) {
    BookMeta meta;
    const QString extension = Formats::extension(path);
    if (readable(extension)) {
        if (const auto document = open(path))
            meta = document->meta();
    } else if (std::ranges::contains(kindle, extension)) {
        meta = readMobiMeta(path);
    }
    const BookMeta named = metaFromFileName(path);
    if (meta.title.isEmpty()) {
        meta.title = named.title;
        if (meta.authors.isEmpty())
            meta.authors = named.authors;
    }
    return meta;
}

QString bookImageAttributes(const QString &name, const QByteArray &data, int width) {
    QByteArray bytes = data;
    QBuffer buffer(&bytes);
    QSize size = QImageReader(&buffer).size();
    if (!size.isValid())
        return {};
    if (width > 0 && size.width() > width)
        size = size.scaled(width, 1 << 20, Qt::KeepAspectRatio);
    return QString("src=\"image://book/%1\" width=\"%2\" height=\"%3\"")
        .arg(QString::fromLatin1(name.toUtf8().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)))
        .arg(size.width())
        .arg(size.height());
}

QString bookImageName(const QString &encoded) {
    return QString::fromUtf8(QByteArray::fromBase64(encoded.toLatin1(), QByteArray::Base64UrlEncoding));
}
