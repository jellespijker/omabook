#include "reader.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QAbstractTextDocumentLayout>
#include <QStandardPaths>
#include <QTextLayout>
#include <QTextBlock>
#include <QTextDocument>
#include <QtConcurrent>

#include "backend.h"
#include "documenttext.h"
#include "formatreaders.h"
#include "formats.h"
#include "pagelayout.h"

void BookImageProvider::setDocument(std::shared_ptr<const Document> document) {
    QMutexLocker locker(&m_lock);
    m_document = std::move(document);
}

QImage BookImageProvider::requestImage(const QString &id, QSize *size, const QSize &) {
    std::shared_ptr<const Document> document;
    {
        QMutexLocker locker(&m_lock);
        document = m_document;
    }
    QImage image;
    const QStringList parts = id.split('/');
    if (document && parts.size() == 4 && parts.at(1) == "page")
        image = document->image(parts.at(2).toInt(), parts.at(3).toInt());
    else if (document && parts.size() == 2)
        image = QImage::fromData(document->resource(bookImageName(parts.at(1))));
    if (size)
        *size = image.size();
    return image;
}

Reader::Reader(Backend *backend, const QString &converter, QObject *parent)
    : QObject(parent), m_backend(backend), m_converter(converter), m_marks(new ReadingMarks(this, this)) {
    m_format = new PageFormat(this, backend->settings(), this);
    // A book's own format changed elsewhere (the command line, an agent),
    // or another library came with other books.
    connect(backend, &Backend::bookFormatsChanged, m_format, &PageFormat::refresh);
    connect(backend, &Backend::libraryChanged, m_format, &PageFormat::refresh);
    // Where the reader is gets written a moment after they stop moving, not
    // on every pixel scrolled.
    m_saveSoon.setSingleShot(true);
    m_saveSoon.setInterval(1500);
    connect(&m_saveSoon, &QTimer::timeout, this, &Reader::saveProgress);
    // The script plugins hear where the reader is as often as it is written
    // down, and may ask for a book to be opened.
    connect(&m_saveSoon, &QTimer::timeout, this, [this] {
        if (m_document)
            m_backend->scripts()->emitEvent("page.turned", whereabouts());
    });
    connect(m_backend->scripts(), &ScriptPlugins::openBookRequested, this, [this](qint64 id, int section, double fraction) {
        if (section < 0)
            openBook(id);
        else
            openBookAt(id, section, fraction);
    });
    // A plugin that grew the open book has the reader follow it.
    connect(m_backend->scripts(), &ScriptPlugins::reloadBookRequested, this, [this](qint64 id) {
        if (id == 0 || id == m_bookId)
            reload();
    });
    connect(&m_opening, &QFutureWatcher<Opened>::finished, this, &Reader::loaded);
    connect(&m_converter, &Converter::finished, this, [this](const QString &output, const QString &error) {
        if (m_pendingId == 0)
            return;
        // The EPUB joins the book's formats, so the next opening is instant;
        // one the book was given meanwhile stays, and is what opens.
        QString problem = error;
        if (problem.isEmpty() && m_backend->library() && m_backend->library()->formatPath(m_pendingId, "epub").isEmpty())
            problem = m_backend->library()->addFormat(m_pendingId, output).transform_error(&Failure::text).error_or(QString());
        QFile::remove(output);
        if (!problem.isEmpty())
            fail(problem);
        else
            load(m_backend->library()->formatPath(m_pendingId, "epub"));
    });
}

Reader::~Reader() {
    saveProgress();
    m_opening.waitForFinished();
    // The engine may already have deleted the provider it owns.
    if (m_provider)
        m_provider->setDocument(nullptr);
}

BookImageProvider *Reader::imageProvider() {
    if (!m_provider) {
        m_provider = new BookImageProvider;
        m_provider->setDocument(m_document);
    }
    return m_provider;
}

QString Reader::kind() const {
    return !m_document ? QString() : m_document->kind() == Document::Flow ? "flow" : "pages";
}

