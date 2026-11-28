#include "calibreimport.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <optional>

#include "document.h"
#include "formats.h"

namespace {

// Why a file Calibre lists can't be taken; nothing for one that can.
[[nodiscard]] std::optional<Failure::Code> faultOf(const QString &path) {
    const QFileInfo file(path);
    if (!file.exists()) {
        // In a folder that may not be looked into, a file only seems to be gone.
        const QFileInfo folder(file.absolutePath());
        const bool shut = folder.isDir() && !(folder.isReadable() && folder.isExecutable());
        return shut ? Failure::Code::CalibreFileUnreadable : Failure::Code::CalibreFileMissing;
    }
    if (!Formats::isBook(path))
        return Failure::Code::CalibreFileUnsupported;
    if (!file.isFile() || !QFile(path).open(QIODevice::ReadOnly))
        return Failure::Code::CalibreFileUnreadable;
    return std::nullopt;
}

// A highlight or a note of Calibre's viewer on the passage of its words in
// its chapter, found by them when the book is first opened or its
// annotations are listed (ADR 0004); a bookmark is the start of its chapter.
[[nodiscard]] Annotation annotationOf(const Calibre::Note &note) {
    // When it was made in Calibre, not when it came across.
    Annotation annotation{.id = 0,
                          .kind = note.kind,
                          .place = {.section = note.section, .fraction = 0},
                          .text = {},
                          .note = note.note,
                          .created = note.created > 0 ? note.created : QDateTime::currentSecsSinceEpoch()};
    if (note.kind == "bookmark") {
        annotation.text = note.text;
        return annotation;
    }
    annotation.setPassage({.from = annotation.place, .to = annotation.place, .quote = note.text.trimmed(), .before = {}, .after = {}});
    annotation.color = note.color;
    // A margin note is listed by the start of its words, as one made in the reader is.
    if (note.kind == "margin-note")
        annotation.text = note.text.simplified().left(200);
    return annotation;
}

// The book's other names, Calibre's own among them, each replaced only while
// it is what the last import put there. Returns what Calibre says now.
[[nodiscard]] QJsonObject takeIdentifiers(Library &library, const Book &now, const Calibre::Book &calibre, const QJsonObject &before,
                                          bool &changed) {
    QMap<QString, QString> identifiers = calibre.identifiers;
    identifiers.insert("calibre", calibre.uuid.isEmpty() ? QString::number(calibre.id) : calibre.uuid);
    QJsonObject said;
    for (const auto &[scheme, value] : identifiers.asKeyValueRange()) {
        said.insert(scheme, value);
        const QString here = now.identifiers.value(scheme).toString();
        if (here == before.value(scheme).toString() && here != value)
            changed = library.setIdentifier(now.id, scheme, value) || changed;
    }
    return said;
}

// The place it was read to. Calibre's viewer stamps its places, and the
// later reading wins, as it does between two computers. A percentage kept
// by hand in a column has no time, so it only stands in while the book
// hasn't been read here since.
[[nodiscard]] bool takePlace(Library &library, qint64 id, const Calibre::Reading &said, double percentBefore, qint64 &placedAt) {
    const Progress here = library.progress(id);
    const bool newer = said.readAt > here.updated;
    const bool byHand =
        said.readAt == 0 && said.percent > 0 && said.percent != percentBefore && (here.updated == 0 || here.updated == placedAt);
    if (!newer && !byHand)
        return false;
    library.setProgress(id, {.place = {.section = said.section, .fraction = 0}, .percent = said.percent, .updated = said.readAt});
    placedAt = library.progress(id).updated;
    return true;
}

// The words an annotation of Calibre's is known by.
[[nodiscard]] QString wordsOf(const Annotation &annotation) {
    if (annotation.kind == "bookmark")
        return annotation.text;
    return annotation.kind == "margin-note" ? annotation.note : annotation.quote.simplified();
}

// Takes from `here` the annotation that is `made`, brought in before: of its
// kind and moment, with its words (`sameWords`), or, a highlight whose words
// were put as the book has them when it was found by them, of its moment
// alone. False when there is none.
[[nodiscard]] bool claim(QList<Annotation> &here, const Annotation &made, bool timed, bool sameWords) {
    if (!sameWords && (!timed || made.kind != "highlight"))
        return false;
    const auto same = std::ranges::find_if(here, [&](const Annotation &other) {
        return other.kind == made.kind && (!timed || other.created == made.created) && (!sameWords || wordsOf(other) == wordsOf(made));
    });
    if (same == here.end())
        return false;
    here.erase(same);
    return true;
}

// Each highlight, note and bookmark comes across once, so one removed
// here doesn't return with the next import. One the book has already, from
// an import that was cut off before it could write down what it took, is
// noted as taken rather than brought a second time (#273).
[[nodiscard]] bool takeNotes(Library &library, qint64 id, const QList<Calibre::Note> &said, QJsonArray &taken) {
    QList<Annotation> here = library.annotations(id);
    QList<Calibre::Note> others;
    // Those with their words first, so none of them is claimed by its moment alone.
    for (const Calibre::Note &note : said) {
        if (taken.contains(note.id))
            continue;
        if (claim(here, annotationOf(note), note.created > 0, true))
            taken << note.id;
        else
            others << note;
    }
    bool changed = false;
    for (const Calibre::Note &note : std::as_const(others)) {
        const Annotation made = annotationOf(note);
        if (claim(here, made, note.created > 0, false)) {
            taken << note.id;
        } else if (library.storeAnnotation(id, made).value_or(0) > 0) {
            taken << note.id;
            changed = true;
        }
    }
    return changed;
}

// Calibre's places, as far as the book goes here: one past its last section,
// as after its file was replaced by one with fewer parts, is at the last
// (#276). The book is only opened when a place about to be taken is past its start.
[[nodiscard]] Calibre::Reading withinBook(const Library &library, const Book &book, Calibre::Reading said, const QJsonArray &taken) {
    const bool placed = said.section > 0 && said.readAt > library.progress(book.id).updated;
    const auto further = [&taken](const Calibre::Note &note) { return note.section > 0 && !taken.contains(note.id); };
    if (!placed && !std::ranges::any_of(said.notes, further))
        return said;
    const QString format = Formats::openedAs(book.formats);
    const auto document = format.isEmpty() ? nullptr : Formats::open(library.formatPath(book.id, format));
    if (!document || document->count() <= 0)
        return said;
    const int last = document->count() - 1;
    said.section = std::min(said.section, last);
    for (Calibre::Note &note : said.notes)
        note.section = std::min(note.section, last);
    return said;
}

// The file's books, and over them what the journal says was remembered since.
// A line cut short, by an import that was killed as it wrote it, says nothing.
[[nodiscard]] QJsonObject remembered(const QString &file, const QString &journal) {
    QJsonObject books;
    if (QFile kept(file); kept.open(QIODevice::ReadOnly))
        books = QJsonDocument::fromJson(kept.readAll()).object();
    QFile since(journal);
    if (!since.open(QIODevice::ReadOnly))
        return books;
    while (!since.atEnd()) {
        const QJsonObject line = QJsonDocument::fromJson(since.readLine()).object();
        for (auto book = line.begin(); book != line.end(); ++book)
            books.insert(book.key(), book.value());
    }
    return books;
}

} // namespace

