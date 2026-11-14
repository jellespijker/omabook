#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <algorithm>

#include "bookarchive.h"
#include "formatreaders.h"
#include "htmlclean.h"

namespace {

struct ManifestItem {
    QString path;
    QString type;
    QString properties;
};

// A reference inside a book, relative to the file that makes it, as the path
// of a file in the archive.
[[nodiscard]] QString resolve(const QString &base, const QString &reference) {
    const QString decoded = QUrl::fromPercentEncoding(reference.section('#', 0, 0).toUtf8());
    if (decoded.isEmpty())
        return base;
    const QString directory = base.contains('/') ? base.section('/', 0, -2) : QString();
    QString path = QDir::cleanPath(decoded.startsWith('/') || directory.isEmpty() ? decoded : directory + "/" + decoded);
    while (path.startsWith('/') || path.startsWith("../"))
        path.remove(0, path.startsWith('/') ? 1 : 3);
    return path;
}

[[nodiscard]] QString isbnFrom(const QString &identifier) {
    static const QRegularExpression urn("^urn:isbn:", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression isbn("^(97[89])?[0-9]{9}[0-9Xx]$");
    QString digits = identifier;
    digits.remove(urn).remove('-').remove(' ');
    return isbn.match(digits).hasMatch() ? digits.toUpper() : QString();
}

class EpubDocument : public Document {
public:
    explicit EpubDocument(const QString &path) : m_archive(path) {}

    std::expected<void, QString> load() {
        const QStringList names = m_archive.entries();
        m_files = QSet<QString>(names.begin(), names.end());
        if (m_files.isEmpty())
            return std::unexpected("Not an EPUB: the file is not a readable zip archive.");
        // Font obfuscation also lists files as encrypted; rights.xml and
        // AES-encrypted content are what a locked book has.
        if (m_files.contains("META-INF/rights.xml")
            || m_archive.read("META-INF/encryption.xml").contains("xmlenc#aes"))
            return std::unexpected("This book is protected by DRM, so only the app it was bought for can open it.");

        QXmlStreamReader container(m_archive.read("META-INF/container.xml"));
        while (!container.atEnd() && m_opf.isEmpty())
            if (container.readNext() == QXmlStreamReader::StartElement && container.name() == u"rootfile")
                m_opf = resolve({}, container.attributes().value("full-path").toString());
        if (m_opf.isEmpty() || !m_files.contains(m_opf))
            return std::unexpected("Not an EPUB: it has no package document.");
        readPackage();
        if (m_spine.isEmpty())
            return std::unexpected("This EPUB lists no chapters to read.");
        readToc();
        for (const ManifestItem &item : std::as_const(m_manifest))
            if (item.type == "text/css")
                for (auto [name, flags] : classStyles(decodeText(m_archive.read(item.path))).asKeyValueRange())
                    m_styles[name] |= flags;
        return {};
    }

    Kind kind() const override { return Flow; }
    int count() const override { return m_spine.size(); }
    BookMeta meta() const override { return m_meta; }
    QList<TocEntry> toc() const override { return m_toc; }
    QByteArray resource(const QString &name) const override { return m_archive.read(name); }

    QString html(int section, int width) const override {
        if (section < 0 || section >= m_spine.size())
            return {};
        const QString file = m_spine.at(section);
        CleanOptions options;
        options.styles = m_styles;
        options.image = [this, &file, width](const QString &source) {
            const QString name = resolve(file, source);
            return m_files.contains(name) ? bookImageAttributes(name, m_archive.read(name), width) : QString();
        };
        options.link = [this, &file, section](const QString &target) { return linkFor(file, section, target); };
        return cleanHtml(decodeText(m_archive.read(file)), options);
    }

private:
    [[nodiscard]] QString linkFor(const QString &file, int section, const QString &target) const {
        static const QRegularExpression external("^(https?|mailto):", QRegularExpression::CaseInsensitiveOption);
        if (external.match(target).hasMatch())
            return target;
        const QString fragment = target.contains('#') ? target.section('#', 1) : QString();
        const int to = target.startsWith('#') ? section : m_sectionOf.value(resolve(file, target), -1);
        return to < 0 ? QString() : QString("omabook:%1#%2").arg(to).arg(fragment);
    }

    void readPackage() {
        QXmlStreamReader xml(m_archive.read(m_opf));
        QString coverId, tocId;
        QStringList spineIds;
        bool inMetadata = false;
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::EndElement && xml.name() == u"metadata")
                inMetadata = false;
            if (!xml.isStartElement())
                continue;
            const auto name = xml.name();
            const auto attributes = xml.attributes();
            if (name == u"metadata") {
                inMetadata = true;
            } else if (name == u"item") {
                m_manifest.insert(attributes.value("id").toString(),
                                  {resolve(m_opf, attributes.value("href").toString()),
                                   attributes.value("media-type").toString().toLower(),
                                   attributes.value("properties").toString()});
            } else if (name == u"spine") {
                tocId = attributes.value("toc").toString();
            } else if (name == u"itemref") {
                spineIds << attributes.value("idref").toString();
            } else if (inMetadata && name == u"meta") {
                const QString key = attributes.hasAttribute("name") ? attributes.value("name").toString()
                                                                    : attributes.value("property").toString();
                const QString value = attributes.hasAttribute("content")
                                          ? attributes.value("content").toString()
                                          : xml.readElementText(QXmlStreamReader::SkipChildElements);
                if (key == "cover") coverId = value;
                else if (key == "calibre:series" || key == "belongs-to-collection") m_meta.series = value.simplified();
                else if (key == "calibre:series_index" || key == "group-position") m_meta.seriesIndex = value.toDouble();
            } else if (inMetadata) {
                const QString field = name.toString();
                const QString scheme = attributes.value("scheme").toString();
                const QString value = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
                if (value.isEmpty()) continue;
                if (field == "title" && m_meta.title.isEmpty()) m_meta.title = value.simplified();
                else if (field == "creator") m_meta.authors << value.simplified();
                else if (field == "language" && m_meta.language.isEmpty()) m_meta.language = value;
                else if (field == "publisher") m_meta.publisher = value.simplified();
                else if (field == "date" && m_meta.published.isEmpty()) m_meta.published = value.left(10);
                else if (field == "subject") m_meta.tags << value.simplified();
                else if (field == "description") m_meta.description = value.contains('<') ? htmlToText(value) : value;
                else if (field == "identifier" && m_meta.isbn.isEmpty()
                         && (scheme.compare("isbn", Qt::CaseInsensitive) == 0 || value.startsWith("urn:isbn:", Qt::CaseInsensitive)
                             || scheme.isEmpty()))
                    m_meta.isbn = isbnFrom(value);
            }
        }
        m_meta.authors.removeDuplicates();
        m_meta.tags.removeDuplicates();

        for (const QString &id : std::as_const(spineIds)) {
            const QString path = m_manifest.value(id).path;
            if (!path.isEmpty() && m_files.contains(path) && !m_sectionOf.contains(path)) {
                m_sectionOf.insert(path, m_spine.size());
                m_spine << path;
            }
        }

        QString cover = m_manifest.value(coverId).path;
        for (const ManifestItem &item : std::as_const(m_manifest))
            if (cover.isEmpty() && item.properties.split(' ').contains("cover-image"))
                cover = item.path;
        for (auto it = m_manifest.constBegin(); it != m_manifest.constEnd() && cover.isEmpty(); ++it)
            if (it->type.startsWith("image/") && (it.key().contains("cover", Qt::CaseInsensitive)
                                                  || it->path.contains("cover", Qt::CaseInsensitive)))
                cover = it->path;
        if (!cover.isEmpty())
            m_meta.cover = QImage::fromData(m_archive.read(cover));

        for (const ManifestItem &item : std::as_const(m_manifest))
            if (item.properties.split(' ').contains("nav"))
                m_nav = item.path;
        m_ncx = m_manifest.value(tocId).path;
        for (const ManifestItem &item : std::as_const(m_manifest))
            if (m_ncx.isEmpty() && item.type == "application/x-dtbncx+xml")
                m_ncx = item.path;
    }

