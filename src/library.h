#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <expected>
#include <optional>

#include "bookmeta.h"

class QSqlQuery;

struct Book {
    qint64 id = 0;
    BookMeta meta;          // without the cover picture; see Library::coverPath
    QString path;           // the book's folder, relative to the library
    QStringList formats;    // extensions, in the order the reader prefers them
    qint64 added = 0;       // seconds since the epoch
    bool hasCover = false;
    double percent = 0;     // how far it has been read, 0 to 100
    qint64 lastRead = 0;
    // A book need not be a file. One with formats is digital; `paper` says
    // there is a copy on a real shelf; one with neither is only known of (a
    // book read elsewhere, or one to read some day).
    bool paper = false;
    // What the user, or a site they keep their reading on, says of it:
    // "to-read", "reading", "read", or empty when nobody said.
    QString shelf;
    int rating = 0;         // 1 to 5 stars, 0 for none
    QString finished;       // the day it was finished, as an ISO date, when known
    // What the book is called elsewhere: {"goodreads": "4671", "calibre": "12"}.
    QJsonObject identifiers;

    // "to-read", "reading" or "read": the shelf when there is one, else how
    // far the book was read here; empty for a book nothing is known of.
    QString status() const;
    // "digital", "paper", "both", or "none" for a book that is only known of.
    QString held() const;

    // Whether every word of a search matches: bare words look in the title,
    // authors, series and tags; "author:", "title:", "series:", "tag:",
    // "format:", "lang:", "status:" (to-read, reading, read, unread),
    // "is:" (digital, paper, wishlist) and "rating:" look in one field.
    bool matches(const QString &query) const;
    QJsonObject toJson() const;
};

struct Progress {
    int section = 0;
    double fraction = 0;    // how far into the section, 0 to 1
    double percent = 0;     // how far into the book, 0 to 100
    qint64 updated = 0;     // when it was read to here, seconds since the epoch
};

// One of a book's files.
struct FormatFile {
    QString format;
    QString path;
    QString hash;           // SHA-256 of the file, which is how two libraries know a book is the same
    qint64 size = 0;
};

// The AI's running notes on a book up to a place: what the next catch-up
// builds on, so only what was read since has to be sent.
struct Recap {
    int section = 0;
    double fraction = 0;
    QString text;
    qint64 updated = 0;
};

// The AI's notes on an earlier book of a series.
struct SeriesNotes {
    QString title;
    QString text;
    // Whether the text is only what the book is about (its description, or
    // one looked up) and not notes on what was read.
    bool blurb = false;
};

struct Annotation {
    qint64 id = 0;
    QString kind;           // "bookmark" or "highlight"
    int section = 0;
    double fraction = 0;
    QString text;
    QString note;
    qint64 created = 0;
};

// A library is a folder: one SQLite file and, per book, a folder holding the
// book's files and cover under "Author/Title (id)". Files are copied in, so
// the originals are never touched, and the folder can be backed up or synced
// as it is. The window and the command line both work through this class and
// each thread opens its own, which SQLite's write-ahead log keeps in step.
class Library {
public:
    explicit Library(const QString &root);
    ~Library();
    Library(const Library &) = delete;
    Library &operator=(const Library &) = delete;

    // Creates the folder and the database when they don't exist yet.
    bool open(QString *error = nullptr);
    QString root() const noexcept { return m_root; }
    QString databasePath() const;
    // Where copies of the database are kept: one is made each time an
    // Omabook update changes the library's format, before anything changes.
    QString backupFolder() const;
    // The format the database is in, counted from 1; it only ever goes up.
    int formatVersion() const;

    struct Added {
        qint64 id = 0;
        bool duplicate = false; // the very same file was already here
        QString error;          // English; empty when it worked
    };
    // Copies a book in. Its details are what the file says about itself,
    // unless the caller already knows them (another library does).
    Added add(const QString &file, const std::optional<BookMeta> &known = std::nullopt);
    // Adds a book that has no file: one on a real shelf (`paper`), or one
    // that is only known of. It can be given files later with addFormat.
    Added addEntry(const BookMeta &meta, bool paper);
    // Adds another format of a book already here, replacing one of the same kind.
    bool addFormat(qint64 id, const QString &file, QString *error = nullptr);
    // Moves the book's folder to the trash and forgets it.
    bool remove(qint64 id, QString *error = nullptr);

