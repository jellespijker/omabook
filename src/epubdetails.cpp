#include "epubdetails.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>

#include "bookarchive.h"
#include "formatreaders.h"
#include "zipedit.h"

namespace {

using namespace Qt::StringLiterals;

// The file's bytes, read where they lie for as long as the file is open.
[[nodiscard]] std::expected<QByteArrayView, Failure> mapped(QFile &file) {
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(Failure(Failure::Code::FileUnreadable, {file.errorString()}));
    const uchar *bytes = file.size() > 0 ? file.map(0, file.size()) : nullptr;
    if (bytes == nullptr)
        return std::unexpected(Failure(Failure::Code::EpubNotAnArchive));
    return QByteArrayView(bytes, file.size());
}

// The package document's path in the archive, as its container names it.
[[nodiscard]] QString packagePath(const Archive &archive) {
    QXmlStreamReader container(archive.read(u"META-INF/container.xml"_s));
    while (!container.atEnd())
        if (container.readNext() == QXmlStreamReader::StartElement && container.name() == u"rootfile")
            return QDir::cleanPath(container.attributes().value(u"full-path"_s).toString());
    return {};
}

// A path the package names, relative to it, as a path in the archive.
[[nodiscard]] QString inArchive(const QString &package, const QString &href) {
    const QString decoded = QUrl::fromPercentEncoding(href.section(u'#', 0, 0).toUtf8());
    const QString folder = package.contains(u'/') ? package.section(u'/', 0, -2) + u'/' : QString();
    return QDir::cleanPath(folder + decoded);
}

[[nodiscard]] QByteArray png(const QByteArray &jpeg) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    return QImage::fromData(jpeg).save(&buffer, "PNG") ? bytes : QByteArray();
}

// The cover written in: in place of the one the package names, or beside it
// as a new file the package is told of (`newCover`, its href).
void putCover(const QByteArray &opf, const QString &package, const QStringList &entries, const QByteArray &cover, ZipEdit::Change &change,
              QString &newCover) {
    const std::optional<OpfDetails::CoverItem> item = OpfDetails::coverItem(opf);
    const QString inside = item ? inArchive(package, item->href) : QString();
    const QByteArray asPng = item && item->mediaType == u"image/png" ? png(cover) : QByteArray();
    if (item && entries.contains(inside) && (item->mediaType == u"image/jpeg" || !asPng.isEmpty())) {
        change.replaced.insert(inside, asPng.isEmpty() ? cover : asPng);
        return;
    }
    newCover = u"omabook-cover.jpg"_s;
    change.added.append({inArchive(package, newCover), cover});
}

[[nodiscard]] QStringList simplified(const QStringList &names) {
    QStringList each;
    for (const QString &name : names)
        each << name.simplified();
    each.removeDuplicates();
    return each;
}

} // namespace

std::expected<QByteArray, Failure> EpubDetails::written(const QString &path, const OpfDetails::Details &details) {
    QFile file(path);
    const auto zip = mapped(file);
    if (!zip)
        return std::unexpected(zip.error());
    const Archive archive(path);
    const QStringList entries = archive.entries();
    if (entries.contains(u"META-INF/rights.xml"_s) || archive.read(u"META-INF/encryption.xml"_s).contains("xmlenc#aes"))
        return std::unexpected(Failure(Failure::Code::EpubProtected));
    const QString package = packagePath(archive);
    const QByteArray opf = package.isEmpty() ? QByteArray() : archive.read(package);
    if (opf.isNull())
        return std::unexpected(Failure(Failure::Code::EpubWithoutPackage));

    ZipEdit::Change change{.replaced = {}, .added = {}, .epub = true};
    QString newCover;
    if (!details.cover.isEmpty())
        putCover(opf, package, entries, details.cover, change, newCover);
    const auto rewritten = OpfDetails::rewritten(opf, details, newCover);
    if (!rewritten)
        return std::unexpected(rewritten.error());
    change.replaced.insert(package, *rewritten);
    return ZipEdit::edited(*zip, change);
}

std::expected<void, Failure> EpubDetails::check(const QString &path, const OpfDetails::Details &details) {
    const OpenedDocument opened = openEpub(path);
    if (!opened)
        return std::unexpected(opened.error());
    QFile file(path);
    const auto zip = mapped(file);
    const auto files = zip ? ZipEdit::entries(*zip) : std::expected<QList<ZipEdit::Entry>, Failure>(std::unexpect, zip.error());
    const bool startsRight =
        files && !files->isEmpty() && files->first().name == u"mimetype" && files->first().method == 0 && files->first().localExtra == 0;
    const BookMeta said = (*opened)->meta();
    if (!startsRight || said.title != details.meta.title.simplified() || said.authors != simplified(details.meta.authors)
        || said.series != details.meta.series.simplified())
        return std::unexpected(Failure(Failure::Code::WrittenEpubUnlike));
    return {};
}
