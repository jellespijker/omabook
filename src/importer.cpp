#include "importer.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <ranges>

#include "bookarchive.h"

#include "calibre.h"
#include "formatprovider.h"
#include "formats.h"

using namespace Qt::StringLiterals;

namespace {

// One book to bring in: its files, and its details when something already
// knows them better than the files do.
struct Incoming {
    QStringList files;
    std::optional<BookMeta> meta;
    QString cover;
    // The book as Calibre has it, when it comes from a Calibre library.
    std::optional<Calibre::Book> calibre;

    // A book that is one file, of which nothing more is known.
    static Incoming file(const QString &path) { return {.files = {path}, .meta = std::nullopt, .cover = {}, .calibre = std::nullopt}; }
};

// What Calibre said of each book the last time it was imported, kept beside
// the library so the next import can tell a value Calibre changed from one
// the user changed here.
class CalibreMemory {
public:
    explicit CalibreMemory(const QString &libraryRoot) : m_file(libraryRoot + "/calibre-import.json") {
        QFile file(m_file);
        if (file.open(QIODevice::ReadOnly))
            m_books = QJsonDocument::fromJson(file.readAll()).object();
    }

    [[nodiscard]] QJsonObject of(qint64 id) const { return m_books.value(QString::number(id)).toObject(); }
    void remember(qint64 id, const QJsonObject &said) {
        if (of(id) == said)
            return;
        m_books.insert(QString::number(id), said);
        m_changed = true;
    }
    void save() {
        if (!m_changed)
            return;
        QSaveFile file(m_file);
        file.setDirectWriteFallback(false);
        if (file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(m_books).toJson(QJsonDocument::Compact)) > 0)
            file.commit();
        m_changed = false;
    }

private:
    QString m_file;
    QJsonObject m_books;
    bool m_changed = false;
};

// Brings what Calibre says of a book's reading into the library. A value is
// replaced only while it still is what the last import put there (or nothing,
// for a book never imported from Calibre), so a shelf or a star changed in
// Omabook since stays as the user left it. True when something changed.
[[nodiscard]] bool takeReading(Library &library, qint64 id, const Calibre::Book &calibre, CalibreMemory &memory) {
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

    QMap<QString, QString> identifiers = calibre.identifiers;
    identifiers.insert("calibre", calibre.uuid.isEmpty() ? QString::number(calibre.id) : calibre.uuid);
    const QJsonObject identifiersBefore = before.value("identifiers").toObject();
    QJsonObject identifiersSaid;
    for (const auto &[scheme, value] : identifiers.asKeyValueRange()) {
        identifiersSaid.insert(scheme, value);
        const QString here = now->identifiers.value(scheme).toString();
        if (here == identifiersBefore.value(scheme).toString() && here != value)
            changed = library.setIdentifier(id, scheme, value) || changed;
    }

    // The place it was read to. Calibre's viewer stamps its places, and the
    // later reading wins, as it does between two computers. A percentage kept
    // by hand in a column has no time, so it only stands in while the book
    // hasn't been read here since.
    const Progress here = library.progress(id);
    qint64 placedAt = qint64(before.value("progress_at").toDouble());
    const bool newer = said.readAt > here.updated;
    const bool byHand = said.readAt == 0 && said.percent > 0 && said.percent != before.value("percent").toDouble()
                        && (here.updated == 0 || here.updated == placedAt);
    if (newer || byHand) {
        library.setProgress(id, {.section = said.section, .percent = said.percent, .updated = said.readAt});
        placedAt = library.progress(id).updated;
        changed = true;
    }

    // Each highlight and bookmark comes across once, so one removed here
    // doesn't return with the next import.
    QJsonArray notes = before.value("notes").toArray();
    for (const Calibre::Note &note : said.notes) {
        if (notes.contains(note.id))
            continue;
        const Annotation annotation{.id = 0, .kind = note.kind, .section = note.section, .fraction = 0, .text = note.text, .note = note.note};
        if (library.addAnnotation(id, annotation) > 0) {
            notes << note.id;
            changed = true;
        }
    }

    memory.remember(id, QJsonObject{{"shelf", said.shelf}, {"rating", said.rating}, {"finished", said.finished},
                                     {"identifiers", identifiersSaid}, {"percent", said.percent},
                                     {"progress_at", double(placedAt)}, {"notes", notes}});
    return changed;
}

[[nodiscard]] bool isArchive(const QString &path) {
    static constexpr std::array suffixes{"zip"_L1, "rar"_L1, "7z"_L1, "tar"_L1, "tgz"_L1};
    return std::ranges::contains(suffixes, QFileInfo(path).suffix().toLower()) && !Formats::isBook(path);
}

// Books are often passed around packed. The ones inside an archive are
// unpacked beside each other in `scratch`, which lives as long as the import.
[[nodiscard]] QStringList unpack(const QString &archivePath, const QTemporaryDir &scratch, int &counter) {
    QStringList files;
    const Archive archive(archivePath);
    for (const QString &entry : archive.entries()) {
        if (!Formats::isBook(entry))
            continue;
        const QString folder = scratch.filePath(QString::number(++counter));
        const QString file = folder + "/" + QFileInfo(entry).fileName();
        QFile out(file);
        const QByteArray data = archive.read(entry);
        if (QDir().mkpath(folder) && !data.isNull() && out.open(QIODevice::WriteOnly) && out.write(data) == data.size())
            files << file;
    }
    return files;
}

