#pragma once

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

#include "bookmeta.h"
#include "bookplace.h"
#include "person.h"

class QSqlQuery;
class Statements;

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
    // When its details (the fields of `meta`, its cover and identifiers)
    // last changed, and when what is said of its reading (shelf, stars,
    // finished day, paper) did, in seconds since the epoch; 0 when not
    // known, as for every book of a library from before these were kept.
    // A sync takes the newer details, and leaves a book alone that changed
    // after the other computer removed it.
    qint64 detailsAt = 0;
    qint64 readingAt = 0;

    // "to-read", "reading" or "read": the shelf when there is one, else how
    // far the book was read here; empty for a book nothing is known of.
    QString status() const;
    // "digital", "paper", "both", or "none" for a book that is only known of.
    QString held() const;

    // Whether every word of a search matches: bare words look in the title,
    // authors, series and tags; "author:", "title:", "series:", "tag:",
    // "format:", "lang:", "publisher:", "about:" (the description), "status:"
    // (to-read, reading, read, unread), "is:" (digital, paper, wishlist) and
    // "rating:" look in one field. "genre:" looks in the tags, where a
    // book's subjects are, and knows "fiction" and "nonfiction" as the many
    // tags each is spelled as ("Fantasy", "Thrillers", "History",
    // "Programming"). "quotes" keep words together (tag:"science fiction"),
    // a word with "-" in front must not match, and "OR" between words
    // matches either side: `genre:fiction OR series:Dune -tag:poetry`.
    bool matches(const QString &query) const;
    // What else a search may find a book by, beyond its details: asked of a
    // bare word the details don't hold (with `only` false), and of the value
    // of "knows:" (with `only` true), which looks in that and nothing else.
    // The knowledge graph is what the window's search and `omabook search`
    // give here (knowledgesearch.h); without it "knows:" finds nothing.
    using Knows = std::function<bool(const Book &book, const QString &word, bool only)>;
    bool matches(const QString &query, const Knows &knows) const;
    // What is wrong with a search as a collection's rule, in English: a
    // field it doesn't know ("auther:"), a quote left open, or "knows:",
    // which is for a search and not a rule. Empty when it is fine.
    static QString searchProblem(const QString &query);
    QJsonObject toJson() const;
};

// Where a book was read to.
struct Progress {
    BookPlace place;
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
    BookPlace place; // what was read up to
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

// A bookmark (a place), a highlight (a passage) or a margin note (words
// about a passage, its `note`, #82), as the annotations table keeps it. A passage's start is `place`; where it ends, its words and those
// around them are the rest of its Passage (#80), found again by them when the
// book's file changes (AnnotationAnchors). A highlight from before passages
// is a place and its words (`text`), until its book is opened.
struct Annotation {
    qint64 id = 0;
    QString kind; // "bookmark", "highlight" or "margin-note"
    BookPlace place;
    QString text;
    QString note;
    qint64 created = 0;
    BookPlace to; // where its passage ends; a bookmark's is its place
    QString quote, before, after;
    QString anchored;      // the hash of the file its places were found in; empty before they were
    double confidence = 1; // how sure that place is (FoundPassage::confidence)
    bool lost = false;     // not found in that file: kept for the reader to place again or remove

    [[nodiscard]] Passage passage() const;
    void setPassage(const Passage &passage);
    // Whether it is on a passage found by its words: not a bookmark's place.
    [[nodiscard]] bool hasPassage() const;
    // What the plugins and the trash keep of it: its place, kind, words and
    // note, and its passage when it has one; read back the same.
    [[nodiscard]] QJsonObject toJson(QJsonObject alongside = {}) const;
    [[nodiscard]] static Annotation fromJson(const QJsonObject &object);
};

// A named part of the library, named as its owner likes: a saved
// selection, not a place. A book can be in several, and nothing is copied or
// moved to put it in one. What is in it is what its rule (a search, as
// Book::matches reads it) finds, and the books put in or taken out by hand,
// which win over the rule; so a new book lands in every collection whose
// rule finds it, the moment it is added. Collections nest: "Studie" can
// hold "Python" and "Hydraulica", written "Studie/Python", and holds
// every book they hold besides its own, unless it takes one out by hand.
// Collections are kept in the library's database, so they go wherever the
// library goes.
struct Collection {
    qint64 id = 0;
    qint64 parent = 0;      // the collection it is part of; 0 for one at the top
    QString name;
    QString path;           // its name under its parents': "Studie/Python"
    int depth = 0;          // 0 at the top, 1 inside one, and so on
    QString rule;
    QSet<qint64> included, excluded; // by hand
    // What the collections inside it select, at every depth: it holds that too.
    struct Part {
        QString rule;
        QSet<qint64> included, excluded;
        [[nodiscard]] bool holds(const Book &book) const;
    };
    QList<Part> within;
    bool hasChildren = false;
    // How the shelf shows it ({"view", "group", "sort", "reversed"}) and the
    // reading format options of its own (readingformat.h), which lie between
    // the library's defaults and a book's own.
    QJsonObject shelf, format;
    qint64 updated = 0;     // seconds since the epoch; a sync keeps the newer
    bool removed = false;   // removed, remembered so a sync doesn't bring it back

