#include "importer.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
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
#include "curation.h"
#include "formatprovider.h"
#include "formats.h"
#include "subscribedspace.h"

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

// An ISBN as it is compared: its digits (and a closing X), whatever the
// hyphens and spaces.
[[nodiscard]] QString isbnKey(const QString &isbn) {
    QString key;
    for (const QChar c : isbn)
        if (c.isDigit() || c == 'x' || c == 'X')
            key += c.toUpper();
    return key;
}

// What an incoming book is called elsewhere: its ISBN, and what Calibre
// knows it by.
[[nodiscard]] QMap<QString, QString> identifiersOf(const BookMeta &meta, const std::optional<Calibre::Book> &calibre) {
    QMap<QString, QString> identifiers = calibre ? calibre->identifiers : QMap<QString, QString>();
    // Calibre's own number is only the same within one Calibre library;
    // its uuid is the same everywhere.
    if (calibre && !calibre->uuid.isEmpty())
        identifiers.insert("calibre", calibre->uuid);
    if (!meta.isbn.isEmpty() && !identifiers.contains("isbn"))
        identifiers.insert("isbn", meta.isbn);
    return identifiers;
}

// The books here that have no file, which a file being imported joins
// rather than becoming the same book twice.
class Fileless {
public:
    explicit Fileless(const Library &library) : m_wished(SubscribedSpace::wishedFiles(library)) {
        for (const Book &book : library.books())
            if (book.formats.isEmpty())
                m_books << book;
    }

    // The one that the file `file`, described so, is: the wishlist entry a
    // space made of this very file (#63), whatever its own details say, as
    // the space knows the book by the file's hash; else as match() finds it.
    [[nodiscard]] qint64 entryFor(const QString &file, const BookMeta &meta, const QMap<QString, QString> &identifiers) const {
        const qint64 wished = m_wished.isEmpty() ? 0 : m_wished.value(Library::hashFile(file));
        return wished != 0 ? wished : match(meta, identifiers);
    }

    // The one that is this book, if any: called the same elsewhere, else the
    // same title by the same author as tidy is sure of it (two numbers of a
    // series, or another author, are two books).
    [[nodiscard]] qint64 match(const BookMeta &meta, const QMap<QString, QString> &identifiers) const {
        const auto sameId = [](const QString &scheme, const QString &here, const QString &there) {
            return scheme == "isbn" ? !isbnKey(there).isEmpty() && isbnKey(here) == isbnKey(there)
                                    : !there.isEmpty() && here == there;
        };
        for (const Book &book : m_books)
            for (const auto &[scheme, value] : identifiers.asKeyValueRange()) {
                QString here = book.identifiers.value(scheme).toString();
                if (scheme == "isbn" && here.isEmpty())
                    here = book.meta.isbn;
                if (sameId(scheme, here, value))
                    return book.id;
            }
        const QString title = Curation::titleKey(meta.title);
        if (title.isEmpty())
            return 0;
        // Only the books with the title are compared, with this one as -1.
        QList<Book> same;
        for (const Book &book : m_books)
            if (Curation::titleKey(book.meta.title) == title)
                same << book;
        if (same.isEmpty())
            return 0;
        Book incoming;
        incoming.id = -1;
        incoming.meta = meta;
        same << incoming;
        for (const Proposal &proposal : Curation::duplicates(same))
            if (!proposal.unsure && (proposal.bookId == -1 || proposal.otherId == -1))
                return proposal.bookId == -1 ? proposal.otherId : proposal.bookId;
        return 0;
    }

    // It has a file now.
    void joined(qint64 id) {
        m_books.removeIf([id](const Book &book) { return book.id == id; });
        m_wished.removeIf([id](const QHash<QString, qint64>::iterator &wish) { return wish.value() == id; });
    }

private:
    QList<Book> m_books;
    QHash<QString, qint64> m_wished; // a file's hash, the wishlist entry of it
};

// Gives a book that had no file this one. Its details stay as they were,
// with what the file's own details add where it had none; its shelf, stars,
// identifiers and paper copy aren't touched.
[[nodiscard]] bool join(Library &library, qint64 id, const QString &file, const BookMeta &incoming, QString *error) {
    const auto entry = library.book(id);
    if (!entry || !library.addFormat(id, file, error))
        return false;
    BookMeta details = entry->meta;
    details.fillFrom(incoming);
    details.cover = entry->hasCover ? QImage() : incoming.cover;
    return library.setMeta(id, details, error);
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
    Fileless fileless(library);
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
        const QString &first = book.files.first();
        // A book any of whose files is here already is that book.
        QHash<QString, QString> hashes;
        qint64 here = 0;
        for (const QString &file : book.files.mid(1)) {
            hashes.insert(file, Library::hashFile(file));
            if (here == 0)
                here = library.bookWithHash(hashes.value(file));
        }
        Library::Added added{.id = here, .duplicate = here != 0, .error = {}};
        bool joined = false;
        if (here == 0) {
            // The first file makes the book, unless it is a book here without
            // a file; the others join it as more formats.
            const BookMeta described = meta ? *meta : Formats::readMeta(first);
            const qint64 entry = fileless.entryFor(first, described, identifiersOf(described, book.calibre));
            if (entry == 0) {
                added = library.add(first, described);
            } else if (const qint64 same = library.bookWithHash(Library::hashFile(first)); same != 0) {
                added = {.id = same, .duplicate = true, .error = {}};
            } else if (QString error; join(library, entry, first, described, &error)) {
                added = {.id = entry, .duplicate = false, .error = {}};
                joined = true;
                fileless.joined(entry);
            } else {
                added = {.id = 0, .duplicate = false, .error = error};
            }
        }
        summary.items.append({.file = first, .id = added.id, .duplicate = added.duplicate, .joined = joined, .error = added.error});
        if (!added.error.isEmpty()) {
            summary.problems << added.error;
        } else {
            added.duplicate ? ++summary.duplicates : joined ? ++summary.joined : ++summary.added;
            summary.lastId = added.id;
            QStringList have = library.book(added.id)->formats;
            for (const QString &file : book.files) {
                const QString format = Formats::extension(file);
                if ((file == first && here == 0) || have.contains(format))
                    continue;
                // A file that is here already, in this book or another, isn't
                // copied in again.
                if (library.bookWithHash(hashes.contains(file) ? hashes.value(file) : Library::hashFile(file)) != 0)
                    continue;
                if (QString error; library.addFormat(added.id, file, &error))
                    have << format;
                else
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
