#include <QBuffer>
#include <QImageReader>

#include "formatprovider.h"
#include "formatreaders.h"
#include "htmlclean.h"

namespace {

// A book made from a provider's description: its chapters are plain data, so
// this is safe from any thread, and the pictures are read from the book's
// files when a chapter is shown.
class DescribedDocument : public Document {
public:
    DescribedDocument(const BookDescription &description, const BookSource &source)
        : m_description(description), m_source(source), m_files(source.entries()) {
        if (!description.cover.isEmpty()) {
            QByteArray data = source.read(description.cover);
            QBuffer buffer(&data);
            QImageReader reader(&buffer);
            reader.setAutoTransform(true);
            m_description.meta.cover = reader.read();
        }
        for (int section = 0; section < m_description.chapters.size(); ++section) {
            const BookDescription::Chapter &chapter = m_description.chapters.at(section);
            if (!chapter.title.isEmpty())
                m_toc.append({chapter.title, section, {}, chapter.level});
        }
    }

    Kind kind() const override { return Flow; }
    int count() const override { return m_description.chapters.size(); }
    BookMeta meta() const override { return m_description.meta; }
    QList<TocEntry> toc() const override { return m_toc; }
    QByteArray resource(const QString &name) const override { return m_source.read(name); }

    QString html(int section, int width) const override {
        if (section < 0 || section >= count())
            return {};
        const BookDescription::Chapter &chapter = m_description.chapters.at(section);
        CleanOptions options;
        options.image = [&](const QString &address) {
            const QString name = BookSource::resolve(chapter.file, address);
            return m_files.contains(name) ? bookImageAttributes(name, m_source.read(name), width) : QString();
        };
        options.link = [&](const QString &target) -> QString {
            if (target.startsWith("http://") || target.startsWith("https://") || target.startsWith("mailto:"))
                return target;
            const QString anchor = target.contains('#') ? target.section('#', 1) : QString();
            // A place in this chapter, or another chapter by its file.
            if (target.startsWith('#'))
                return QString("omabook:%1#%2").arg(section).arg(anchor);
            const QString file = BookSource::resolve(chapter.file, target);
            for (int other = 0; other < count(); ++other)
                if (!file.isEmpty() && m_description.chapters.at(other).file == file)
                    return QString("omabook:%1#%2").arg(other).arg(anchor);
            return {};
        };
        return cleanHtml(chapter.html, options);
    }

    QString text(int section) const override {
        return section < 0 || section >= count() ? QString() : htmlToText(m_description.chapters.at(section).html);
    }

private:
    BookDescription m_description;
    BookSource m_source;
    QStringList m_files;
    QList<TocEntry> m_toc;
};

} // namespace

std::unique_ptr<Document> Formats::fromDescription(const BookDescription &description, const BookSource &source) {
    return std::make_unique<DescribedDocument>(description, source);
}
