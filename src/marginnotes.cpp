#include "marginnotes.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QTextDocument>
#include <algorithm>

#include "annotationanchors.h"
#include "annotationink.h"
#include "backend.h"
#include "booktext.h"
#include "inkcodec.h"
#include "inksurface.h"
#include "librarybatch.h"
#include "noterecognition.h"
#include "pagelayout.h"
#include "pagetext.h"
#include "passagematch.h"
#include "reader.h"
#include "readingpassages.h"
#include "settings.h"
#include "textgeometry.h"

namespace {

constexpr auto kind = "margin-note";

[[nodiscard]] QVariantMap shown(const Annotation &note, bool ink) {
    return {{"id", note.id},
            {"section", note.place.section},
            {"fraction", note.place.fraction},
            {"quote", note.quote.simplified()},
            {"note", note.note},
            {"ink", ink}};
}

// Strokes as a note keeps them: `scale` ink units to one of theirs, their
// top corner a little in from the note's, and no wider than it.
[[nodiscard]] std::vector<Ink::Stroke> fitted(std::vector<Ink::Stroke> strokes, double scale) {
    constexpr double margin = 6;
    QRectF box;
    for (const Ink::Stroke &stroke : strokes)
        box |= Ink::bounds(stroke);
    const double room = MarginNotes::inkWidth - (2 * margin);
    const double factor = box.width() * scale > room ? room / box.width() : scale;
    for (Ink::Stroke &stroke : strokes) {
        stroke.width *= factor;
        for (Ink::Point &point : stroke.points)
            point.at = ((point.at - box.topLeft()) * factor) + QPointF(margin, margin);
    }
    return strokes;
}

[[nodiscard]] QString plainOf(const QQuickTextDocument *chapter) {
    return chapter && chapter->textDocument() ? chapter->textDocument()->toPlainText() : QString();
}

} // namespace

MarginNotes::MarginNotes(QObject *parent) : QObject(parent) {}

MarginNotes::~MarginNotes() = default;

void MarginNotes::setSettings(QObject *settings) {
    if (settings == m_settings)
        return;
    m_settings = settings;
    m_recognition.reset();
    if (const auto *chosen = qobject_cast<const Settings *>(settings)) {
        // A note read after its book was closed is still the library's.
        m_recognition = std::make_unique<NoteRecognition>(RecognitionQueue::routeFor(*chosen),
                                                          [this] { return m_reader ? m_reader->library() : nullptr; });
        connect(m_recognition.get(), &NoteRecognition::read, this, &MarginNotes::read);
        connect(m_recognition.get(), &NoteRecognition::stateChanged, this, [this] {
            ++m_readings;
            emit readingsChanged();
        });
    }
    emit changed();
}

QObject *MarginNotes::reader() const { return m_reader; }