// A folder, or a plain archive, that a format provider says is one book (a
// folder of Markdown chapters, say) is packed into one file of that format,
// since a library holds a book as a file. Empty when it couldn't be packed.
[[nodiscard]] QString packed(const QString &path, const FormatProvider &provider, const QTemporaryDir &scratch, int &counter) {
    const BookSource source(path);
    const QString folder = scratch.filePath(QString::number(++counter));
    const QString file = folder + "/" + source.name() + "." + provider.extensions.value(0);
    return QDir().mkpath(folder) && source.pack(file) ? file : QString();
}

// The books in a folder: its book files, and its folders that are one book
// each; any other folder is looked into the same way.
void walk(const QString &folder, const QTemporaryDir &scratch, int &counter, QList<Incoming> &incoming, QStringList &problems) {
    const QDir dir(folder);
    for (const QFileInfo &entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (entry.isFile()) {
            if (Formats::isBook(entry.filePath()))
                incoming.append(Incoming::file(entry.filePath()));
        } else if (const auto provider = Formats::providerFor(entry.filePath())) {
            const QString file = packed(entry.filePath(), *provider, scratch, counter);
            if (file.isEmpty())
                problems << entry.fileName() + " couldn't be made into one book.";
            else
                incoming.append(Incoming::file(file));
        } else {
            walk(entry.filePath(), scratch, counter, incoming, problems);
        }
    }
}

[[nodiscard]] QList<Incoming> collect(const QStringList &paths, const QTemporaryDir &scratch, const ImportOptions &options,
                                      ImportSummary &summary) {
    QStringList &problems = summary.problems;
    QList<Incoming> incoming;
    int unpacked = 0;
    for (const QString &path : paths) {
        const bool calibre = options.calibre && QFileInfo(path).isDir() && Calibre::isLibrary(path);
        const auto provider = !calibre && (isArchive(path) || QFileInfo(path).isDir()) ? Formats::providerFor(path) : std::nullopt;
        if (provider) {
            // The whole folder or archive is one book of a provider's format.
            const QString file = packed(path, *provider, scratch, unpacked);
            if (file.isEmpty())
                problems << QFileInfo(path).fileName() + " couldn't be made into one book.";
            else
                incoming.append(Incoming::file(file));
        } else if (isArchive(path)) {
            const QStringList files = unpack(path, scratch, unpacked);
            if (files.isEmpty())
                problems << "There are no books inside " + QFileInfo(path).fileName() + ".";
            for (const QString &file : files)
                incoming.append(Incoming::file(file));
        } else if (!QFileInfo(path).isDir()) {
            incoming.append(Incoming::file(path));
        } else if (options.calibre && Calibre::isLibrary(path)) {
            summary.calibreLibraries << path;
            const Calibre::Books calibre = Calibre::read(path);
            if (!calibre.error.isEmpty())
                problems << calibre.error;
            for (const Calibre::Book &book : calibre.books) {
                const QStringList files = book.files | std::views::filter(Formats::isBook) | std::ranges::to<QStringList>();
                if (!files.isEmpty())
                    incoming.append({.files = files, .meta = book.meta, .cover = book.cover, .calibre = book});
            }
        } else {
            walk(path, scratch, unpacked, incoming, problems);
        }
    }
    return incoming;
}

} // namespace

ImportSummary importBooks(Library &library, const QStringList &paths, const std::function<void(int, int)> &progress,
                          const ImportOptions &options) {
    ImportSummary summary;
    const QTemporaryDir scratch;
    const QList<Incoming> incoming = collect(paths, scratch, options, summary);
    CalibreMemory memory(library.root());
    // Calibre can hold the same file under two entries. The first one's
    // reading is taken; the second would only undo it, on every import.
    QSet<qint64> taken;
    int done = 0;
    for (const Incoming &book : incoming) {
        std::optional<BookMeta> meta = book.meta;
        if (meta && !book.cover.isEmpty()) {
            QImageReader cover(book.cover);
            cover.setAutoTransform(true);
            meta->cover = cover.read();
        }
        // The first file makes the book; the others join it as more formats.
        const Library::Added added = library.add(book.files.first(), meta);
        summary.items.append({.file = book.files.first(), .id = added.id, .duplicate = added.duplicate, .error = added.error});
        if (!added.error.isEmpty()) {
            summary.problems << added.error;
        } else {
            added.duplicate ? ++summary.duplicates : ++summary.added;
            summary.lastId = added.id;
            const QStringList have = library.book(added.id)->formats;
            for (const QString &file : book.files.mid(1)) {
                QString error;
                if (!have.contains(Formats::extension(file)) && !library.addFormat(added.id, file, &error))
                    summary.problems << error;
            }
            if (book.calibre && !taken.contains(added.id)) {
                taken.insert(added.id);
                if (takeReading(library, added.id, *book.calibre, memory) && added.duplicate)
                    ++summary.updated;
                const QString status = library.book(added.id)->status();
                if (status == "read")
                    ++summary.read;
                else if (status == "reading")
                    ++summary.reading;
            }
        }
        if (progress)
            progress(++done, incoming.size());
    }
    memory.save();
    return summary;
}