    [[nodiscard]] bool contains(const Book &book) const;
    [[nodiscard]] QList<Book> of(const QList<Book> &books) const;
    // {"id", "name", "path", "parent", "rule", "included": [ids], "excluded": [ids], "shelf", "format"}.
    [[nodiscard]] QJsonObject toJson() const;
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
    // Opened only as it is: a library in another format than this Omabook's
    // is left alone rather than brought up to date. For upkeep that runs on
    // every library the user named (Folio's folders after each command),
    // which must never change a library's format in passing.
    void keepFormat() noexcept { m_keepFormat = true; }
    // Told, before it starts, when opening brings the library from an older
    // format up to date (#54): a line to show while that runs, as a command
    // prints it on standard error (LibraryMigrations::sayOnStandardError).
    void onBringingUpToDate(std::function<void(const QString &)> say) { m_bringingUpToDate = std::move(say); }
    QString root() const noexcept { return m_root; }
    QString databasePath() const;
    // Where copies of the database are kept: one is made each time an
    // Omabook update changes the library's format, before anything changes.
    QString backupFolder() const;
    // The format the database is in, counted from 1; it only ever goes up.
    int formatVersion() const;
    // Who signs what this library writes to its change log (ADR 0001,
    // *People and keys*): the user's person, kept per user and read once.
    // Nothing when there can be none; the operations then go unsigned.
    [[nodiscard]] const Person *person() const { return m_person.get(m_root); }
    // Where the person is kept: per user, or a test's or a pairing's own.
    // Held as person() holds it: what pairing changes, of a library read or written.
    [[nodiscard]] PersonFile &personFile() const noexcept { return m_person; }
    void usePersonFile(const QString &file) { m_person = PersonFile(file); }

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
    // Moves the book's folder to the trash and forgets it. Its files are
    // remembered as removed (wasRemoved), now or `at` (when it was removed on
    // another computer), but for one another book here still holds, as the
    // book kept by a merge does.
    // Nothing, or why it couldn't be removed.
    [[nodiscard]] std::expected<void, QString> remove(qint64 id, std::optional<qint64> at = std::nullopt);

    QList<Book> books(const QString &query = {}) const;
    std::optional<Book> book(qint64 id) const;

    // Replaces the book's details; a cover in `meta` replaces the cover too.
    // When something changed, the details are stamped with now; given `at`
    // (details another computer had at that time), they are stamped with
    // `at` whether or not anything changed, so a sync doesn't send them back.
    bool setMeta(qint64 id, const BookMeta &meta, QString *error = nullptr, std::optional<qint64> at = std::nullopt);
    bool setCover(qint64 id, const QImage &cover, std::optional<qint64> at = std::nullopt);
    // Stamps the book's details with this time and changes nothing else.
    bool setDetailsAt(qint64 id, qint64 at);
    // What is said of the book's reading: its shelf ("" to leave it to how
    // far it was read here), stars and the day it was finished. A change is
    // stamped with now (Book::readingAt).
    bool setReading(qint64 id, const QString &shelf, int rating, const QString &finished);
    bool setPaper(qint64 id, bool paper);
    // How the book is set on the page where it isn't as the defaults are:
    // only the reading format options it has of its own (readingformat.h),
    // empty when it follows the defaults. Setting an empty object, or one
    // with nothing in it that is an option, puts it back to the defaults.
    QJsonObject bookFormat(qint64 id) const;
    bool setBookFormat(qint64 id, const QJsonObject &options);
    // What the book is called on a site; an empty value forgets it. A
    // change stamps the details with now, or with `at` when given.
    bool setIdentifier(qint64 id, const QString &scheme, const QString &value, std::optional<qint64> at = std::nullopt);
    // The book a site calls this, or 0.
    qint64 bookWithIdentifier(const QString &scheme, const QString &value) const;
    QString coverPath(qint64 id) const;
    // The file of one format, or of the one the reader prefers when empty.
    QString formatPath(qint64 id, const QString &format = {}) const;

