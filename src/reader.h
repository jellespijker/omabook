#pragma once

#include <QFutureWatcher>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QQuickTextDocument>
#include <QTimer>
#include <QVariantList>
#include <memory>

#include "converter.h"
#include "document.h"
#include "graphbuilder.h"
#include "library.h"
#include "metadatasource.h"
#include "pageformat.h"
#include "readingmarks.h"

class Backend;

// Serves the open book's pictures to QML: "image://book/<n>/page/<page>/<width>"
// draws a page, and "image://book/<n>/<name>" is a picture inside a chapter.
// <n> changes with every book, so a cached picture never outlives its book.
// Pictures are asked for on a loader thread while the reader swaps books on
// the GUI thread, hence the lock around the pointer.
class BookImageProvider : public QQuickImageProvider {
public:
    BookImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    void setDocument(std::shared_ptr<const Document> document);
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    QMutex m_lock;
    std::shared_ptr<const Document> m_document;
};

// The book being read: where the reader is in it, the chapter to show, the
// contents, bookmarks and search. Formats differ only in `kind`.
class Reader : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool open READ isOpen NOTIFY bookChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY loadingChanged)
    Q_PROPERTY(qint64 bookId READ bookId NOTIFY bookChanged)
    Q_PROPERTY(QString title READ title NOTIFY bookChanged)
    Q_PROPERTY(QString authors READ authors NOTIFY bookChanged)
    // "flow" for chapters of text, "pages" for fixed pages.
    Q_PROPERTY(QString kind READ kind NOTIFY bookChanged)
    Q_PROPERTY(int count READ count NOTIFY bookChanged)
    Q_PROPERTY(QVariantList toc READ toc NOTIFY bookChanged)
    Q_PROPERTY(QString imagePrefix READ imagePrefix NOTIFY bookChanged)
    Q_PROPERTY(int section READ section WRITE setSection NOTIFY sectionChanged)
    Q_PROPERTY(QString sectionTitle READ sectionTitle NOTIFY sectionChanged)
    Q_PROPERTY(QString html READ html NOTIFY htmlChanged)
    Q_PROPERTY(int pageWidth READ pageWidth WRITE setPageWidth NOTIFY htmlChanged)
    Q_PROPERTY(double fraction READ fraction WRITE setFraction NOTIFY fractionChanged)
    Q_PROPERTY(double percent READ percent NOTIFY fractionChanged)
    Q_PROPERTY(QVariantList bookmarks READ bookmarks NOTIFY bookmarksChanged)
    // The things of the book's knowledge graph, as they are marked on the page.
    Q_PROPERTY(ReadingMarks *marks READ marks CONSTANT)
    // How the text is set: the options in force, and the chapter set with them.
    Q_PROPERTY(PageFormat *format READ format CONSTANT)

