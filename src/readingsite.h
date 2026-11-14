#pragma once

#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "curation.h"
#include "library.h"

class Settings;

// One book as a reading site lists it on somebody's shelves.
struct SiteEntry {
    QString siteId;         // what the site calls the book
    QString title;          // as the site writes it, series and all
    QStringList authors;
    QString isbn;           // 13 digits when the site knows them, else 10
    QString shelf;          // "to-read", "reading" or "read"; empty for a shelf of the user's own making
    int rating = 0;         // the user's own stars, 1 to 5, 0 for none
    QString finished;       // the day it was read, as an ISO date
    QString added;          // the day it was shelved, as an ISO date
    QString binding;        // "Paperback", "Kindle Edition", ...
    int owned = 0;          // copies the user says they own
    QUrl coverUrl;
    double averageRating = 0; // what the site's readers make of it
    QString publisher;
    QString published;      // a year, usually
    QStringList shelves;    // the user's other shelves the book is on
    // On a shelf that stands in for the three and that Omabook has no word
    // for ("did not finish", "stopped reading"): its place there is neither
    // taken over here nor ever written over there.
    bool otherShelf = false;

    // Whether this says there is a copy on a real shelf: the user owns one
    // and the edition is not an ebook or an audiobook. The binding alone is
    // not enough, since sites file every book under some edition.
    bool paper() const;
    // The book's details for the library: the series a title carries
    // ("Dune Messiah (Dune, #2)") is taken out of it.
    BookMeta meta() const;
    QJsonObject toJson() const;
};

// What a site knows of a book asked for by title: enough to tell that the
// book exists and what its readers think of it.
struct SiteBook {
    QString site;           // "Goodreads"
    QString scheme;         // "goodreads"
    QString siteId;
    QString title;
    QStringList authors;
    double averageRating = 0;
    qint64 ratings = 0;
    QUrl coverUrl;
    QUrl url;
};

// Why a site couldn't be asked, as far as the site lets it be told apart.
enum class SiteProblem {
    None,
    NoIdentity,  // nobody is named, or what was pasted names nobody
    NoUser,      // the site knows no such person
    Private,     // the person is there, but their shelves are not shown to others
    Secret,      // a token or key is missing, wrong or has run out
    Refused,     // the site is turning requests away for now
    Unreachable, // no answer at all
    Changed,     // an answer Omabook doesn't understand
};
// "private", "no-user", ...: the problem as one word, for JSON.
QString siteProblemWord(SiteProblem problem);

// What a look at somebody's profile found.
struct SiteProfile {
    QString identity;   // who they are to the site, when the look settled that (a number found from a name)
    QString name;       // what the site calls them
    int books = -1;     // how many books they keep there, or -1 when the site doesn't say
    int shelves = 0;    // on how many shelves
    // False when the person was found but their shelves can't be read live
    // (Goodreads, for most profiles): the export file is then the way in.
    bool shelvesReadable = true;
};

// How one change sent to a site went.
struct SiteWritten {
    qint64 bookId = 0;
    QString siteId;     // what the site calls the book, now that it is known
    QString error;      // English; empty when the site took the change
};

namespace ReadingSites {
struct Outgoing;
}

// A site where people keep their reading: what they read, are reading and
// want to read. Omabook reads the shelves of the account it is connected
// to, reads the site's own export file, writes a file the site takes back
// and, where the site has a way in for that, writes changes to it. Each
// request answers once. A new site is one subclass; what a site can't do it
// says, in the defaults below.
class ReadingSite : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    // Who somebody is on this site, from whatever they have of it: a link
    // to their profile, to a shelf, a feed address, or the bare name or
    // number. Empty when it names nobody here.
    virtual QString identityFrom(const QString &pasted) const { return pasted.trimmed(); }
    // A key that came along in what was pasted (a feed address carries
    // one), which belongs in the keyring and not in the settings.
    virtual QString secretIn(const QString &) const { return {}; }
    // Whether the site wants a token or key: not at all, to do more (read
    // what is private, write changes), or to do anything.
    enum class SecretUse { NoSecret, OptionalSecret, NeedsSecret };
    using enum SecretUse; // ReadingSite::NeedsSecret, as it always read
    virtual SecretUse secretUse() const { return NoSecret; }
    virtual void setSecret(const QString &) {}
    // The page on which somebody finds who they are here, and the one that
    // gives them their token or key.
    virtual QUrl findMePage() const { return {}; }
    virtual QUrl secretPage() const { return {}; }

    // Looks at the account once and answers `checked` with what is there,
    // or `failed`, with problem() saying which kind of failure it was.
    virtual void check();
    SiteProblem problem() const noexcept { return m_problem; }

    // Whether changes can be written to the site with what it was given.
    virtual bool writes() const { return false; }
    // Writes each change; answers `wrote` once, with how each one went.
    // Nothing is ever removed from the site.
    virtual void write(const QList<ReadingSites::Outgoing> &changes);

    virtual QString name() const = 0;
    // The name its books are known by in Book::identifiers.
    virtual QString scheme() const = 0;
    // The account whose shelves are fetched, or empty when none is set.
    virtual QString account() const = 0;

    // Asks for everything on the account's shelves; answers `fetched` or `failed`.
    virtual void fetchShelves() = 0;
    // Asks which books go by this title; answers `found` (with none when
    // the site knows no such book) or `findFailed`.
    virtual void find(const QString &title, const QString &author) = 0;

    struct Export {
        QList<SiteEntry> entries;
        QString error; // English; empty when the file was read
    };
    // Reads the file the site lets its users download.
    virtual Export readExport(const QByteArray &file) const;
    // Writes Omabook's reading as a file the site's own import takes, or
    // nothing when the site has no such import.
    virtual QByteArray writeExport(const QList<Book> &books) const;