    void addTocEntry(const QString &file, const QString &target, const QString &title, int level) {
        const int section = m_sectionOf.value(resolve(file, target), -1);
        if (section >= 0 && !title.simplified().isEmpty())
            m_toc.append({.title = title.simplified(),
                          .section = section,
                          .anchor = target.contains('#') ? target.section('#', 1) : QString(),
                          .level = level});
    }

    void readToc() {
        // EPUB 3's navigation document: nested lists of links.
        if (!m_nav.isEmpty()) {
            using enum HtmlToken::Type;
            const QList<HtmlToken> tokens = tokenizeHtml(decodeText(m_archive.read(m_nav)));
            bool inToc = false, sawToc = false;
            int depth = 0;
            QString target, title;
            bool inLink = false;
            for (const HtmlToken &token : tokens) {
                if (token.type == Open && token.name == "nav") {
                    inToc = !sawToc && (token.attributes.value("type") == "toc" || !token.attributes.contains("type"));
                    depth = 0;
                } else if (token.type == Close && token.name == "nav") {
                    sawToc = sawToc || inToc;
                    inToc = false;
                } else if (!inToc) {
                    continue;
                } else if (token.name == "ol" || token.name == "ul") {
                    depth += token.type == Open ? 1 : -1;
                } else if (token.name == "a" && token.type == Open) {
                    inLink = true;
                    target = token.attributes.value("href");
                    title.clear();
                } else if (token.name == "a" && token.type == Close && inLink) {
                    inLink = false;
                    addTocEntry(m_nav, target, htmlToText(title), std::max(0, depth - 1));
                } else if (inLink && token.type == Text) {
                    title += token.text;
                }
            }
        }
        // EPUB 2's NCX: nested navPoints.
        if (m_toc.isEmpty() && !m_ncx.isEmpty()) {
            QXmlStreamReader xml(m_archive.read(m_ncx));
            int depth = 0;
            QString title;
            while (!xml.atEnd()) {
                xml.readNext();
                if (xml.isStartElement() && xml.name() == u"navPoint") ++depth;
                else if (xml.isEndElement() && xml.name() == u"navPoint") --depth;
                else if (depth > 0 && xml.isStartElement() && xml.name() == u"text") title = xml.readElementText();
                else if (depth > 0 && xml.isStartElement() && xml.name() == u"content")
                    addTocEntry(m_ncx, xml.attributes().value("src").toString(), title, depth - 1);
            }
        }
    }

    Archive m_archive;
    QSet<QString> m_files;
    QString m_opf, m_nav, m_ncx;
    QHash<QString, ManifestItem> m_manifest;
    QStringList m_spine;
    QHash<QString, int> m_sectionOf;
    QHash<QString, TextStyles> m_styles;
    QList<TocEntry> m_toc;
    BookMeta m_meta;
};

} // namespace

OpenedDocument openEpub(const QString &path) { return loaded(std::make_unique<EpubDocument>(path)); }