    QList<Book> books(const QString &query = {}) const;
    std::optional<Book> book(qint64 id) const;

    // Replaces the book's details; a cover in `meta` replaces the cover too.
    bool setMeta(qint64 id, const BookMeta &meta, QString *error = nullptr);
    bool setCover(qint64 id, const QImage &cover);
    // What is said of the book's reading: its shelf ("" to leave it to how
    // far it was read here), stars and the day it was finished.
    bool setReading(qint64 id, const QString &shelf, int rating, const QString &finished);
    bool setPaper(qint64 id, bool paper);
    // What the book is called on a site; an empty value forgets it.
    bool setIdentifier(qint64 id, const QString &scheme, const QString &value);
    // The book a site calls this, or 0.
    qint64 bookWithIdentifier(const QString &scheme, const QString &value) const;
    QString coverPath(qint64 id) const;
    // The file of one format, or of the one the reader prefers when empty.
    QString formatPath(qint64 id, const QString &format = {}) const;

    QList<FormatFile> formatFiles(qint64 id) const;
    // The book that holds a file with this hash, or 0.
    qint64 bookWithHash(const QString &hash) const;
    // Whether a file with this hash was removed from this library, so a sync
    // doesn't bring back what the user threw away.
    bool wasRemoved(const QString &hash) const;
    QStringList removedHashes() const;
    static QString hashFile(const QString &path);

    Progress progress(qint64 id) const;
    // Stamped with now, unless the progress carries its own time.
    void setProgress(qint64 id, const Progress &progress);

    // The AI's notes on a book, by kind: "recap" (the story so far) and
    // "cast" (who is who), each up to the place it was written at.
    Recap notes(qint64 id, const QString &kind) const;
    void setNotes(qint64 id, const QString &kind, const Recap &notes);
    // The books that come before this one in its series, in reading order.
    QList<Book> earlierInSeries(qint64 id) const;
    // What is known of those books: notes of this kind, or of any kind
    // where there are none, and failing that what the book is about (a
    // description looked up for it, kept as notes of the kind "blurb", or
    // its own). Books nothing is known of are left out.
    QList<SeriesNotes> seriesNotes(qint64 id, const QString &kind) const;
    Recap recap(qint64 id) const { return notes(id, "recap"); }
    void setRecap(qint64 id, const Recap &recap) { setNotes(id, "recap", recap); }

    // Folds one book into another: the formats `keep` lacks move over, and
    // what is left of `other` goes to the trash.
    bool merge(qint64 keep, qint64 other, QString *error = nullptr);

    qint64 addAnnotation(qint64 bookId, const Annotation &annotation);
    void removeAnnotation(qint64 annotationId);
    QList<Annotation> annotations(qint64 bookId) const;

    // How often what the shelf shows (books, their files, their reading
    // places) and the knowledge graph have changed, by anyone. Compared
    // with an earlier answer they say what changed since.
    struct Changes {
        qint64 books = 0, graph = 0;
        bool operator==(const Changes &) const = default;
    };
    [[nodiscard]] Changes changes() const;

    // Many writes as one transaction, for as long as it lives: much faster
    // than each on its own, and seen by others as one change. A batch
    // inside another joins it.
    class Batch {
    public:
        explicit Batch(const Library &library);
        ~Batch();
        Batch(const Batch &) = delete;
        Batch &operator=(const Batch &) = delete;

    private:
        const Library &m_library;
    };

private:
    // The knowledge graph is kept in the same database.
    friend class Graph;
    mutable int m_batches = 0;
    QString bookFolder(qint64 id) const;
    // Keeps a copy of the database as it is, or says why it couldn't.
    [[nodiscard]] std::expected<void, QString> backUp(int schema) const;
    // Runs a statement that changes something; false when it couldn't.
    bool exec(const QString &sql, const QVariantList &values = {}) const;
    // Runs a statement that asks something; what it found is read with next().
    [[nodiscard]] QSqlQuery query(const QString &sql, const QVariantList &values = {}) const;
    // Adds a row: its id, or what the database said was wrong.
    [[nodiscard]] std::expected<qint64, QString> insert(const QString &sql, const QVariantList &values) const;

    QString m_root;
    QString m_connection;
};
