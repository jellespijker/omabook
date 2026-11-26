#include "importer.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QSet>
#include <QTemporaryDir>
#include <algorithm>
#include <array>
#include <ranges>

#include "bookarchive.h"

#include "calibre.h"
#include "calibreimport.h"
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
    if (!entry)
        return false;
    if (const auto added = library.addFormat(id, file); !added) {
        *error = added.error().text();
        return false;
    }
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

// Whether a walk comes to what `entry` leads to for the first time, which is
// then noted in `seen`. Links are followed, so several names, a link back up
// among them, can lead to one folder or file; a link to itself leads nowhere.
[[nodiscard]] bool firstVisit(const QFileInfo &entry, QSet<QString> &seen) {
    const QString real = entry.canonicalFilePath();
    if (real.isEmpty() || seen.contains(real))
        return false;
    seen.insert(real);
    return true;
}

// The books in a folder: its book files, and its folders that are one book
// each; any other folder is looked into the same way. The folder itself and
// what is in it are passed over when they are in `seen` already.
void walk(const QString &folder, const QTemporaryDir &scratch, int &counter, QList<Incoming> &incoming, QStringList &problems,
          QSet<QString> &seen) {
    if (!firstVisit(QFileInfo(folder), seen))
        return;
    const QDir dir(folder);
    for (const QFileInfo &entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (entry.isFile()) {
            if (Formats::isBook(entry.filePath()) && firstVisit(entry, seen))
                incoming.append(Incoming::file(entry.filePath()));
        } else if (const auto provider = Formats::providerFor(entry.filePath())) {
            if (!firstVisit(entry, seen))
                continue;
            const QString file = packed(entry.filePath(), *provider, scratch, counter);
            if (file.isEmpty())
                problems << Failure(Failure::Code::NotOneBook, {entry.fileName()}).text();
            else
                incoming.append(Incoming::file(file));
        } else {
            walk(entry.filePath(), scratch, counter, incoming, problems, seen);
        }
    }
}

// A book of a Calibre library comes in with the files of it that can be
// taken. What can't is named, and so is a book left with none of them.
void take(const Calibre::Book &book, QList<Incoming> &incoming, ImportSummary &summary) {
    const CalibreImport::Sifted sifted = CalibreImport::sift(book);
    if (sifted.files.isEmpty()) {
        const auto why = book.files.isEmpty() ? Failure::Code::CalibreBookEmpty : Failure::Code::CalibreBookUnusable;
        summary.leftOut.insert(summary.skipped++, Failure(why, {CalibreImport::nameOf(book)}));
    } else {
        incoming.append({.files = sifted.files, .meta = book.meta, .cover = book.cover, .calibre = book});
    }
    summary.leftOut << sifted.leftOut;
}

// Why a book didn't come in. One of a Calibre library is named, and counted
// among those that didn't come across.
void failed(const Incoming &book, const QString &why, ImportSummary &summary) {
    if (!book.calibre) {
        summary.problems << why;
        return;
    }
    ++summary.skipped;
    summary.problems << Failure(Failure::Code::CalibreBookFailed, {CalibreImport::nameOf(*book.calibre), why}).text();
}

// Copies in the formats a book here lacks. `copied` is the file that has
// just gone in, when one made the book or joined it.
void addFormats(Library &library, const Incoming &book, qint64 id, const QString &copied, const QHash<QString, QString> &hashes,
                ImportSummary &summary) {
    QStringList have = library.book(id).value_or(Book{}).formats;
    for (const QString &file : book.files) {
        const QString format = Formats::extension(file);
        // A file that is here already, in this book or another, isn't copied
        // in again.
        if (file == copied || library.bookWithHash(hashes.contains(file) ? hashes.value(file) : Library::hashFile(file)) != 0)
            continue;
        // Nor is one of a format the book has: the file here stays.
        if (have.contains(format)) {
            if (book.calibre)
                summary.leftOut << Failure(Failure::Code::CalibreFileNotCopied, {CalibreImport::nameOf(*book.calibre), file});
            continue;
        }
        if (const auto r = library.addFormat(id, file); r)
            have << format;
        else
            summary.problems << r.error().text();
    }
}

