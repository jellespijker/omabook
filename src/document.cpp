#include "document.h"

#include "htmlclean.h"

QString Document::html(int, int) const { return {}; }
QByteArray Document::resource(const QString &) const { return {}; }
QImage Document::image(int, int) const { return {}; }
QSizeF Document::pageSize(int) const { return {}; }

// A flowing book's words are its chapter without the markup; paged formats
// that have words (PDF) answer for themselves.
QString Document::text(int section) const {
    return kind() == Flow ? htmlToText(html(section, 0)) : QString();
}
