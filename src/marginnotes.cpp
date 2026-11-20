#include "marginnotes.h"

#include <QTextDocument>
#include <algorithm>

#include "annotationanchors.h"
#include "booktext.h"
#include "hyphenation.h"
#include "pagelayout.h"
#include "passagematch.h"
#include "reader.h"
#include "readingpassages.h"

namespace {

constexpr auto kind = "margin-note";

[[nodiscard]] QVariantMap shown(const Annotation &note) {
    return {{"id", note.id},
            {"section", note.place.section},
            {"fraction", note.place.fraction},
            {"quote", note.quote.simplified()},
            {"note", note.note}};
}

[[nodiscard]] QString plainOf(const QQuickTextDocument *chapter) {
    return chapter && chapter->textDocument() ? chapter->textDocument()->toPlainText() : QString();
}

} // namespace

MarginNotes::MarginNotes(QObject *parent) : QObject(parent) {}

QObject *MarginNotes::reader() const { return m_reader; }

void MarginNotes::setReader(QObject *reader) {
    auto *given = qobject_cast<Reader *>(reader);
    if (given == m_reader)
        return;
    if (m_reader)
        disconnect(m_reader, nullptr, this, nullptr);
    m_reader = given;
    if (given) {
        connect(given, &Reader::bookChanged, this, &MarginNotes::read);
        // Another chapter's notes are spanned; any that came meanwhile are read too.
        connect(given, &Reader::sectionChanged, this, &MarginNotes::read);
        // Found again in the file the book is read from, some moved and some may be lost.
        if (const auto *passages = given->findChild<ReadingPassages *>())
            connect(passages, &ReadingPassages::anchored, this, &MarginNotes::read);
    }
    read();
}

Library *MarginNotes::library() const { return m_reader && m_reader->bookId() > 0 ? m_reader->library() : nullptr; }

QVariantList MarginNotes::notes() const {
    QVariantList list;
    for (const Annotation &note : m_notes)
        list << shown(note);
    return list;
}

QVariantList MarginNotes::lost() const {
    QVariantList list;
    for (const Annotation &note : m_lost)
        list << shown(note);
    return list;
}

void MarginNotes::setDrafting(bool drafting) {
    if (drafting == m_drafting)
        return;
    m_drafting = drafting;
    announce();
}

void MarginNotes::read() {
    m_notes.clear();
    m_lost.clear();
    m_fileHash.clear();
    if (const Library *own = library()) {
        for (const Annotation &note : own->annotations(m_reader->bookId(), kind))
            (note.lost ? m_lost : m_notes) << note;
        if (const auto file = AnnotationAnchors::readableFile(*own, m_reader->bookId()))
            m_fileHash = file->hash;
    }
    announce();
}

void MarginNotes::announce() {
    ++m_revision;
    // The layout makes room for them (PageLayout::columns), the same in every chapter of the book.
    if (m_reader && m_reader->format())
        m_reader->format()->setNotes(!m_notes.isEmpty() || m_drafting);
    emit changed();
}

void MarginNotes::refresh() { read(); }

const Annotation *MarginNotes::find(qint64 id) const {
    const auto found = std::ranges::find(m_notes, id, &Annotation::id);
    if (found != m_notes.cend())
        return &*found;
    const auto lost = std::ranges::find(m_lost, id, &Annotation::id);
    return lost != m_lost.cend() ? &*lost : nullptr;
}

QList<std::pair<qint64, std::pair<qsizetype, qsizetype>>> MarginNotes::spansIn(const QString &shown) const {
    QList<std::pair<qint64, std::pair<qsizetype, qsizetype>>> spans;
    if (!m_reader)
        return spans;
    // One index of the text's words for every note sought in it.
    std::optional<PassageMatch::Index> index;
    for (const Annotation &note : m_notes) {
        if (note.place.section != m_reader->section())
            continue;
        if (!index)
            index.emplace(shown);
        const Passage passage = note.passage();
        const auto match = PassageMatch::find(
            *index, {.quote = passage.quote, .before = passage.before, .after = passage.after, .near = passage.from.fraction});
        if (match)
            spans.append({note.id, {match->start, match->end}});
    }
    return spans;
}