signals:
    void fetched(const QList<SiteEntry> &entries);
    void failed(const QString &error);
    void found(const QList<SiteBook> &books);
    void findFailed(const QString &error);
    void checked(const SiteProfile &profile);
    void wrote(const QList<SiteWritten> &results);

protected:
    // Says what went wrong, and which kind of wrong it was.
    void fail(SiteProblem problem, const QString &message);
    void clearProblem() { m_problem = SiteProblem::None; }

private:
    SiteProblem m_problem = SiteProblem::None;
};

namespace ReadingSites {

// The sites Omabook knows, by the name their books go by, in the order
// they are shown: "goodreads", "openlibrary", "hardcover".
QStringList schemes();
// One site as somebody uses it: who they are there, and the token or key
// that opens it for them (empty for none). Null for a scheme nobody knows.
ReadingSite *make(const QString &scheme, const QString &identity, const QString &secret, QNetworkAccessManager *network,
                  QObject *parent);
// The sites that can be asked as things stand, owned by `parent`: each with
// the identity in the settings and its secret from the keyring. Goodreads
// and Hardcover are left out when their plugin is switched off, and
// Hardcover also while it has no token, so nothing is then asked of them.
QList<ReadingSite *> all(const Settings &settings, QNetworkAccessManager *network, QObject *parent);
// The one with this scheme, or null.
ReadingSite *one(const Settings &settings, const QString &scheme, QNetworkAccessManager *network, QObject *parent);
// The series a title carries in brackets, taken out of it: "Dune Messiah
// (Dune, #2)" is "Dune Messiah", second of "Dune". A range ("#1-3") counts
// from its start, and of several series the first is taken.
struct TitleParts {
    QString title;
    QString series;
    double index = 0;
};
TitleParts splitSeries(const QString &title);

// What the sites' requests share: who is asking, and a time limit.
QNetworkRequest request(const QUrl &url);
// An address with every value percent-encoded once.
QUrl address(const QString &base, const QList<QPair<QString, QString>> &items);

// Rows of fields, as RFC 4180 has them: quoted fields may hold commas, line
// breaks and doubled quotes.
QList<QStringList> parseCsv(const QString &text);
QString csvField(const QString &text);

// The 13 digits of an ISBN, from either form and however it is punctuated;
// empty when it isn't one.
QString isbn13(const QString &isbn);
// What two spellings of a title or a name share: Curation's keys, which the
// library's duplicates are found with too.
QString titleKey(const QString &title);
QString authorKey(const QString &author);

// Whether two titles, or an author and a book's authors, are the same: a
// subtitle on one side or a middle initial on the other makes no difference.
bool sameTitle(const QString &asked, const QString &theirs);
bool sameAuthor(const QString &asked, const QStringList &authors);

// The book an entry is, or 0: by the site's own number, then by ISBN, then
// by title and first author.
qint64 match(const QList<Book> &books, const SiteEntry &entry, const QString &scheme);

struct Change {
    QString title;
    qint64 bookId = 0;
    QString what;   // "new", "updated", "unchanged" or "unmatched"
    QString note;   // what changed, or what was kept as it is here
};

// One book's reading as it should be on the site: only the fields named
// are to change there.
struct Outgoing {
    qint64 bookId = 0;
    QString siteId;         // what the site calls the book, when that is known
    QString title;
    QStringList authors;
    QString isbn;
    QString shelf;          // "to-read", "reading" or "read"; empty to leave the shelf alone
    int rating = 0;         // 0 to leave the stars alone
    QString finished;       // empty to leave the date alone
    bool isNew = false;     // the site doesn't list the book yet
    // The book's whole status here, for a site that moves a book of its own
    // accord when it is rated: it is put back where it belongs.
    QString status;

