#include <QBuffer>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QXmlStreamReader>
#include <algorithm>
#include <array>
#include <ranges>

#include "bookarchive.h"
#include "booksource.h"
#include "formatreaders.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] bool isPicture(const QString &name) {
    static constexpr std::array suffixes{"jpg"_L1, "jpeg"_L1, "png"_L1, "gif"_L1, "webp"_L1, "bmp"_L1, "avif"_L1};
    // macOS leaves a thumbnail twin of every file in __MACOSX.
    return std::ranges::contains(suffixes, QFileInfo(name).suffix().toLower()) && !name.startsWith("__MACOSX"_L1);
}

// What ComicRack's ComicInfo.xml says of the comic, over `meta`; whether it
// says the comic is read right to left, as manga are (#265).
[[nodiscard]] bool readInfo(const QByteArray &info, BookMeta &meta) {
    bool rightToLeft = false;
    QXmlStreamReader xml(info);
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement || xml.name() == u"ComicInfo")
            continue;
        const QString field = xml.name().toString();
        const QString value = xml.readElementText(QXmlStreamReader::SkipChildElements).trimmed();
        if (value.isEmpty())
            continue;
        if (field == "Title")
            meta.title = value;
        else if (field == "Series")
            meta.series = value;
        else if (field == "Number")
            meta.seriesIndex = value.toDouble();
        else if (field == "Writer")
            meta.authors = BookMeta::splitTags(value);
        else if (field == "Publisher")
            meta.publisher = value;
        else if (field == "Summary")
            meta.description = value;
        else if (field == "Genre")
            meta.tags = BookMeta::splitTags(value);
        else if (field == "LanguageISO")
            meta.language = value;
        else if (field == "Year")
            meta.published = value;
        else if (field == "Manga")
            rightToLeft = value.compare(u"YesAndRightToLeft", Qt::CaseInsensitive) == 0;
    }
    return rightToLeft;
}

// A comic is an archive of pictures, read in the order of their names.
class ComicDocument : public Document {
public:
    explicit ComicDocument(const QString &path) : m_archive(path) {}

    std::expected<void, Failure> load(const QString &path) {
        const QStringList names = m_archive.entries();
        m_pages = names | std::views::filter(isPicture) | std::ranges::to<QStringList>();
        if (m_pages.isEmpty())
            return std::unexpected(Failure(Failure::Code::ComicWithoutPictures));
        BookSource::sortNaturally(m_pages);

        m_meta.title = QFileInfo(path).completeBaseName();
        // ComicRack's ComicInfo.xml, where the archive carries one.
        for (const QString &name : names)
            if (name.compare("ComicInfo.xml", Qt::CaseInsensitive) == 0)
                m_rightToLeft = readInfo(m_archive.read(name), m_meta);
        m_meta.cover = image(0, 600);
        // Pictures none of which can be drawn would show as blank pages (#264).
        m_shown = !m_meta.cover.isNull()
                  || std::ranges::any_of(std::views::iota(1, count()), [this](int page) { return !image(page, 600).isNull(); });
        return {};
    }

    std::optional<Failure> unreadable() const override {
        return m_shown ? std::nullopt : std::optional(Failure(Failure::Code::ComicPicturesUnreadable));
    }

    Qt::LayoutDirection pageDirection(const QString & /*language*/) const override {
        return m_rightToLeft ? Qt::RightToLeft : Qt::LayoutDirectionAuto;
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
            const QMutexLocker locker(&m_lock);
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
        const QMutexLocker locker(&m_lock);
        return m_sizes.value(page);
    }

private:
    Archive m_archive;
    QStringList m_pages;
    mutable QMutex m_lock;
    mutable QHash<int, QSize> m_sizes;
    BookMeta m_meta;
    bool m_shown = false;
    bool m_rightToLeft = false;
};

} // namespace

OpenedDocument openComic(const QString &path) { return loaded(std::make_unique<ComicDocument>(path), path); }