QString CalibreImport::nameOf(const Calibre::Book &book) {
    return book.meta.authors.isEmpty() ? book.meta.title : book.meta.title + " - " + book.meta.authors.first();
}

CalibreImport::Sifted CalibreImport::sift(const Calibre::Book &book) {
    Sifted sifted;
    for (const QString &file : book.files) {
        if (const auto fault = faultOf(file))
            sifted.leftOut << Failure(*fault, {nameOf(book), file});
        else
            sifted.files << file;
    }
    return sifted;
}

CalibreImport::Memory::Memory(const QString &libraryRoot)
    : m_file(libraryRoot + "/calibre-import.json"), m_journal(libraryRoot + "/calibre-import.journal"),
      m_books(remembered(m_file, m_journal)) {}

QJsonObject CalibreImport::Memory::of(qint64 id) const { return m_books.value(QString::number(id)).toObject(); }

void CalibreImport::Memory::remember(qint64 id, const QJsonObject &said) {
    if (of(id) == said)
        return;
    const QString key = QString::number(id);
    m_books.insert(key, said);
    m_said.insert(key, said);
    // At once: whatever ends the import, what is in the library is written down.
    // A line of its own, also after one that was cut short.
    const QByteArray line = '\n' + QJsonDocument(QJsonObject{{key, said}}).toJson(QJsonDocument::Compact) + '\n';
    if (QFile journal(m_journal); journal.open(QIODevice::WriteOnly | QIODevice::Append))
        journal.write(line); // Should it not be written, save() still has it.
}

void CalibreImport::Memory::save() {
    if (m_said.isEmpty())
        return;
    // Read again: another import may have remembered books of its own meanwhile.
    QJsonObject books = remembered(m_file, m_journal);
    for (auto book = m_said.begin(); book != m_said.end(); ++book)
        books.insert(book.key(), book.value());
    QSaveFile file(m_file);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(books).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit())
        QFile::remove(m_journal);
    m_said = {};
}

bool CalibreImport::takeReading(Library &library, qint64 id, const Calibre::Book &calibre, Memory &memory) {
    const auto now = library.book(id);
    if (!now)
        return false;
    const QJsonObject before = memory.of(id);
    const Calibre::Reading said = withinBook(library, *now, calibre.reading, before.value("notes").toArray());
    bool changed = false;

    const QString shelf = now->shelf == before.value("shelf").toString() ? said.shelf : now->shelf;
    const int rating = now->rating == before.value("rating").toInt() ? said.rating : now->rating;
    const QString finished = now->finished == before.value("finished").toString() ? said.finished : now->finished;
    if (shelf != now->shelf || rating != now->rating || finished != now->finished)
        changed = library.setReading(id, shelf, rating, finished) || changed;

    bool identified = false;
    const QJsonObject identifiersSaid = takeIdentifiers(library, *now, calibre, before.value("identifiers").toObject(), identified);
    qint64 placedAt = static_cast<qint64>(before.value("progress_at").toDouble());
    const bool placed = takePlace(library, id, said, before.value("percent").toDouble(), placedAt);
    QJsonArray notes = before.value("notes").toArray();
    const bool noted = takeNotes(library, id, said.notes, notes);
    changed = changed || identified || placed || noted;

    memory.remember(id, QJsonObject{{"shelf", said.shelf},
                                    {"rating", said.rating},
                                    {"finished", said.finished},
                                    {"identifiers", identifiersSaid},
                                    {"percent", said.percent},
                                    {"progress_at", static_cast<double>(placedAt)},
                                    {"notes", notes}});
    return changed;
}