    QString note() const;
    QJsonObject toJson() const;
};

struct Summary {
    int added = 0;      // entries that became new books
    int updated = 0;    // books whose reading, copy or identifier changed
    int unchanged = 0;
    int kept = 0;       // books where what Omabook says was newer and stayed
    int unmatched = 0;  // entries that are no book here and were not added
    QList<Change> changes;
    QStringList problems;
    // Covers still to fetch: for new books, and for books that have none.
    QList<QPair<qint64, QUrl>> covers;
    // What Omabook says that the site doesn't yet: to be written there, when
    // the site takes changes and the user turned that on.
    QList<Outgoing> outgoing;

    QJsonObject toJson() const;
};

struct Options {
    bool addNew = true;     // whether an entry that is no book here becomes one
    bool writeBack = false; // whether what was changed here is gathered to go to the site
    bool dryRun = false;    // work out everything, change nothing
};

// Brings a site's entries into the library. Nothing is ever removed, and a
// book that is not among the entries is left alone.
//
// A matched book takes the site's shelf, stars and finished date field by
// field, by this rule: a field that is empty here is filled in; a field that
// still holds what the last sync with this site brought (kept with the book
// as the identifier "<scheme>:synced") follows the site; any other value was
// set in Omabook since and stays, to go back to the site with an export.
// A book whose status comes only from how far it was read here moves forward
// (to-read, reading, read) but never back.
//
// With `writeBack`, the same rule runs both ways: a field changed here that
// the site still has as the last sync brought it goes into `outgoing`, and
// so does a value the site lacks. A field changed on both sides stays as it
// is here, is not sent, and is counted as kept. Nothing is removed on
// either side: an empty field never replaces a value.
Summary merge(Library &library, const QList<SiteEntry> &entries, const QString &scheme, const Options &options = {});
// Records that these changes reached the site, so the next sync sees them
// as what the site says rather than as changes still to send.
void sent(Library &library, const QString &scheme, const QList<Outgoing> &changes);

// A cover for a book the site gave no picture of: Open Library's, by ISBN.
QUrl coverByIsbn(const QString &isbn);

// What a suggestion turned out to be once the sites were asked.
struct Checked {
    Curation::Suggestion suggestion;
    QString state = "checking"; // "checking", "found", "missing" (no site knows it) or "unchecked" (no site could be asked)
    SiteBook book;              // when found
    qint64 bookId = 0;          // the book in the library it already is, if it is one
    QString shelf;              // that book's status
    QString held;               // and whether there is a copy: "digital", "paper", "both", "none"

    QJsonObject toJson() const;
};

// Puts a suggestion on the to-read shelf: the book it already is here when
// it is one, else a new book that is only known of. The id, or 0 with `error`.
qint64 addToRead(Library &library, const Checked &checked, QString *error = nullptr);

} // namespace ReadingSites

// Checks a model's suggestions against the reading sites, one at a time:
// that each book exists, what its readers rate it, and whether the library
// already has it or wants it. The sites must outlive the check.
class SuggestionCheck : public QObject {
    Q_OBJECT

public:
    explicit SuggestionCheck(const QList<ReadingSite *> &sites, QObject *parent = nullptr);

    void check(const QList<Curation::Suggestion> &suggestions, const QList<Book> &books);
    const QList<ReadingSites::Checked> &results() const noexcept { return m_results; }

signals:
    void changed();
    void done();

private:
    void next();
    void ask();
    void answered(const QList<SiteBook> &books, bool failed);

    QList<ReadingSite *> m_sites;
    QList<ReadingSites::Checked> m_results;
    QList<Book> m_books;
    int m_at = -1;
    int m_site = 0;
    bool m_anyAnswered = false;
};

// Fetches covers one after another, gently: a site that says "slow down"
// ends the run, and what is left waits for the next sync.
class CoverFetcher : public QObject {
    Q_OBJECT

public:
    explicit CoverFetcher(QNetworkAccessManager *network, QObject *parent = nullptr);

    void fetch(const QList<QPair<qint64, QUrl>> &covers);
    void stop();

signals:
    // A picture arrived for this book.
    void cover(qint64 bookId, const QImage &picture);
    void done(int fetched, int left);

private:
    void next();

    QNetworkAccessManager *m_network;
    QList<QPair<qint64, QUrl>> m_queue;
    int m_fetched = 0;
    bool m_running = false;
};
