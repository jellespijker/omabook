#include "blurb.h"

namespace {

QString letters(const QString &text) {
    QString kept;
    for (const QChar c : text.section(':', 0, 0).toLower())
        if (c.isLetterOrNumber())
            kept += c;
    return kept;
}

} // namespace

bool BlurbSearch::sameTitle(const QString &asked, const QString &found) {
    const QString a = letters(asked), b = letters(found);
    return !a.isEmpty() && !b.isEmpty() && (a == b || a.startsWith(b) || b.startsWith(a));
}

BlurbSearch::BlurbSearch(const QList<Book> &books, const Sources &sources, int patience, QObject *parent)
    : QObject(parent) {
    for (const Book &book : books) {
        for (MetadataSource *source : sources ? sources(this) : QList<MetadataSource *>()) {
            ++m_waiting;
            connect(source, &MetadataSource::found, this, [this, book](const QList<MetaCandidate> &candidates) {
                for (const MetaCandidate &candidate : candidates) {
                    const QString text = candidate.meta.description.trimmed();
                    if (!m_blurbs.contains(book.id) && !text.isEmpty() && sameTitle(book.meta.title, candidate.meta.title)) {
                        // A blurb is a paragraph; a catalog's long essay is cut.
                        m_blurbs.insert(book.id, text.left(1200));
                        break;
                    }
                }
                answered();
            });
            connect(source, &MetadataSource::failed, this, &BlurbSearch::answered);
            source->search(book.meta);
        }
    }
    m_patience.setSingleShot(true);
    connect(&m_patience, &QTimer::timeout, this, [this] {
        m_waiting = 1;
        answered();
    });
    // With nothing to ask, the answer still arrives after the constructor,
    // when whoever made this has had the chance to listen.
    m_patience.start(m_waiting > 0 ? patience : 0);
}

void BlurbSearch::answered() {
    if (m_done || --m_waiting > 0)
        return;
    m_done = true;
    m_patience.stop();
    emit finished(m_blurbs);
}