QVariantList MarginNotes::spans(QQuickTextDocument *chapter) {
    const QString text = plainOf(chapter);
    const size_t key = qHash(text);
    if (key == m_spannedText && m_revision == m_spannedRevision)
        return m_spans;
    m_spans.clear();
    for (const auto &[id, span] : spansIn(text))
        if (const Annotation *note = find(id))
            m_spans << QVariantMap{{"id", id}, {"start", span.first}, {"end", span.second}, {"note", note->note}};
    m_spannedText = key;
    m_spannedRevision = m_revision;
    return m_spans;
}

std::optional<std::pair<Passage, double>> MarginNotes::passageIn(const QString &shown, qsizetype start, qsizetype end) const {
    if (!m_reader || start >= end)
        return std::nullopt;
    const Hyphenation::Plain plain(shown);
    const int section = m_reader->section();
    const Passage near = Passage::of(plain.text(), section, plain.toPlain(start), plain.toPlain(end));
    if (PassageMatch::words(near.quote).isEmpty())
        return std::nullopt;
    // The page shows the chapter otherwise than the book's text has it
    // (its heading, its spacing): there its places are the text's own.
    const BookText text(m_reader->count(),
                        [reader = m_reader, section](int asked) { return reader && asked == section ? reader->sectionText() : QString(); });
    if (const std::optional<FoundPassage> found = near.findAgain(text))
        return std::pair{found->passage, found->confidence};
    return std::pair{near, 0.0};
}

std::optional<qint64> MarginNotes::write(const QString &shown, qsizetype start, qsizetype end, const QString &words) {
    Library *own = library();
    const QString said = words.trimmed();
    const auto passage = said.isEmpty() || !own ? std::nullopt : passageIn(shown, start, end);
    if (!passage)
        return std::nullopt;
    Annotation note{.id = 0, .kind = kind, .place = {}, .text = passage->first.quote.simplified().left(200), .note = said, .created = 0};
    note.setPassage(passage->first);
    // Placed in the file it is read from; one that wasn't is sought there when the book is opened next.
    if (passage->second > 0) {
        note.anchored = m_fileHash;
        note.confidence = passage->second;
    }
    const qint64 id = own->addAnnotation(m_reader->bookId(), note);
    read();
    return id > 0 ? std::optional(id) : std::nullopt;
}

qint64 MarginNotes::write(QQuickTextDocument *chapter, int start, int end, const QString &words) {
    return write(plainOf(chapter), start, end, words).value_or(0);
}

bool MarginNotes::change(qint64 id, const QString &words) {
    const Annotation *note = find(id);
    Library *own = library();
    if (!note || !own)
        return false;
    if (words.trimmed().isEmpty()) {
        remove(id);
        return true;
    }
    Annotation changed = *note;
    changed.note = words.trimmed();
    const bool written = changed.note == note->note || own->storeAnnotation(m_reader->bookId(), changed).has_value();
    read();
    return written;
}

void MarginNotes::remove(qint64 id) {
    if (Library *own = library(); own && find(id))
        own->removeAnnotation(id);
    read();
}

bool MarginNotes::placeAgain(qint64 id, const QString &shown, qsizetype start, qsizetype end) {
    Library *own = library();
    const auto passage = own && find(id) ? passageIn(shown, start, end) : std::nullopt;
    const bool placed = passage && AnnotationAnchors::placeAgain(*own, id, passage->first, m_fileHash);
    read();
    return placed;
}

bool MarginNotes::placeAgain(qint64 id, QQuickTextDocument *chapter, int start, int end) {
    return placeAgain(id, plainOf(chapter), start, end);
}

QVariantList MarginNotes::stack(const QVariantList &wanted, double spacing, double bottom) const {
    QList<PageLayout::Note> notes;
    for (const QVariant &one : wanted) {
        const QVariantMap note = one.toMap();
        notes.append({.top = note.value("top").toDouble(), .height = note.value("height").toDouble()});
    }
    QVariantList tops;
    for (const double top : PageLayout::stack(notes, spacing, bottom))
        tops << top;
    return tops;
}
