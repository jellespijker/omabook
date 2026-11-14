#include <QBuffer>
#include <QCollator>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QXmlStreamReader>

#include "bookarchive.h"
#include "formatreaders.h"

namespace {

bool isPicture(const QString &name) {
    static const QStringList suffixes{"jpg", "jpeg", "png", "gif", "webp", "bmp", "avif"};
    // macOS leaves a thumbnail twin of every file in __MACOSX.
    return suffixes.contains(QFileInfo(name).suffix().toLower()) && !name.startsWith("__MACOSX");
}

// A comic is an archive of pictures, read in the order of their names.
class ComicDocument : public Document {
public:
    explicit ComicDocument(const QString &path) : m_archive(path) {}

    bool load(const QString &path, QString *error) {
        const QStringList names = m_archive.entries();
        for (const QString &name : names)
            if (isPicture(name))
                m_pages << name;
        if (m_pages.isEmpty()) {
            *error = "This archive holds no pictures to read as a comic.";
            return false;
        }
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(m_pages.begin(), m_pages.end(), collator);

        m_meta.title = QFileInfo(path).completeBaseName();
        // ComicRack's ComicInfo.xml, where the archive carries one.
        for (const QString &name : names) {
            if (name.compare("ComicInfo.xml", Qt::CaseInsensitive) != 0)
                continue;
            QXmlStreamReader xml(m_archive.read(name));
            while (!xml.atEnd()) {
                if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() == u"ComicInfo")
                    continue;
                const QString field = xml.name().toString();
                const QString value = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
                if (value.isEmpty()) continue;
                if (field == "Title") m_meta.title = value;
                else if (field == "Series") m_meta.series = value;
                else if (field == "Number") m_meta.seriesIndex = value.toDouble();
                else if (field == "Writer") m_meta.authors = BookMeta::splitTags(value);
                else if (field == "Publisher") m_meta.publisher = value;
                else if (field == "Summary") m_meta.description = value;
                else if (field == "Genre") m_meta.tags = BookMeta::splitTags(value);
                else if (field == "LanguageISO") m_meta.language = value;
                else if (field == "Year") m_meta.published = value;
            }
        }
        m_meta.cover = image(0, 600);
        return true;
    }

    Kind kind() const override { return Pages; }
    int count() const override { return m_pages.size(); }
    BookMeta meta() const override { return m_meta; }

    QImage image(int page, int width) const override {
        if (page < 0 || page >= m_pages.size())
            return {};
        QByteArray data = m_archive.read(m_pages.at(page));
        QBuffer buffer(&data);
        QImageReader reader(&buffer);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        {
            QMutexLocker locker(&m_lock);
            m_sizes.insert(page, size);
        }
        // Decoders that can scale while they read (JPEG) do it far faster.
        if (width > 0 && size.width() > width)
            reader.setScaledSize(size.scaled(width, 1 << 20, Qt::KeepAspectRatio));
        return reader.read();
    }

    // Known once the page has been drawn; reading it sooner would mean
    // unpacking every page just to measure it.
    QSizeF pageSize(int page) const override {
        QMutexLocker locker(&m_lock);
        return m_sizes.value(page);
    }

private:
    Archive m_archive;
    QStringList m_pages;
    mutable QMutex m_lock;
    mutable QHash<int, QSize> m_sizes;
    BookMeta m_meta;
};

} // namespace

std::unique_ptr<Document> openComic(const QString &path, QString *error) {
    auto document = std::make_unique<ComicDocument>(path);
    if (!document->load(path, error))
        return nullptr;
    return document;
}