// What comes with a book from Calibre besides its files: Calibre's cover,
// which a new book was to get, and the reading. Calibre can hold the same file
// under two entries. The first one's reading is taken (`taken`: a book here,
// the entry it was taken for); the second would only undo it, on every import.
void takeCalibre(Library &library, const Calibre::Book &calibre, const Library::Added &added, CalibreImport::Memory &memory,
                 QHash<qint64, QString> &taken, ImportSummary &summary) {
    const QString name = CalibreImport::nameOf(calibre);
    if (!calibre.cover.isEmpty() && !added.duplicate && QImageReader(calibre.cover).read().isNull())
        summary.leftOut << Failure(Failure::Code::CalibreCoverUnread, {name, calibre.cover});
    if (taken.contains(added.id)) {
        summary.leftOut << Failure(Failure::Code::CalibreSameBook, {name, taken.value(added.id)});
        return;
    }
    taken.insert(added.id, name);
    if (CalibreImport::takeReading(library, added.id, calibre, memory) && added.duplicate)
        ++summary.updated;
    const QString status = library.book(added.id).value_or(Book{}).status();
    if (status == "read")
        ++summary.read;
    else if (status == "reading")
        ++summary.reading;
}

[[nodiscard]] QList<Incoming> collect(const QStringList &paths, const QTemporaryDir &scratch, const ImportOptions &options,
                                      ImportSummary &summary) {
    QStringList &problems = summary.problems;
    QList<Incoming> incoming;
    int unpacked = 0;
    QSet<QString> seen; // the folders and files the walks have come to
    for (const QString &path : paths) {
        const bool calibre = options.calibre && QFileInfo(path).isDir() && Calibre::isLibrary(path);
        const auto provider = !calibre && (isArchive(path) || QFileInfo(path).isDir()) ? Formats::providerFor(path) : std::nullopt;
        if (provider) {
            // The whole folder or archive is one book of a provider's format.
            const QString file = packed(path, *provider, scratch, unpacked);
            if (file.isEmpty())
                problems << Failure(Failure::Code::NotOneBook, {QFileInfo(path).fileName()}).text();
            else
                incoming.append(Incoming::file(file));
        } else if (isArchive(path)) {
            const QStringList files = unpack(path, scratch, unpacked);
            if (files.isEmpty())
                problems << Failure(Failure::Code::NoBooksInside, {QFileInfo(path).fileName()}).text();
            for (const QString &file : files)
                incoming.append(Incoming::file(file));
        } else if (!QFileInfo(path).isDir()) {
            incoming.append(Incoming::file(path));
        } else if (options.calibre && Calibre::isLibrary(path)) {
            summary.calibreLibraries << path;
            const Calibre::Books calibre = Calibre::read(path);
            if (!calibre.error.isEmpty())
                problems << calibre.error;
            for (const Calibre::Book &book : calibre.books)
                take(book, incoming, summary);
        } else {
            walk(path, scratch, unpacked, incoming, problems, seen);
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
    CalibreImport::Memory memory(library.root());
    Fileless fileless(library);
    QHash<qint64, QString> taken; // see takeCalibre
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
            failed(book, added.error, summary);
        } else {
            added.duplicate ? ++summary.duplicates : joined ? ++summary.joined : ++summary.added;
            summary.lastId = added.id;
            addFormats(library, book, added.id, here == 0 ? first : QString(), hashes, summary);
            if (const auto &calibre = book.calibre)
                takeCalibre(library, *calibre, added, memory, taken, summary);
        }
        if (progress)
            progress(++done, incoming.size());
    }
    memory.save();
    summary.keepInLog();
    return summary;
}