void MarginNotes::setReader(QObject *reader) {
    auto *given = qobject_cast<Reader *>(reader);
    if (given == m_reader)
        return;
    if (m_reader)
        disconnect(m_reader, nullptr, this, nullptr);
    m_reader = given;
    if (given) {
        connect(given, &Reader::bookChanged, this, [this] {
            m_words.clear();
            read();
        });
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
        list << shown(note, inked(note.id));
    return list;
}

QVariantList MarginNotes::lost() const {
    QVariantList list;
    for (const Annotation &note : m_lost)
        list << shown(note, inked(note.id));
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
    m_inked.clear();
    m_fileHash.clear();
    if (const Library *own = library()) {
        for (const Annotation &note : own->annotations(m_reader->bookId(), kind)) {
            (note.lost ? m_lost : m_notes) << note;
            if (AnnotationInk::has(*own, note.id))
                m_inked.insert(note.id);
        }
        if (const auto file = AnnotationAnchors::readableFile(*own, m_reader->bookId()))
            m_fileHash = file->hash;
    }
    announce();
}

void MarginNotes::announce() {
    ++m_revision;
    // The layout makes room for them (PageLayout::columns), the same in every chapter of the book.
    if (m_reader && m_reader->format())
        m_reader->format()->setNotes(!m_notes.isEmpty() || m_drafting || m_othersWanted);
    emit changed();
}

void MarginNotes::setOthersWanted(bool wanted) {
    if (m_othersWanted == wanted)
        return;
    m_othersWanted = wanted;
    announce();
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
            m_spans << QVariantMap{{"id", id}, {"start", span.first}, {"end", span.second}, {"note", note->note}, {"ink", inked(id)}};
    m_spannedText = key;
    m_spannedRevision = m_revision;
    return m_spans;
}

std::optional<std::pair<Passage, double>> MarginNotes::passageIn(const QString &shown, qsizetype start, qsizetype end) const {
    if (!m_reader)
        return std::nullopt;
    const int section = m_reader->section();
    const BookText text(m_reader->count(),
                        [reader = m_reader, section](int asked) { return reader && asked == section ? reader->sectionText() : QString(); });
    const std::optional<FoundPassage> found = Passage::shownAt(shown, section, start, end, text);
    return found ? std::optional(std::pair{found->passage, found->confidence}) : std::nullopt;
}

std::optional<qint64> MarginNotes::write(const QString &shown, qsizetype start, qsizetype end, const QString &words) {
    const QString said = words.trimmed();
    const auto passage = said.isEmpty() || !library() ? std::nullopt : passageIn(shown, start, end);
    if (!passage)
        return std::nullopt;
    const qint64 id = add(noteOn(*passage, said));
    return id > 0 ? std::optional(id) : std::nullopt;
}

// A note the library didn't take is said not to be kept, and typed words are
// put on the clipboard, never silently lost (#259).
void MarginNotes::sayNotKept(const Library &library, const QString &words) const {
    QString why = notWritten(library.databasePath()).text();
    if (!words.isEmpty()) {
        QGuiApplication::clipboard()->setText(words);
        why += u' ' + tr("The note's words were copied to the clipboard.");
    }
    m_reader->backend()->say(why);
}

qint64 MarginNotes::add(const Annotation &note) {
    Library *own = library();
    if (!own)
        return 0;
    const qint64 id = own->addAnnotation(m_reader->bookId(), note);
    if (id <= 0)
        sayNotKept(*own, note.note);
    read();
    return id;
}

std::optional<Passage> MarginNotes::passageOnPage(int page, int start, int end) const {
    const std::shared_ptr<const Document> document = m_reader ? m_reader->document() : nullptr;
    if (!document || page < 0 || page >= document->count())
        return std::nullopt;
    // A PDF's page's text is the book's own text of it: its places are exact.
    const QString text = document->text(page);
    const auto [from, to] = TextGeometry::wholeWords(text, start, end);
    if (to <= from)
        return std::nullopt;
    const Passage passage = Passage::of(text, page, from, to);
    return PassageMatch::words(passage.quote).isEmpty() ? std::nullopt : std::optional(passage);
}

qint64 MarginNotes::writeOnPage(int page, int start, int end, const QString &words) {
    const QString said = words.trimmed();
    const std::optional<Passage> passage = said.isEmpty() ? std::nullopt : passageOnPage(page, start, end);
    if (!passage)
        return 0;
    // Its places are those of the book's own text of the page: exact.
    return add(noteOn({*passage, 1.0}, said));
}

qint64 MarginNotes::writeOnPart(int page, const QRectF &area, double aspect, const QString &words) {
    Annotation note = Annotation::on(kind, {.page = page, .area = area}, aspect);
    note.note = words.trimmed();
    if (note.note.isEmpty() || note.area.isEmpty())
        return 0;
    // The page is its place, in the file it was written on (ADR 0006).
    note.anchored = m_fileHash;
    return add(note);
}

bool MarginNotes::placeAgainOnPage(qint64 id, int page, int start, int end) {
    Library *own = library();
    const std::optional<Passage> passage = own && find(id) ? passageOnPage(page, start, end) : std::nullopt;
    const bool placed = passage && AnnotationAnchors::placeAgain(*own, id, *passage, m_fileHash);
    read();
    return placed;
}

QVariantList MarginNotes::pageNotes(int page, QSizeF shown) {
    const std::shared_ptr<const Document> document = m_reader ? m_reader->document() : nullptr;
    QVariantList found;
    if (!document || page < 0 || page >= document->count() || shown.isEmpty())
        return found;
    const QString text = document->text(page);
    const auto standing = [&found](const Annotation &note, bool part, const QRectF &around, const QRectF &end) {
        found << QVariantMap{
            {"id", note.id},           {"note", note.note},         {"part", part},        {"x", around.x()},      {"y", around.y()},
            {"width", around.width()}, {"height", around.height()}, {"endX", end.right()}, {"endY", end.bottom()}, {"endTop", end.top()}};
    };
    for (const Annotation &note : std::as_const(m_notes)) {
        if (note.place.section != page)
            continue;
        if (const std::optional<PageRegion> region = note.region()) {
            const QRectF part(region->area.topLeft() * shown.width(), region->area.size() * shown.width());
            standing(note, true, part, part);
            continue;
        }
        const auto span = text.isEmpty() ? std::nullopt : note.passage().spanIn(text);
        if (!span)
            continue;
        if (!m_words.contains(page))
            m_words.insert(page, document->words(page));
        const QList<QRectF> rects =
            PageText::rects(m_words.value(page), static_cast<int>(span->first), static_cast<int>(span->second), shown);
        if (rects.isEmpty())
            continue;
        QRectF around;
        for (const QRectF &rect : rects)
            around |= rect;
        standing(note, false, around, rects.last());
    }
    return found;
}

Annotation MarginNotes::noteOn(const std::pair<Passage, double> &passage, const QString &words) const {
    Annotation note{.id = 0, .kind = kind, .place = {}, .text = passage.first.quote.simplified().left(200), .note = words, .created = 0};
    note.setPassage(passage.first);
    // Placed in the file it is read from; one that wasn't is sought there when the book is opened next.
    if (passage.second > 0) {
        note.anchored = m_fileHash;
        note.confidence = passage.second;
    }
    return note;
}

qint64 MarginNotes::write(QQuickTextDocument *chapter, int start, int end, const QString &words) {
    return write(plainOf(chapter), start, end, words).value_or(0);
}

bool MarginNotes::change(qint64 id, const QString &words) {
    const Annotation *note = find(id);
    Library *own = library();
    if (!note || !own)
        return false;
    // Its ink is the note written by hand: that stays without words.
    if (words.trimmed().isEmpty() && !inked(id))
        return remove(id);
    Annotation changed = *note;
    changed.note = words.trimmed();
    const bool written = changed.note == note->note || own->storeAnnotation(m_reader->bookId(), changed).has_value();
    if (!written)
        sayNotKept(*own, changed.note);
    read();
    return written;
}

bool MarginNotes::remove(qint64 id) {
    Library *own = m_reader && m_reader->stillThere() ? library() : nullptr;
    const bool removed = own && find(id);
    if (removed)
        own->removeAnnotation(id);
    read();
    return removed;
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

std::optional<qint64> MarginNotes::writeInk(const QString &shown, qsizetype start, qsizetype end, std::vector<Ink::Stroke> strokes,
                                            double scale) {
    Library *own = library();
    const auto passage = strokes.empty() || !own || scale <= 0 ? std::nullopt : passageIn(shown, start, end);
    if (!passage)
        return std::nullopt;
    const qint64 id = [&] {
        const Library::Batch batch(*own);
        const qint64 made = own->addAnnotation(m_reader->bookId(), noteOn(*passage, {}));
        if (made > 0 && !AnnotationInk::write(*own, made, {.added = fitted(std::move(strokes), scale), .removed = {}})) {
            own->removeAnnotation(made);
            return qint64{0};
        }
        return made;
    }();
    read();
    if (id <= 0) {
        sayNotKept(*own, {});
        return std::nullopt;
    }
    if (m_recognition)
        m_recognition->soon(m_reader->bookId(), id);
    return id;
}

qint64 MarginNotes::writeInk(QQuickTextDocument *chapter, int start, int end, const QString &snapshot, const QVariantList &which,
                             double scale) {
    const auto all = Ink::decodeAll(QByteArray::fromBase64(snapshot.toLatin1()));
    if (!all)
        return 0;
    std::vector<Ink::Stroke> chosen;
    for (const QVariant &index : which)
        if (const auto at = index.toLongLong(); at >= 0 && std::cmp_less(at, all->size()))
            chosen.push_back(all->at(static_cast<std::size_t>(at)));
    return writeInk(plainOf(chapter), start, end, std::move(chosen), scale).value_or(0);
}

QVariantMap MarginNotes::strokes(qint64 id) const {
    const Library *own = library();
    return own && find(id) ? Ink::items(AnnotationInk::strokes(*own, id)).toVariantMap() : QVariantMap();
}

bool MarginNotes::ink(qint64 id, const Ink::Change &change) {
    Library *own = library();
    if (!own || !find(id) || !AnnotationInk::write(*own, id, change))
        return false;
    // The ink is the note: with none left, there is none.
    if (!AnnotationInk::has(*own, id)) {
        own->removeAnnotation(id);
    } else if (m_recognition) {
        m_recognition->soon(m_reader->bookId(), id);
    }
    read();
    return true;
}

bool MarginNotes::ink(qint64 id, const QVariantMap &batch) {
    const auto change = Ink::fromBatch(QJsonObject::fromVariantMap(batch));
    return change && ink(id, *change);
}

void MarginNotes::recognize(qint64 id) {
    if (m_recognition && library() && inked(id))
        m_recognition->now(m_reader->bookId(), id);
}

QString MarginNotes::readingOf(qint64 id) const { return m_recognition ? m_recognition->state(id) : QString(); }

double MarginNotes::inkBottom(QQuickItem *surface) const {
    const auto *ink = qobject_cast<InkSurface *>(surface);
    double bottom = 0;
    if (ink)
        for (const Ink::Stroke &stroke : ink->sheet().strokes())
            bottom = std::max(bottom, Ink::bounds(stroke).bottom());
    return bottom;
}

QVariantList MarginNotes::found(const QString &words) const {
    QVariantList found;
    const QString needle = words.simplified();
    if (needle.size() < 2)
        return found;
    for (const Annotation &note : m_notes)
        if (const QString said = note.note.simplified(); said.contains(needle, Qt::CaseInsensitive))
            found << QVariantMap{{"section", note.place.section},
                                 {"fraction", note.place.fraction},
                                 {"snippet", said.size() > 160 ? said.left(160) + u'…' : said},
                                 {"note", note.id}};
    return found;
}
