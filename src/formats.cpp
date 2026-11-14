#include "formats.h"

#include <QBuffer>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>

#include <QReadWriteLock>

#include "formatprovider.h"
#include "formatreaders.h"

namespace {

using Opener = std::unique_ptr<Document> (*)(const QString &, QString *);

struct Format {
    const char *extension;
    const char *name;
    Opener open; // null for a format that is read after converting it
};

// In the order the reader prefers when a book has several: what Omabook
// reads itself before what it converts, flowing text before fixed pages.
const QList<Format> &formats() {
    static const QList<Format> formats{
        {"epub", "EPUB", openEpub},
        {"fb2", "FictionBook", openFb2},
        {"fb2.zip", "FictionBook", openFb2},
        {"fbz", "FictionBook", openFb2},
        {"md", "Markdown", openText},
        {"markdown", "Markdown", openText},
        {"html", "HTML", openText},
        {"htm", "HTML", openText},
        {"xhtml", "HTML", openText},
        {"txt", "Plain text", openText},
        {"pdf", "PDF", openPdf},
        {"cbz", "Comic (CBZ)", openComic},
        {"cbr", "Comic (CBR)", openComic},
        {"cb7", "Comic (CB7)", openComic},
        {"cbt", "Comic (CBT)", openComic},
        {"azw3", "Kindle (AZW3)", nullptr},
        {"azw", "Kindle (AZW)", nullptr},
        {"mobi", "Kindle (MOBI)", nullptr},
        {"prc", "Kindle (PRC)", nullptr},
        {"kfx", "Kindle (KFX)", nullptr},
        {"docx", "Word", nullptr},
        {"odt", "OpenDocument", nullptr},
        {"rtf", "Rich text", nullptr},
        {"lit", "Microsoft Reader", nullptr},
        {"pdb", "Palm", nullptr},
        {"lrf", "Sony (LRF)", nullptr},
        {"djvu", "DjVu", nullptr},
        {"chm", "Help (CHM)", nullptr},
    };
    return formats;
}

const Format *find(const QString &extension) {
    for (const Format &format : formats())
        if (extension == QLatin1String(format.extension))
            return &format;
    return nullptr;
}

// The formats added as providers: Omabook's own first, then plugins'.
struct Registry {
    QReadWriteLock lock;
    QList<FormatProvider> providers{markdownBookProvider()};
    std::function<void()> loader;
};
Registry &registry() {
    static Registry registry;
    return registry;
}

std::optional<FormatProvider> providerWith(const QString &extension) {
    for (const FormatProvider &provider : Formats::providers())
        if (provider.extensions.contains(extension))
            return provider;
    return std::nullopt;
}

} // namespace

void Formats::addProvider(const FormatProvider &provider) {
    removeProvider(provider.id);
    const QWriteLocker locker(&registry().lock);
    registry().providers.append(provider);
}

void Formats::removeProvider(const QString &id) {
    const QWriteLocker locker(&registry().lock);
    QList<FormatProvider> &providers = registry().providers;
    providers.erase(std::remove_if(providers.begin(), providers.end(), [&](const FormatProvider &provider) { return provider.id == id; }),
                    providers.end());
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
        loading = true;
        loader();
        loading = false;
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
    for (const FormatProvider &provider : providers())
        if (provider.claims && provider.claims(source))
            return provider;
    return std::nullopt;
}

QString Formats::extension(const QString &path) {
    const QString lower = QFileInfo(path).fileName().toLower();
    return lower.endsWith(".fb2.zip") ? "fb2.zip" : QFileInfo(lower).suffix();
}

QStringList Formats::extensions() {
    QStringList extensions;
    for (const Format &format : formats())
        extensions << format.extension;
    for (const FormatProvider &provider : providers())
        for (const QString &extension : provider.extensions)
            if (!extensions.contains(extension))
                extensions << extension;
    return extensions;
}

bool Formats::isBook(const QString &path) { return find(extension(path)) != nullptr || providerWith(extension(path)); }

bool Formats::readable(const QString &extension) {
    const Format *format = find(extension);
    return format ? format->open != nullptr : providerWith(extension).has_value();
}

QString Formats::name(const QString &extension) {
    const Format *format = find(extension);
    if (const auto provider = format ? std::nullopt : providerWith(extension))
        return provider->name;
    return format ? format->name : extension.toUpper();
}

QStringList Formats::byPreference(QStringList extensions) {
    const QStringList order = Formats::extensions();
    std::stable_sort(extensions.begin(), extensions.end(), [&](const QString &a, const QString &b) {
        const int ia = order.indexOf(a), ib = order.indexOf(b);
        return (ia < 0 ? order.size() : ia) < (ib < 0 ? order.size() : ib);
    });
    return extensions;
}

std::unique_ptr<Document> Formats::open(const QString &path, QString *error) {
    QString ignored;
    QString *why = error ? error : &ignored;
    if (!QFileInfo::exists(path)) {
        *why = "There is no such file: " + path;
        return nullptr;
    }
    // A format added as a provider: by the file's extension, or for a folder
    // or a plain archive by what is in it.
    if (const auto provider = providerFor(path)) {
        const BookSource source(path);
        const auto description = provider->describe ? provider->describe(source, why) : std::nullopt;
        if (!description || description->chapters.isEmpty()) {
            if (why->isEmpty())
                *why = provider->name + " found no chapters in " + QFileInfo(path).fileName() + ".";
            return nullptr;
        }
        return fromDescription(*description, source);
    }
    if (!QFileInfo(path).isFile()) {
        *why = "Omabook doesn't know what kind of book the folder " + QFileInfo(path).fileName() + " is.";
        return nullptr;
    }
    const Format *format = find(extension(path));
    if (!format) {
        *why = "Omabook doesn't know the format of " + QFileInfo(path).fileName() + ".";
        return nullptr;
    }
    if (!format->open) {
        *why = QString(format->name) + " files are read by converting them to EPUB first.";
        return nullptr;
    }
    return format->open(path, why);
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
    } else if (QStringList{"mobi", "azw", "azw3", "prc"}.contains(extension)) {
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