    QList<FormatFile> formatFiles(qint64 id) const;
    // The book that holds a file with this hash, or 0.
    qint64 bookWithHash(const QString &hash) const;
    // What names a book that has no file across two libraries, where a
    // file's hash can't: "entry:<scheme>=<value>" for each of its
    // identifiers and its ISBN, and "entry:title=<title>|<authors>" with
    // both folded to lower case and letters and digits only. Two books are
    // the same when any key is the same.
    static QStringList entryKeys(const QJsonObject &identifiers, const QString &isbn, const QString &title,
                                 const QStringList &authors);
    static QStringList entryKeys(const Book &book);
    // Whether a file with this hash, or a book without a file of one of its
    // entryKeys, was removed from this library, so a sync doesn't bring back
    // what the user threw away.
    bool wasRemoved(const QString &hash) const;
    // When it was removed, in seconds since the epoch; none when it wasn't.
    std::optional<qint64> removedAt(const QString &hash) const;
    QStringList removedHashes() const;
    // Every removal, with its time.
    struct Removal {
        QString key;        // a file's hash, or an entry key
        qint64 at = 0;
    };
    QList<Removal> removals() const;
    static QString hashFile(const QString &path);

    Progress progress(qint64 id) const;
    // Stamped with now, unless the progress carries its own time.
    void setProgress(qint64 id, const Progress &progress);

    // The AI's notes on a book, by kind: "recap" (the story so far) and
    // "cast" (who is who), each up to the place it was written at.
    Recap notes(qint64 id, const QString &kind) const;
    void setNotes(qint64 id, const QString &kind, const Recap &notes);
    // The books that come before this one in its series, in reading order,
    // of those on its side of the library (BookSides).
    QList<Book> earlierInSeries(qint64 id) const;
    // What is known of those books: notes of this kind, or of any kind
    // where there are none, and failing that what the book is about (a
    // description looked up for it, kept as notes of the kind "blurb", or
    // its own). Books nothing is known of are left out.
    QList<SeriesNotes> seriesNotes(qint64 id, const QString &kind) const;
    Recap recap(qint64 id) const { return notes(id, "recap"); }
    void setRecap(qint64 id, const Recap &recap) { setNotes(id, "recap", recap); }

    // Folds one book into another: the formats and cover `keep` lacks move
    // over; its identifiers are both books' (keep's value where both have
    // one); it is paper when either is, and keeps its own shelf, stars and
    // finishing day where it has them (of two shelves, the one further
    // along); the further reading place wins, with the AI's notes and graph
    // places that go with it; bookmarks, highlights, the reading log and
    // the other book's node in the knowledge graph move over. What is left
    // of `other` goes to the trash.
    bool merge(qint64 keep, qint64 other, QString *error = nullptr);