public:
    // The reader reads from the backend's library. `converter` is the program
    // that turns other formats into EPUB, empty when none is installed.
    Reader(Backend *backend, const QString &converter, QObject *parent = nullptr);
    ~Reader() override;
    // The converter changes when the Calibre plugin is switched: there is
    // none while it is off, and opening a book that needs one says so.
    void setConverter(const QString &program, bool calibreOff);

    // The engine takes ownership of the provider it is given.
    BookImageProvider *imageProvider();

    bool isOpen() const noexcept { return m_document != nullptr; }
    bool loading() const noexcept { return m_loading; }
    QString error() const noexcept { return m_error; }
    qint64 bookId() const { return m_bookId; }
    QString title() const noexcept { return m_title; }
    QString authors() const noexcept { return m_authors; }
    QString kind() const;
    int count() const { return m_document ? m_document->count() : 0; }
    QVariantList toc() const noexcept { return m_toc; }
    QString imagePrefix() const { return QString("image://book/%1/").arg(m_generation); }
    int section() const noexcept { return m_section; }
    void setSection(int section);
    QString sectionTitle() const;
    QString html() const noexcept { return m_html; }
    int pageWidth() const noexcept { return m_pageWidth; }
    void setPageWidth(int width);
    double fraction() const noexcept { return m_fraction; }
    void setFraction(double fraction);
    double percent() const;
    QVariantList bookmarks() const;
    ReadingMarks *marks() const noexcept { return m_marks; }
    PageFormat *format() const noexcept { return m_format; }

    Q_INVOKABLE void openBook(qint64 id);
    Q_INVOKABLE void openBookAt(qint64 id, int section, double fraction);
    Q_INVOKABLE void close();
    // Reads the open book's file anew and stays at the same place: for a
    // book that grew while it was open. Nothing when no book is open.
    Q_INVOKABLE void reload();
    // Whether the book that just "changed" is the same one, read anew.
    bool growing() const noexcept { return m_growing; }
    // Moves to another chapter and, in it, to a named place or the very end.
    Q_INVOKABLE void goTo(int section, const QString &anchor = {}, bool atEnd = false);
    // Moves to a place partway down a chapter (0 to 1), as search results and bookmarks do.
    Q_INVOKABLE void goToPlace(int section, double fraction);
    // Where each page of a laid-out chapter starts, top to bottom, and where
    // the last one ends: pages `height` tall at most that never cut a line
    // of text in two.
    Q_INVOKABLE QVariantList pageBreaks(QQuickTextDocument *chapter, double height) const;
    // Where a named place is in a laid-out chapter, as a character position, or -1.
    Q_INVOKABLE int anchorPosition(QQuickTextDocument *chapter, const QString &anchor) const;
    // Follows a link in the text; true when it led somewhere in the book.
    Q_INVOKABLE bool follow(const QString &link);
    // Height over width of a page, or 0 while it isn't known yet.
    Q_INVOKABLE double pageAspect(int page) const;
    Q_INVOKABLE void addBookmark(const QString &text);
    Q_INVOKABLE void removeBookmark(qint64 id);
    // Every place the words occur: [{section, title, snippet}], at most 200.
    Q_INVOKABLE QVariantList find(const QString &words) const;

    // For the assistant: the chapter in front of the reader, and everything
    // up to where they are in it.
    BookMeta meta() const;
    QString sectionText() const;
    QString textSoFar() const;
    // What lies between an earlier place and where the reader is now.
    QString textSince(int section, double fraction) const;
    // For the knowledge graph: where the reader is, the text between a place
    // (the start when there is none) and there, section by section, and the
    // library the book is in.
    GraphPlace place() const { return {m_bookId, m_section, m_fraction}; }
    QList<GraphBuild::Passage> passagesSince(const std::optional<GraphPlace> &after) const;
    Library *library() const;
    // The AI's notes on the book so far, kept with the library.
    Recap notes(const QString &kind) const;
    void setNotes(const QString &kind, const QString &text);
    // The notes on the earlier books of the series this one belongs to.
    QList<SeriesNotes> seriesNotes(const QString &kind) const;
    // Those books that nothing is known of yet: no notes and no description.
    QList<Book> unknownEarlierBooks() const;
    // Keeps what a look-up said a book is about.
    void setBlurb(qint64 bookId, const QString &text);
    // The catalogs to look things up in.
    QList<MetadataSource *> newSources(QObject *parent) const;

signals:
    void bookChanged();
    void loadingChanged();
    void sectionChanged();
    void htmlChanged();
    void fractionChanged();
    void bookmarksChanged();
    // The view should show this place of the current chapter: a named place
    // when there is one, else this far down it (0 to 1).
    void moved(const QString &anchor, double fraction);

private:
    void load(const QString &file);
    void loaded();
    void fail(const QString &error);
    void refreshHtml();
    void saveProgress();
    QJsonObject whereabouts() const;
    QString textFrom(int section, double fraction, bool withFirstPage) const;

    Backend *m_backend;
    Converter m_converter;
    bool m_calibreOff = false;
    // The engine's once given to it, so it may be gone before the reader.
    QPointer<BookImageProvider> m_provider;
    // The opened book, or why it couldn't be opened.
    using Opened = QPair<std::shared_ptr<Document>, QString>;
    QFutureWatcher<Opened> m_opening;
    std::shared_ptr<Document> m_document;
    QTimer m_saveSoon;
    qint64 m_bookId = 0;
    qint64 m_pendingId = 0;
    QString m_title, m_authors, m_error, m_html;
    QVariantList m_toc;
    bool m_loading = false;
    int m_generation = 0;
    int m_section = 0;
    int m_pageWidth = 0;
    double m_fraction = 0;
    // How far the book had been read when that was last written to the reading log.
    double m_loggedPercent = 0;
    int m_wantedSection = -1; // where the book being opened should open, when it was said
    double m_wantedFraction = 0;
    bool m_reloading = false;
    bool m_reloadWanted = false; // the file changed again while it was being read anew
    // Rereads that failed in a row: a book whose file is being renamed (its
    // title changed) can't be read for a moment, so it is tried again.
    int m_reloadFailures = 0;
    bool m_growing = false;
    ReadingMarks *m_marks;
    PageFormat *m_format = nullptr;
};