// Opens a book at a place rather than where it was last read to.
void Reader::openBookAt(qint64 id, int section, double fraction) {
    openBook(id);
    if (m_pendingId == id) {
        m_wantedSection = section;
        m_wantedFraction = fraction;
    }
}

void Reader::openBook(qint64 id) {
    close();
    m_wantedSection = -1;
    Library *library = m_backend->library();
    const auto book = library ? library->book(id) : std::nullopt;
    // A plugin's own kind of book opens in the plugin's view, never here (#125).
    if (!book || m_backend->scripts()->openInView(*book))
        return;
    // A paper book, or one only on a list, has nothing to open, unless a space it is in brings its file (#63).
    if (book->formats.isEmpty()) {
        m_backend->spaces()->bring(*book, [this, id] { openBook(id); });
        return;
    }
    m_pendingId = id;
    m_title = book->meta.title;
    m_authors = book->meta.authorText();
    m_error.clear();
    m_loading = true;
    emit loadingChanged();

    // Read here, or a book of a plugin that isn't, which says which.
    if (const QString format = Formats::openedAs(book->formats); !format.isEmpty()) {
        load(library->formatPath(id, format));
        return;
    }
    const QString source = library->formatPath(id);
    if (source.isEmpty()) {
        fail(tr("This book has no file to read."));
        return;
    }
    if (!m_converter.available() && m_calibreOff) {
        fail(tr("%1 books are read by converting them to EPUB with Calibre, and the Calibre plugin is switched off. "
                "Switch it on under Plugins and open the book again.")
                 .arg(Formats::name(book->formats.value(0))));
        return;
    }
    if (!m_converter.available()) {
        fail(tr("%1 books are read by converting them to EPUB, which needs Calibre's ebook-convert. Install the "
                "calibre package and open the book again.")
                 .arg(Formats::name(book->formats.value(0))));
        return;
    }
    const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(cache);
    m_converter.convert(source, cache + QString("/converting-%1.epub").arg(id));
}

// Opening reads the whole table of contents and, for some archives, every
// header, so it happens off the GUI thread.
void Reader::load(const QString &file) {
    m_opening.setFuture(QtConcurrent::run([file]() -> Opened {
        QString error;
        std::shared_ptr<Document> document = Formats::open(file, &error);
        return {document, error};
    }));
}

// Where the reader is, as script plugins are told of it.
QJsonObject Reader::whereabouts() const {
    const auto book = m_backend->library() ? m_backend->library()->book(m_bookId) : std::nullopt;
    const QJsonObject identifiers = book ? book->identifiers : QJsonObject();
    return {{"id", m_bookId}, {"generated", identifiers.value("generated").toString()}, {"identifiers", identifiers},
            {"section", m_section}, {"sections", count()}, {"fraction", m_fraction}, {"percent", percent()}};
}

// The open book's file changed under the reader: a plugin that writes a
// book as it is read added to it. The file is read anew, beside the window
// like any opening, and the reader stays where it is.
void Reader::reload() {
    Library *library = m_backend->library();
    const auto book = m_document && library ? library->book(m_bookId) : std::nullopt;
    if (m_reloading)
        m_reloadWanted = true;
    if (!book || m_reloading || m_pendingId != 0)
        return;
    for (const QString &format : book->formats) {
        if (Formats::readable(format)) {
            saveProgress();
            m_reloading = true;
            load(library->formatPath(m_bookId, format));
            return;
        }
    }
}

