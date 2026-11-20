#include <QMutex>
#include <QMutexLocker>
#include <algorithm>
#include <poppler-qt6.h>

#include "formatreaders.h"

namespace {

void addOutline(const QVector<Poppler::OutlineItem> &items, int level, int pages, QList<TocEntry> &toc) {
    for (const Poppler::OutlineItem &item : items) {
        const auto destination = item.destination();
        const int page = destination ? destination->pageNumber() - 1 : -1;
        if (page >= 0 && page < pages && !item.name().simplified().isEmpty())
            toc.append({.title = item.name().simplified(), .section = page, .anchor = {}, .level = level});
        if (level < 3)
            addOutline(item.children(), level + 1, pages, toc);
    }
}

// Poppler isn't safe to use from two threads at once, and pages are drawn off
// the GUI thread while it asks for text, so one lock guards the document.
class PdfDocument : public Document {
public:
    std::expected<void, QString> load(const QString &path) {
        m_document = Poppler::Document::load(path);
        if (!m_document)
            return std::unexpected("Not a PDF, or one too damaged to read.");
        if (m_document->isLocked())
            return std::unexpected("This PDF needs a password to open.");
        m_document->setRenderHint(Poppler::Document::Antialiasing);
        m_document->setRenderHint(Poppler::Document::TextAntialiasing);
        m_pages = m_document->numPages();
        if (m_pages < 1)
            return std::unexpected("This PDF has no pages.");
        m_meta.title = m_document->info("Title").simplified();
        m_meta.authors = BookMeta::splitAuthors(m_document->info("Author"));
        m_meta.description = m_document->info("Subject").trimmed();
        m_meta.tags = BookMeta::splitTags(m_document->info("Keywords"));
        m_meta.cover = image(0, 600);
        addOutline(m_document->outline(), 0, m_pages, m_toc);
        return {};
    }

    Kind kind() const override { return Pages; }
    int count() const override { return m_pages; }
    BookMeta meta() const override { return m_meta; }
    QList<TocEntry> toc() const override { return m_toc; }

    QImage image(int page, int width) const override {
        const QMutexLocker locker(&m_lock);
        const auto pdfPage = m_document->page(page);
        if (!pdfPage || pdfPage->pageSizeF().width() <= 0)
            return {};
        const double resolution = 72.0 * std::clamp(width, 64, 6000) / pdfPage->pageSizeF().width();
        return pdfPage->renderToImage(resolution, resolution);
    }

    QSizeF pageSize(int page) const override {
        const QMutexLocker locker(&m_lock);
        const auto pdfPage = m_document->page(page);
        return pdfPage ? pdfPage->pageSizeF() : QSizeF();
    }

    QString text(int page) const override {
        const QMutexLocker locker(&m_lock);
        const auto pdfPage = m_document->page(page);
        return pdfPage ? pdfPage->text(QRectF()).trimmed() : QString();
    }

    // Poppler's words, each found in the page's text in turn, so that its
    // place there is the one text() counts in.
    QList<PageWord> words(int page) const override {
        const QMutexLocker locker(&m_lock);
        const auto pdfPage = m_document->page(page);
        if (!pdfPage || pdfPage->pageSizeF().isEmpty())
            return {};
        const QSizeF size = pdfPage->pageSizeF();
        const QString text = pdfPage->text(QRectF()).trimmed();
        QList<PageWord> words;
        qsizetype at = 0;
        for (const auto &box : pdfPage->textList()) {
            const qsizetype found = text.indexOf(box->text(), at);
            if (box->text().isEmpty() || found < 0)
                continue;
            const QRectF bounds = box->boundingBox();
            words.append({.box = QRectF(bounds.x() / size.width(), bounds.y() / size.height(), bounds.width() / size.width(),
                                        bounds.height() / size.height()),
                          .start = found,
                          .end = found + box->text().size()});
            at = found + box->text().size();
        }
        return words;
    }

private:
    mutable QMutex m_lock;
    std::unique_ptr<Poppler::Document> m_document;
    int m_pages = 0;
    QList<TocEntry> m_toc;
    BookMeta m_meta;
};

} // namespace

OpenedDocument openPdf(const QString &path) { return loaded(std::make_unique<PdfDocument>(), path); }
