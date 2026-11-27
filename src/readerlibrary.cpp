// What the open book has in the library (Reader): the place it is read to, its
// bookmarks and the notes on it, and what happens when the book leaves the
// library while it is open (#229).

#include "reader.h"

#include "backend.h"
#include "librarybatch.h"

// A book that left the library: removed by this window, the command line, an
// agent or a sync. A library that is away says nothing of its books.
bool Reader::gone(qint64 id) const {
    const Library *library = m_backend->library();
    return library && !library->book(id);
}

bool Reader::stillThere() {
    const qint64 id = m_bookId > 0 ? m_bookId : m_pendingId;
    if (id <= 0)
        return false;
    if (!gone(id))
        return true;
    putAwayRemoved();
    return false;
}

// Nothing more is read from the book or written about it: the page says where
// it went, with the way back to the library.
void Reader::putAwayRemoved() {
    const QString why =
        tr("“%1” was removed from the library. Its files were moved to the trash, where you can get them back.").arg(m_title);
    const bool wasOpen = m_document != nullptr;
    const QJsonObject where = whereabouts();
    m_saveSoon.stop();
    m_converter.cancel();
    m_reloading = false;
    unload();
    fail(why);
    emit bookChanged();
    emit htmlChanged();
    if (wasOpen)
        m_backend->scripts()->emitEvent("book.closed", where);
}

void Reader::saveProgress() {
    m_saveSoon.stop();
    if (!m_document || !m_backend->library() || gone(m_bookId))
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
        list << QVariantMap{{"id", annotation.id},
                            {"section", annotation.place.section},
                            {"fraction", annotation.place.fraction},
                            {"text", annotation.text}};
    return list;
}

bool Reader::addBookmark(const QString &text) {
    if (!stillThere() || !m_document || !m_backend->library())
        return false;
    Annotation bookmark;
    bookmark.kind = "bookmark";
    bookmark.place = {.section = m_section, .fraction = m_fraction};
    bookmark.text = text.simplified().left(200);
    const bool kept = m_backend->library()->addAnnotation(m_bookId, bookmark) > 0;
    if (!kept)
        m_backend->say(notWritten(m_backend->library()->databasePath()).text()); // never silently lost (#259)
    emit bookmarksChanged();
    return kept;
}

void Reader::removeBookmark(qint64 id) {
    if (m_backend->library())
        m_backend->library()->removeAnnotation(id);
    emit bookmarksChanged();
}

Library *Reader::library() const { return m_backend->library(); }

Recap Reader::notes(const QString &kind) const {
    return m_document && m_backend->library() ? m_backend->library()->notes(m_bookId, kind) : Recap();
}

QList<Book> Reader::unknownEarlierBooks() const {
    QList<Book> unknown;
    const Library *library = m_document ? m_backend->library() : nullptr;
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

QList<SeriesNotes> Reader::seriesNotes(const QString &kind) const {
    return m_document && m_backend->library() ? m_backend->library()->seriesNotes(m_bookId, kind) : QList<SeriesNotes>();
}

void Reader::setNotes(const QString &kind, const QString &text) {
    if (m_document && m_backend->library())
        m_backend->library()->setNotes(m_bookId, kind, {m_section, m_fraction, text, 0});
}