void Reader::loaded() {
    const auto [document, error] = m_opening.result();
    const bool again = m_reloading;
    m_reloading = false;
    if (again && m_reloadWanted)
        QTimer::singleShot(0, this, &Reader::reload);
    m_reloadWanted = false;
    if (again) {
        // What was open stays open when the new version can't be read.
        if (!m_document)
            return;
        if (!document) {
            // A book renamed as it is written moves its file; the new path is
            // there a moment later, so it is read again before anything is said.
            if (++m_reloadFailures <= 2) {
                QTimer::singleShot(400 * m_reloadFailures, this, &Reader::reload);
                return;
            }
            m_reloadFailures = 0;
            m_backend->say(tr("The book changed, and can't be read as it is now: %1").arg(error));
            return;
        }
        m_reloadFailures = 0;
    } else {
        if (m_pendingId == 0)
            return; // closed while it was opening
        if (!document) {
            fail(error);
            return;
        }
        m_bookId = m_pendingId;
        m_pendingId = 0;
    }
    m_document = document;
    ++m_generation;
    if (m_provider)
        m_provider->setDocument(m_document);

    m_toc.clear();
    for (const TocEntry &entry : m_document->toc())
        m_toc << QVariantMap{{"title", entry.title}, {"section", entry.section}, {"anchor", entry.anchor}, {"level", entry.level}};

    if (again) {
        // The same place; only a book that got shorter moves the reader.
        m_section = qBound(0, m_section, m_document->count() - 1);
        if (const auto book = m_backend->library()->book(m_bookId)) {
            m_title = book->meta.title;
            m_authors = book->meta.authorText();
        }
        refreshHtml();
        // The book is the one it was, so what goes with a book (the
        // conversation about it) stays; see growing().
        m_growing = true;
        emit bookChanged();
        m_growing = false;
        emit sectionChanged();
        emit fractionChanged();
        m_backend->scripts()->emitEvent("page.turned", whereabouts());
        return;
    }
    Progress progress = m_backend->library()->progress(m_bookId);
    if (m_wantedSection >= 0) {
        progress.place.section = m_wantedSection;
        progress.place.fraction = m_wantedFraction;
        m_wantedSection = -1;
    }
    m_section = qBound(0, progress.place.section, m_document->count() - 1);
    m_fraction = qBound(0.0, progress.place.fraction, 1.0);
    m_session.begin(percent());
    m_loading = false;
    refreshHtml();
    emit bookChanged();
    emit loadingChanged();
    emit sectionChanged();
    emit fractionChanged();
    emit bookmarksChanged();
    if (const auto book = m_backend->library()->book(m_bookId)) {
        QJsonObject opened = book->toJson();
        opened.insert("has_cover", book->hasCover);
        QJsonObject where = whereabouts();
        where.insert("book", opened);
        m_backend->scripts()->emitEvent("book.opened", where);
    }
}

void Reader::setConverter(const QString &program, bool calibreOff) {
    m_converter.setProgram(program);
    m_calibreOff = calibreOff;
}

void Reader::fail(const QString &error) {
    m_pendingId = 0;
    m_loading = false;
    m_error = error;
    emit loadingChanged();
}

void Reader::close() {
    m_converter.cancel();
    m_pendingId = 0;
    m_reloading = false;
    if (m_loading || !m_error.isEmpty()) {
        m_loading = false;
        m_error.clear();
        emit loadingChanged();
    }
    if (!m_document)
        return;
    saveProgress();
    // The book's node in the knowledge graph says how far it has been read.
    if (const Library *library = m_backend->library())
        Graph(*library).bookNode(m_bookId);
    m_backend->reload();
    m_backend->scripts()->emitEvent("book.closed", whereabouts());
    m_document.reset();
    if (m_provider)
        m_provider->setDocument(nullptr);
    m_bookId = 0;
    m_html.clear();
    m_toc.clear();
    emit bookChanged();
    emit htmlChanged();
}

namespace {
// Pictures no taller than `most`, their width shrunk with them: a picture
// laid out taller than a page can only be cut across two.
QString fitPictures(QString html, int most) {
    if (most <= 0)
        return html;
    static const QRegularExpression sized(R"re((<img [^>]*?)width="(\d+)" height="(\d+)")re");
    QString fitted;
    qsizetype from = 0;
    for (auto it = sized.globalMatch(html); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const int width = match.captured(2).toInt(), height = match.captured(3).toInt();
        fitted += QStringView(html).mid(from, match.capturedStart() - from);
        if (height > most && height > 0)
            fitted += QString(R"re(%1width="%2" height="%3")re").arg(match.captured(1)).arg(std::max(1, width * most / height)).arg(most);
        else
            fitted += match.captured(0);
        from = match.capturedEnd();
    }
    return fitted + QStringView(html).mid(from);
}
} // namespace

