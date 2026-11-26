#include "calibreimport.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <optional>

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

// Each highlight, note and bookmark comes across once, so one removed
// here doesn't return with the next import.
[[nodiscard]] bool takeNotes(Library &library, qint64 id, const QList<Calibre::Note> &said, QJsonArray &taken) {
    bool changed = false;
    for (const Calibre::Note &note : said) {
        if (taken.contains(note.id))
            continue;
        if (library.storeAnnotation(id, annotationOf(note)).value_or(0) > 0) {
            taken << note.id;
            changed = true;
        }
    }
    return changed;
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

CalibreImport::Memory::Memory(const QString &libraryRoot) : m_file(libraryRoot + "/calibre-import.json") {
    QFile file(m_file);
    if (file.open(QIODevice::ReadOnly))
        m_books = QJsonDocument::fromJson(file.readAll()).object();
}

QJsonObject CalibreImport::Memory::of(qint64 id) const { return m_books.value(QString::number(id)).toObject(); }

void CalibreImport::Memory::remember(qint64 id, const QJsonObject &said) {
    if (of(id) == said)
        return;
    m_books.insert(QString::number(id), said);
    m_changed = true;
}

void CalibreImport::Memory::save() {
    if (!m_changed)
        return;
    QSaveFile file(m_file);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(m_books).toJson(QJsonDocument::Compact)) > 0)
        file.commit();
    m_changed = false;
}

bool CalibreImport::takeReading(Library &library, qint64 id, const Calibre::Book &calibre, Memory &memory) {
    const auto now = library.book(id);
    if (!now)
        return false;
    const QJsonObject before = memory.of(id);
    const Calibre::Reading &said = calibre.reading;
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