    // The collections, as a tree read from the top: each one followed by
    // those inside it, in the order they are listed in. With `withRemoved`
    // also the removed ones a sync needs to know of.
    [[nodiscard]] QList<Collection> collections(bool withRemoved = false) const;
    [[nodiscard]] std::optional<Collection> collection(qint64 id) const;
    // By its path ("Studie/Python"), its name where only one has it, or
    // its number; whatever the case.
    [[nodiscard]] std::optional<Collection> collectionNamed(const QString &name) const;
    // A new collection, last among those beside it; its number, or why it
    // can't be. A name with "/" in it is a path, and the collections it goes
    // through are made where they aren't there yet; `parent` puts it inside
    // another. A collection of that name that was removed comes back, empty.
    std::expected<qint64, QString> addCollection(const QString &name, const QString &rule = {}, qint64 parent = 0);
    bool renameCollection(qint64 id, const QString &name, QString *error = nullptr);
    bool setCollectionRule(qint64 id, const QString &rule, QString *error = nullptr);
    // Puts a collection inside another (0 for the top), never inside itself.
    bool setCollectionParent(qint64 id, qint64 parent, QString *error = nullptr);
    // Takes a collection away; its books stay in the library, and the
    // collections inside it move up to where it was.
    bool removeCollection(qint64 id);
    // Puts a book in a collection by hand, or takes it out, whatever the
    // rule says; only what differs from the rule is written down.
    bool putInCollection(qint64 id, qint64 bookId, bool in);
    // The book is in the collection as its rule says again.
    bool followRule(qint64 id, qint64 bookId);
    bool setCollectionShelf(qint64 id, const QJsonObject &shelf);
    bool setCollectionFormat(qint64 id, const QJsonObject &options);
    // Writes everything of a collection as it is given, its time and its
    // parent too: what a sync brings from another computer. Given its `id`,
    // that one; else the one of that name inside that parent, or a new one.
    // Its number, or none when it can't be written.
    std::optional<qint64> storeCollection(const Collection &given);
    // The collections a book is in, in their order.
    [[nodiscard]] QList<Collection> collectionsOf(const Book &book) const;
    // The collection whose reading format a book is set with: `preferred`
    // (the one it was opened from) when the book is in it and it has a
    // format of its own, else the first such one it is in. None when no
    // collection of the book's has a format.
    [[nodiscard]] std::optional<Collection> formatCollection(qint64 bookId, qint64 preferred = 0) const;

    // A book's bookmarks and highlights (libraryannotations.cpp). A new one
    // is made now; its number, or 0 when it can't be written.
    qint64 addAnnotation(qint64 bookId, const Annotation &annotation);
    // Writes one as it is given, its time too: what a sync brings from
    // another computer. Given its `id`, that one; else a new one. Its number,
    // or none when it can't be written.
    std::optional<qint64> storeAnnotation(qint64 bookId, const Annotation &annotation);
    // Other words to know one by; whether there is one of that number.
    bool renameAnnotation(qint64 annotationId, const QString &text);
    void removeAnnotation(qint64 annotationId);
    // Those of one kind ("bookmark", "highlight", "margin-note"), or all of them.
    QList<Annotation> annotations(qint64 bookId, const QString &kind = {}) const;

    // How often what the shelf shows (books, their files, their reading
    // places), the knowledge graph and the books' own reading formats have
    // changed, by anyone. Compared
    // with an earlier answer they say what changed since.
    struct Changes {
        qint64 books = 0, graph = 0, format = 0;
        bool operator==(const Changes &) const = default;
    };
    [[nodiscard]] Changes changes() const;

    // Many writes as one transaction, for as long as it lives: much faster
    // than each on its own, and seen by others as one change. A batch
    // inside another joins it.
    class Batch {
    public:
        // When the database becomes the batch's alone: at its first write, or
        // at once, so what it reads before it writes can't change under it
        // (another program that wants it waits until the batch is done).
        enum class Lock : std::uint8_t { AtFirstWrite, AtOnce };
        explicit Batch(const Library &library, Lock lock = Lock::AtFirstWrite);
        ~Batch();
        Batch(const Batch &) = delete;
        Batch &operator=(const Batch &) = delete;
        // Whether the transaction could be begun (one inside another: always).
        [[nodiscard]] bool begun() const noexcept { return m_begun; }

    private:
        const Library &m_library;
        bool m_begun = true;
    };

private:
    // The knowledge graph, schema history and spaces live in the same database.
    friend class Graph;
    friend struct GraphCut;
    friend class LibraryMigrations;
    friend class Spaces;
    friend class ChangeLog;
    friend class SpaceReplay;
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
    bool m_keepFormat = false;
    std::function<void(const QString &)> m_bringingUpToDate;
    QString m_connection;
    // Its statements, each prepared once (#104).
    std::unique_ptr<Statements> m_statements;
    // This computer's id here, once asked for (Spaces::deviceId).
    mutable QString m_deviceId;
    mutable PersonFile m_person;
};