void Reader::refreshHtml() {
    const QString html = m_document && m_document->kind() == Document::Flow
                             ? fitPictures(m_document->html(m_section, m_pageWidth).replace("image://book/", imagePrefix()), m_pageHeight)
                             : QString();
    if (html != m_html) {
        m_html = html;
        emit htmlChanged();
    }
}

void Reader::setPageWidth(int width) {
    if (width == m_pageWidth || width <= 0)
        return;
    m_pageWidth = width;
    refreshHtml();
}

void Reader::setPageHeight(int height) {
    if (height == m_pageHeight || height < 0)
        return;
    m_pageHeight = height;
    refreshHtml();
}

void Reader::setSection(int section) {
    if (!m_document || section == m_section || section < 0 || section >= m_document->count())
        return;
    m_section = section;
    m_fraction = 0;
    refreshHtml();
    emit sectionChanged();
    emit fractionChanged();
    m_saveSoon.start();
}

void Reader::goTo(int section, const QString &anchor, bool atEnd) {
    if (!m_document || section < 0 || section >= m_document->count())
        return;
    setSection(section);
    setFraction(atEnd ? 1 : 0);
    emit moved(anchor, atEnd ? 1 : 0);
}

void Reader::goToPlace(int section, double fraction) {
    if (!m_document || section < 0 || section >= m_document->count())
        return;
    setSection(section);
    setFraction(fraction);
    emit moved({}, qBound(0.0, fraction, 1.0));
}

QVariantList Reader::pageBreaks(QQuickTextDocument *chapter, double height) const {
    if (!chapter || !chapter->textDocument())
        return {0.0, 0.0};
    QVariantList breaks;
    for (const double at : PageLayout::pageBreaks(*chapter->textDocument(), height))
        breaks << at;
    return breaks;
}

double Reader::screenStep(QQuickTextDocument *chapter, double top, double height, bool forward) const {
    if (!chapter || !chapter->textDocument())
        return forward ? top + height : std::max(0.0, top - height);
    return PageLayout::screenStep(*chapter->textDocument(), top, height, forward);
}

int Reader::anchorPosition(QQuickTextDocument *chapter, const QString &anchor) const {
    if (!chapter || !chapter->textDocument() || anchor.isEmpty())
        return -1;
    for (QTextBlock block = chapter->textDocument()->begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().anchorNames().contains(anchor))
                return it.fragment().position();
    return -1;
}

bool Reader::follow(const QString &link) {
    if (!link.startsWith("omabook:"))
        return false;
    const QString target = link.mid(8);
    goTo(target.section('#', 0, 0).toInt(), target.section('#', 1));
    return true;
}

void Reader::setFraction(double fraction) {
    fraction = qBound(0.0, fraction, 1.0);
    if (qFuzzyCompare(fraction + 1, m_fraction + 1))
        return;
    m_fraction = fraction;
    emit fractionChanged();
    m_saveSoon.start();
}

double Reader::percent() const {
    if (!m_document || m_document->count() == 0)
        return 0;
    // The last page of a paged book is the whole book read.
    if (m_document->kind() == Document::Pages)
        return m_document->count() == 1 ? 100 : 100.0 * m_section / (m_document->count() - 1);
    return 100.0 * (m_section + m_fraction) / m_document->count();
}

QString Reader::sectionTitle() const { return titleOf(m_section); }

QString Reader::nextTitle() const {
    return m_document && m_section + 1 < m_document->count() ? titleOf(m_section + 1) : QString();
}

QString Reader::titleOf(int section) const {
    // The first entry of the nearest chapter at or before this one: a file
    // holding several entries is named by the one it opens with.
    QString title;
    int best = -1;
    for (const TocEntry &entry : m_document ? m_document->toc() : QList<TocEntry>()) {
        if (entry.section <= section && entry.section > best) {
            best = entry.section;
            title = entry.title;
        }
    }
    return title;
}

