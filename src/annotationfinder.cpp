#include "annotationfinder.h"

#include <QVariantMap>

#include "annotationwords.h"
#include "bookink.h"
#include "document.h"
#include "highlights.h"
#include "lineink.h"
#include "reader.h"

using namespace Qt::StringLiterals;

namespace {

constexpr auto notesKey = "notes:"_L1;

// What one is, as the list says it.
[[nodiscard]] QString kindName(const Annotation &annotation) {
    if (annotation.kind == QLatin1StringView(Highlights::kind))
        return AnnotationFinder::tr("Your highlight");
    if (annotation.kind == QLatin1StringView(BookInk::kind) || LineInk::isKind(annotation.kind))
        return AnnotationFinder::tr("Your ink");
    if (annotation.kind == "bookmark"_L1)
        return AnnotationFinder::tr("Your bookmark");
    return AnnotationFinder::tr("Your margin note");
}

// Its words: a note's and the words it is on, a highlight's, a bookmark's name or the ink's read words.
[[nodiscard]] QString wordsOf(const Annotation &annotation) {
    const QString quote = annotation.quote.simplified();
    const QString on = annotation.region() ? AnnotationFinder::tr("A part of the page") : quote;
    if (!annotation.note.trimmed().isEmpty())
        return on.isEmpty() ? annotation.note.simplified() : AnnotationFinder::tr("%1 — on “%2”").arg(annotation.note.simplified(), on);
    if (annotation.kind == QLatin1StringView(Highlights::kind))
        return on.isEmpty() ? annotation.text.simplified() : on;
    return annotation.text.simplified();
}

} // namespace

AnnotationFinder::AnnotationFinder(QObject *parent) : QObject(parent) {}

QObject *AnnotationFinder::reader() const { return m_reader; }

void AnnotationFinder::setReader(QObject *reader) {
    auto *given = qobject_cast<Reader *>(reader);
    if (given == m_reader)
        return;
    m_reader = given;
    emit readerChanged();
}

QString AnnotationFinder::onlyNotes(const QString &typed) const {
    const QString trimmed = typed.trimmed();
    return trimmed.startsWith(notesKey, Qt::CaseInsensitive) ? trimmed.mid(notesKey.size()).trimmed() : QString();
}

QString AnnotationFinder::whereIs(int section) const {
    if (m_reader->kind() == "pages"_L1)
        return tr("Page %1").arg(section + 1);
    const std::shared_ptr<const Document> document = m_reader->document();
    QString title;
    int best = -1;
    for (const TocEntry &entry : document ? document->toc() : QList<TocEntry>())
        if (entry.section <= section && entry.section > best) {
            best = entry.section;
            title = entry.title;
        }
    return title;
}

QVariantList AnnotationFinder::find(const QString &typed) const {
    QVariantList found;
    const Library *library = m_reader ? m_reader->library() : nullptr;
    if (!library || m_reader->bookId() <= 0)
        return found;
    const QString words = onlyNotes(typed).isEmpty() ? typed : onlyNotes(typed);
    for (const Annotation &annotation : AnnotationWords::in(*library, m_reader->bookId(), AnnotationWords::phrases(words))) {
        // A margin note says which it is, as the notes' own list does (MarginNotes).
        const QVariant note = annotation.kind == "margin-note"_L1 ? QVariant(annotation.id) : QVariant();
        const QString where = annotation.lost ? tr("not found in this edition") : whereIs(annotation.place.section);
        found << QVariantMap{{"id", annotation.id},
                             {"kind", annotation.kind},
                             {"section", annotation.place.section},
                             {"fraction", annotation.place.fraction},
                             {"title", where.isEmpty() ? kindName(annotation) : u"%1 · %2"_s.arg(kindName(annotation), where)},
                             {"snippet", wordsOf(annotation)},
                             {"quote", annotation.lost ? QString() : annotation.quote.simplified()},
                             {"lost", annotation.lost},
                             {"note", note}};
    }
    return found;
}