double Reader::pageAspect(int page) const {
    const QSizeF size = m_document ? m_document->pageSize(page) : QSizeF();
    return size.width() > 0 ? size.height() / size.width() : 0;
}

void Reader::saveProgress() {
    m_saveSoon.stop();
    if (!m_document || !m_backend->library())
        return;
    m_backend->library()->setProgress(m_bookId, {m_section, m_fraction, percent()});
    // The reading log: that the book went from there to here today, and the time it took.
    m_session.save(*m_backend->library(), m_bookId, percent());
}

QVariantList Reader::bookmarks() const {
    QVariantList list;
    if (!m_document || !m_backend->library())
        return list;
    for (const Annotation &annotation : m_backend->library()->annotations(m_bookId, "bookmark"))
        list << QVariantMap{{"id", annotation.id}, {"section", annotation.place.section}, {"fraction", annotation.place.fraction},
                            {"text", annotation.text}};
    return list;
}

void Reader::addBookmark(const QString &text) {
    if (!m_document || !m_backend->library())
        return;
    Annotation bookmark;
    bookmark.kind = "bookmark";
    bookmark.place = {.section = m_section, .fraction = m_fraction};
    bookmark.text = text.simplified().left(200);
    m_backend->library()->addAnnotation(m_bookId, bookmark);
    emit bookmarksChanged();
}

void Reader::removeBookmark(qint64 id) {
    if (m_backend->library())
        m_backend->library()->removeAnnotation(id);
    emit bookmarksChanged();
}

QVariantList Reader::find(const QString &words) const {
    return m_document ? DocumentText::find(*m_document, words) : QVariantList();
}

BookMeta Reader::meta() const {
    BookMeta meta;
    meta.title = m_title;
    meta.authors = BookMeta::splitAuthors(m_authors);
    return meta;
}

QString Reader::sectionText() const { return m_document ? m_document->text(m_section) : QString(); }

QString Reader::textSoFar() const { return textFrom(0, 0, true); }

QString Reader::textSince(int section, double fraction) const { return textFrom(section, fraction, false); }

QString Reader::textFrom(int fromSection, double fromFraction, bool withFirstPage) const {
    return m_document ? DocumentText::between(*m_document, {fromSection, fromFraction, m_section, m_fraction}, withFirstPage) : QString();
}

QList<GraphBuild::Passage> Reader::passagesSince(const std::optional<GraphPlace> &after) const {
    return m_document ? GraphBuild::passages(*m_document, after, place()) : QList<GraphBuild::Passage>();
}

Library *Reader::library() const { return m_backend->library(); }

Recap Reader::notes(const QString &kind) const {
    return m_document && m_backend->library() ? m_backend->library()->notes(m_bookId, kind) : Recap();
}

QList<Book> Reader::unknownEarlierBooks() const {
    QList<Book> unknown;
    Library *library = m_document ? m_backend->library() : nullptr;
    for (const Book &book : library ? library->earlierInSeries(m_bookId) : QList<Book>()) {
        bool known = !book.meta.description.trimmed().isEmpty();
        for (const QString &kind : {QString("recap"), QString("cast"), QString("blurb")})
            known = known || !library->notes(book.id, kind).text.isEmpty();
        if (!known)
            unknown.append(book);
    }
    return unknown;
}

void Reader::setBlurb(qint64 bookId, const QString &text) {
    if (m_backend->library())
        m_backend->library()->setNotes(bookId, "blurb", {0, 0, text, 0});
}

QList<MetadataSource *> Reader::newSources(QObject *parent) const { return m_backend->newSources(parent); }

QList<SeriesNotes> Reader::seriesNotes(const QString &kind) const {
    return m_document && m_backend->library() ? m_backend->library()->seriesNotes(m_bookId, kind) : QList<SeriesNotes>();
}

void Reader::setNotes(const QString &kind, const QString &text) {
    if (m_document && m_backend->library())
        m_backend->library()->setNotes(m_bookId, kind, {m_section, m_fraction, text, 0});
}
