#include "readingsite.h"

#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>

#include "goodreads.h"
#include "graph.h"
#include "hardcover.h"
#include "openlibrarylog.h"
#include "secretstore.h"
#include "settings.h"

namespace {

// Letters and digits, lower case, accents dropped.
QString key(const QString &text) {
    QString key;
    for (const QChar c : text.normalized(QString::NormalizationForm_KD))
        if (c.isLetterOrNumber())
            key += c.toLower();
    return key;
}

// The library's books by everything an entry can be matched on, so a shelf
// of a thousand books is matched in one pass.
class Index {
public:
    Index(const QList<Book> &books, const QString &scheme) {
        for (const Book &book : books)
            add(book.id, book.identifiers.value(scheme).toString(), book.meta.isbn, book.meta.title, book.meta.authors.value(0));
    }

    void add(qint64 id, const QString &siteId, const QString &isbn, const QString &title, const QString &author) {
        if (!siteId.isEmpty() && !m_bySiteId.contains(siteId))
            m_bySiteId.insert(siteId, id);
        if (!siteId.isEmpty())
            m_linked.insert(id, siteId);
        if (const QString digits = ReadingSites::isbn13(isbn); !digits.isEmpty() && !m_byIsbn.contains(digits))
            m_byIsbn.insert(digits, id);
        const QString name = ReadingSites::titleKey(title);
        if (name.isEmpty())
            return;
        const QString who = ReadingSites::authorKey(author);
        if (!m_byTitle.value(name + "|" + who).contains(id))
            m_byTitle[name + "|" + who] << id;
        if (!m_byTitleAlone.value(name).contains(id))
            m_byTitleAlone[name] << id;
    }

    qint64 find(const QString &siteId, const QString &isbn, const QString &title, const QString &author) const {
        if (const qint64 id = m_bySiteId.value(siteId); id && !siteId.isEmpty())
            return id;
        if (const QString digits = ReadingSites::isbn13(isbn); !digits.isEmpty() && m_byIsbn.contains(digits))
            return m_byIsbn.value(digits);
        const QString name = ReadingSites::titleKey(title);
        if (name.isEmpty())
            return 0;
        const QString who = ReadingSites::authorKey(author);
        if (const qint64 id = unlinked(m_byTitle.value(name + "|" + who), siteId))
            return id;
        // The same title by another author is another book. Only where one
        // side names no author at all is the title enough.
        if (who.isEmpty())
            return unlinked(m_byTitleAlone.value(name), siteId);
        return unlinked(m_byTitle.value(name + "|"), siteId);
    }

private:
    // Of the books that go by a title, the one this entry can be: a book
    // already known to the site as another book (another edition, shelved
    // on its own) is not it while there is one that isn't.
    qint64 unlinked(const QList<qint64> &candidates, const QString &siteId) const {
        for (const qint64 id : candidates)
            if (siteId.isEmpty() || !m_linked.contains(id) || m_linked.value(id) == siteId)
                return id;
        return candidates.value(0);
    }

    QHash<QString, qint64> m_bySiteId, m_byIsbn;
    QHash<QString, QList<qint64>> m_byTitle, m_byTitleAlone;
    QHash<qint64, QString> m_linked;
};

// What a site said of a book's reading, as one word to keep with the book.
QString said(const QString &shelf, int rating, const QString &finished) {
    return shelf + "|" + QString::number(rating) + "|" + finished;
}

int along(const QString &shelf) { return shelf == "read" ? 3 : shelf == "reading" ? 2 : shelf == "to-read" ? 1 : 0; }

} // namespace

bool SiteEntry::paper() const {
    static const QRegularExpression notPaper("ebook|e-book|kindle|audio|audible|digital|nook|mp3",
                                             QRegularExpression::CaseInsensitiveOption);
    return owned > 0 && !notPaper.match(binding).hasMatch();
}

BookMeta SiteEntry::meta() const {
    BookMeta meta;
    const ReadingSites::TitleParts parts = ReadingSites::splitSeries(title);
    meta.title = parts.title;
    meta.series = parts.series;
    meta.seriesIndex = parts.index;
    meta.authors = authors;
    meta.isbn = isbn;
    meta.publisher = publisher;
    meta.published = published;
    meta.tags = shelves;
    return meta;
}

QJsonObject SiteEntry::toJson() const {
    QJsonObject object{{"id", siteId}, {"title", title}, {"authors", QJsonArray::fromStringList(authors)}, {"shelf", shelf}};
    if (!isbn.isEmpty()) object.insert("isbn", isbn);
    if (rating > 0) object.insert("rating", rating);
    if (!finished.isEmpty()) object.insert("finished", finished);
    if (!added.isEmpty()) object.insert("added", added);
    if (!binding.isEmpty()) object.insert("binding", binding);
    if (owned > 0) object.insert("owned", owned);
    if (coverUrl.isValid()) object.insert("cover_url", coverUrl.toString());
    if (averageRating > 0) object.insert("average_rating", averageRating);
    if (!shelves.isEmpty()) object.insert("shelves", QJsonArray::fromStringList(shelves));
    object.insert("paper", paper());
    return object;
}

ReadingSite::Export ReadingSite::readExport(const QByteArray &) const {
    return {{}, name() + " has no export file that Omabook reads."};
}

QByteArray ReadingSite::writeExport(const QList<Book> &) const { return {}; }

QString siteProblemWord(SiteProblem problem) {
    switch (problem) {
    case SiteProblem::None: return {};
    case SiteProblem::NoIdentity: return "no-identity";
    case SiteProblem::NoUser: return "no-user";
    case SiteProblem::Private: return "private";
    case SiteProblem::Secret: return "token";
    case SiteProblem::Refused: return "refused";
    case SiteProblem::Unreachable: return "unreachable";
    case SiteProblem::Changed: return "changed";
    }
    return {};
}

void ReadingSite::fail(SiteProblem problem, const QString &message) {
    m_problem = problem;
    emit failed(message);
}

// A site with no cheaper way to look reads the shelves and counts.
void ReadingSite::check() {
    auto *once = new QObject(this);
    connect(this, &ReadingSite::fetched, once, [this, once](const QList<SiteEntry> &entries) {
        once->deleteLater();
        SiteProfile profile;
        profile.name = account();
        profile.books = entries.size();
        QSet<QString> shelves;
        for (const SiteEntry &entry : entries)
            shelves << entry.shelf;
        profile.shelves = shelves.size();
        emit checked(profile);
    });
    connect(this, &ReadingSite::failed, once, [once] { once->deleteLater(); });
    fetchShelves();
}

void ReadingSite::write(const QList<ReadingSites::Outgoing> &changes) {
    QList<SiteWritten> results;
    for (const ReadingSites::Outgoing &change : changes)
        results.append({change.bookId, change.siteId, name() + " takes no changes."});
    QMetaObject::invokeMethod(this, [this, results] { emit wrote(results); }, Qt::QueuedConnection);
}

QStringList ReadingSites::schemes() { return {"goodreads", "openlibrary", "hardcover"}; }

ReadingSite *ReadingSites::make(const QString &scheme, const QString &identity, const QString &secret,
                                QNetworkAccessManager *network, QObject *parent) {
    ReadingSite *site = nullptr;
    if (scheme == "goodreads")
        site = new GoodreadsSite(identity, network, parent);
    else if (scheme == "openlibrary")
        site = new OpenLibraryLog(identity, network, parent);
    else if (scheme == "hardcover")
        site = new HardcoverSite(identity, network, parent);
    if (site && !secret.isEmpty())
        site->setSecret(secret);
    return site;
}

QList<ReadingSite *> ReadingSites::all(const Settings &settings, QNetworkAccessManager *network, QObject *parent) {
    QList<ReadingSite *> sites;
    for (const QString &scheme : schemes()) {
        if (!settings.siteOn(scheme))
            continue;
        // The keyring is only asked for a site somebody connected to.
        const QString secret = settings.site(scheme).value("secret").toBool() ? Secrets::shared().get(scheme) : QString();
        if (scheme == "hardcover" && secret.isEmpty())
            continue;
        sites << make(scheme, settings.siteIdentity(scheme), secret, network, parent);
    }
    return sites;
}

ReadingSite *ReadingSites::one(const Settings &settings, const QString &scheme, QNetworkAccessManager *network, QObject *parent) {
    ReadingSite *wanted = nullptr;
    for (ReadingSite *site : all(settings, network, parent)) {
        if (site->scheme() == scheme && !wanted)
            wanted = site;
        else
            delete site;
    }
    return wanted;
}

QNetworkRequest ReadingSites::request(const QUrl &url) {
    QNetworkRequest http(url);
    // Sites ask clients to say who they are; the app's name says enough.
    http.setHeader(QNetworkRequest::UserAgentHeader, "Omabook/" OMABOOK_VERSION);
    http.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    http.setTransferTimeout(20000);
    return http;
}

QUrl ReadingSites::address(const QString &base, const QList<QPair<QString, QString>> &items) {
    QStringList parts;
    for (const auto &[name, value] : items)
        parts << name + "=" + QString::fromLatin1(QUrl::toPercentEncoding(value));
    QUrl url(base);
    url.setQuery(parts.join('&'), QUrl::StrictMode);
    return url;
}

QList<QStringList> ReadingSites::parseCsv(const QString &text) {
    QList<QStringList> rows;
    QStringList row;
    QString field;
    bool quoted = false, fresh = true; // fresh: nothing of this field was read yet
    const qsizetype size = text.size();
    for (qsizetype i = text.startsWith(QChar(0xFEFF)) ? 1 : 0; i < size; ++i) {
        const QChar c = text.at(i);
        if (quoted) {
            if (c != '"')
                field += c;
            else if (i + 1 < size && text.at(i + 1) == '"')
                field += text.at(++i);
            else
                quoted = false;
        } else if (c == '"' && fresh) {
            // A quote opens a field only at its start; ="0306406152" is plain text.
            quoted = true;
            fresh = false;
        } else if (c == ',') {
            row << field;
            field.clear();
            fresh = true;
        } else if (c == '\n' || c == '\r') {
            if (c == '\r' && i + 1 < size && text.at(i + 1) == '\n')
                ++i;
            row << field;
            field.clear();
            fresh = true;
            // An empty line is no row.
            if (row.size() > 1 || !row.first().isEmpty())
                rows << row;
            row.clear();
        } else {
            field += c;
            fresh = false;
        }
    }
    if (!field.isEmpty() || !row.isEmpty()) {
        row << field;
        rows << row;
    }
    return rows;
}

QString ReadingSites::csvField(const QString &text) {
    static const QRegularExpression special("[\",\\r\\n]");
    if (!text.contains(special))
        return text;
    QString quoted = text;
    return "\"" + quoted.replace("\"", "\"\"") + "\"";
}

QString ReadingSites::isbn13(const QString &isbn) {
    QString digits;
    for (const QChar c : isbn)
        if (c.isDigit() || c.toUpper() == 'X')
            digits += c.toUpper();
    if (digits.size() == 13 && !digits.contains('X'))
        return digits;
    if (digits.size() != 10 || digits.left(9).contains('X'))
        return {};
    // The ten-digit form is the thirteen-digit one without "978" and with
    // another check digit.
    const QString body = "978" + digits.left(9);
    int sum = 0;
    for (int i = 0; i < 12; ++i)
        sum += body.at(i).digitValue() * (i % 2 ? 3 : 1);
    return body + QString::number((10 - sum % 10) % 10);
}

ReadingSites::TitleParts ReadingSites::splitSeries(const QString &title) {
    TitleParts parts;
    parts.title = title.simplified();
    if (!parts.title.endsWith(')'))
        return parts;
    // The last group of brackets, which may hold brackets of its own:
    // "Night Watch (Discworld (City Watch), #6)".
    int depth = 0;
    qsizetype open = -1;
    for (qsizetype i = parts.title.size() - 1; i >= 0 && open < 0; --i) {
        if (parts.title.at(i) == ')')
            ++depth;
        else if (parts.title.at(i) == '(' && --depth == 0)
            open = i;
    }
    if (open <= 0)
        return parts;
    // Several series are listed with semicolons; the first is the main one.
    const QString inside = parts.title.mid(open + 1, parts.title.size() - open - 2).section(';', 0, 0);
    // A number sign that opens the brackets ("(#1-7)") names no series.
    const qsizetype sign = inside.lastIndexOf('#');
    if (sign <= 0)
        return parts;
    static const QRegularExpression number("^\\s*(\\d+(?:\\.\\d+)?)");
    const auto match = number.match(inside.mid(sign + 1));
    QString name = inside.left(sign).simplified();
    while (name.endsWith(','))
        name.chop(1);
    if (!match.hasMatch() || name.simplified().isEmpty())
        return parts;
    parts.series = name.simplified();
    parts.index = match.captured(1).toDouble();
    parts.title = parts.title.left(open).simplified();
    return parts;
}

QString ReadingSites::titleKey(const QString &title) {
    static const QRegularExpression brackets("\\s*\\([^()]*\\)\\s*$");
    static const QRegularExpression article("^(the|a|an)\\s+", QRegularExpression::CaseInsensitiveOption);
    QString bare = title.simplified();
    // Only when something is left: a title that is all brackets stays.
    if (const QString without = QString(bare).remove(brackets); !key(without).isEmpty())
        bare = without;
    // "The Hobbit" is shelved as "Hobbit, The" as often as not.
    if (bare.endsWith(", The", Qt::CaseInsensitive))
        bare.chop(5);
    if (const QString without = QString(bare).remove(article); !key(without).isEmpty())
        bare = without;
    return key(bare);
}

QString ReadingSites::authorKey(const QString &author) { return key(Curation::tidyAuthor(author)); }

bool ReadingSites::sameAuthor(const QString &asked, const QStringList &authors) {
    const QString wanted = ReadingSites::authorKey(asked);
    if (wanted.isEmpty() || authors.isEmpty())
        return true;
    // "Ursula Le Guin" and "Ursula K. Le Guin" share a family name.
    const QString family = key(asked.simplified().section(' ', -1));
    for (const QString &author : authors) {
        const QString theirs = ReadingSites::authorKey(author);
        if (theirs == wanted || (family.size() >= 3 && theirs.endsWith(family)))
            return true;
    }
    return false;
}

bool ReadingSites::sameTitle(const QString &asked, const QString &theirs) {
    const QString a = ReadingSites::titleKey(asked), b = ReadingSites::titleKey(theirs);
    if (a.isEmpty() || b.isEmpty())
        return false;
    // A subtitle on one side only is still the same book.
    return a == b || (qMin(a.size(), b.size()) >= 4 && (a.startsWith(b) || b.startsWith(a)));
}

qint64 ReadingSites::match(const QList<Book> &books, const SiteEntry &entry, const QString &scheme) {
    return Index(books, scheme).find(entry.siteId, entry.isbn, entry.title, entry.authors.value(0));
}

QUrl ReadingSites::coverByIsbn(const QString &isbn) {
    const QString digits = isbn13(isbn);
    // Without "default=false" Open Library answers a blank picture for a
    // book it has no cover of.
    return digits.isEmpty() ? QUrl() : QUrl("https://covers.openlibrary.org/b/isbn/" + digits + "-L.jpg?default=false");
}

QJsonObject ReadingSites::Summary::toJson() const {
    QJsonArray list;
    for (const Change &change : changes) {
        // A thousand books that are as they were say nothing one by one.
        if (change.what == "unchanged" && change.note.isEmpty())
            continue;
        QJsonObject object{{"title", change.title}, {"change", change.what}};
        if (change.bookId > 0) object.insert("id", change.bookId);
        if (!change.note.isEmpty()) object.insert("note", change.note);
        list << object;
    }
    QJsonArray toSend;
    for (const Outgoing &change : outgoing)
        toSend << change.toJson();
    return {{"new", added}, {"updated", updated}, {"unchanged", unchanged}, {"kept", kept}, {"unmatched", unmatched},
            {"changes", list}, {"to_send", toSend}, {"problems", QJsonArray::fromStringList(problems)}};
}

QString ReadingSites::Outgoing::note() const {
    QStringList parts;
    if (!shelf.isEmpty()) parts << "shelf " + shelf;
    if (rating > 0) parts << QString("%1 stars").arg(rating);
    if (!finished.isEmpty()) parts << "finished " + finished;
    return (isNew ? "add: " : "") + parts.join(", ");
}

QJsonObject ReadingSites::Outgoing::toJson() const {
    QJsonObject object{{"id", bookId}, {"title", title}, {"new_on_site", isNew}};
    if (!shelf.isEmpty()) object.insert("status", shelf);
    if (rating > 0) object.insert("rating", rating);
    if (!finished.isEmpty()) object.insert("finished", finished);
    return object;
}

ReadingSites::Summary ReadingSites::merge(Library &library, const QList<SiteEntry> &entries, const QString &scheme,
                                          const Options &options) {
    Summary summary;
    const QList<Book> held = library.books();
    Index index(held, scheme);
    Graph graph(library);
    const QString syncedKey = scheme + ":synced";
    const bool writes = !options.dryRun;
    auto coverFor = [](const SiteEntry &entry) { return entry.coverUrl.isValid() ? entry.coverUrl : coverByIsbn(entry.isbn); };
    QSet<qint64> onSite;
    // In a dry run a book that would be added gets a number of its own, so
    // the same book listed twice still counts once.
    qint64 pretend = -1;

    for (const SiteEntry &entry : entries) {
        const BookMeta meta = entry.meta();
        if (meta.title.isEmpty()) {
            ++summary.unmatched;
            summary.problems << "An entry without a title was left out" + (entry.siteId.isEmpty() ? QString(".") : " (" + entry.siteId + ").");
            continue;
        }
        const QString theirs = said(entry.shelf, entry.rating, entry.finished);
        const qint64 known = index.find(entry.siteId, entry.isbn, entry.title, entry.authors.value(0));
        const auto book = known > 0 ? library.book(known) : std::nullopt;

        if (known < 0) {
            // Seen earlier in this dry run.
            ++summary.unchanged;
            continue;
        }
        if (!book) {
            if (!options.addNew) {
                ++summary.unmatched;
                summary.changes.append({meta.title, 0, "unmatched", {}});
                continue;
            }
            qint64 id = pretend--;
            if (writes) {
                const Library::Added added = library.addEntry(meta, entry.paper());
                if (!added.error.isEmpty()) {
                    ++summary.unmatched;
                    summary.problems << meta.title + ": " + added.error;
                    continue;
                }
                id = added.id;
                library.setReading(id, entry.shelf, entry.rating, entry.finished);
                library.setIdentifier(id, scheme, entry.siteId);
                library.setIdentifier(id, syncedKey, theirs);
                graph.bookNode(id);
                if (const QUrl cover = coverFor(entry); cover.isValid())
                    summary.covers.append({id, cover});
            }
            index.add(id, entry.siteId, entry.isbn, entry.title, entry.authors.value(0));
            onSite << id;
            ++summary.added;
            summary.changes.append({meta.title, qMax<qint64>(0, id), "new", entry.paper() ? "a paper book" : "only known of"});
            continue;
        }
        onSite << book->id;

        // What the last sync brought, to tell a change made here from one
        // made on the site.
        const QStringList base = book->identifiers.value(syncedKey).toString().split('|');
        const bool synced = base.size() == 3;
        QStringList took, kept;
        QString shelf = book->shelf, finished = book->finished;
        int rating = book->rating;
        Outgoing out;
        // What is remembered as "what the site said" for next time. A field
        // that differs on both sides keeps what was remembered before (or a
        // mark that nothing was), so it goes on being a difference to look
        // at, and never turns into a change to send.
        QStringList remembered{entry.shelf, QString::number(entry.rating), entry.finished};
        auto differs = [&](int field) { remembered[field] = synced ? base.at(field) : QString("?"); };

        // One field, three ways: `mine` here, `theirs` on the site, and
        // `before`, what the last sync brought. Empty never wins over a value.
        enum class Verdict { Same, Take, Send, Keep };
        using enum Verdict;
        auto decide = [&](bool mineEmpty, bool theirsEmpty, bool same, bool mineUnchanged, bool theirsUnchanged) {
            if (same || (mineEmpty && theirsEmpty))
                return Same;
            if (mineEmpty)
                return Take;
            // What the site lacks is sent, unless it had it at the last sync
            // and it was not touched here since: then it was taken off there,
            // and is neither put back nor taken off here.
            if (theirsEmpty)
                return options.writeBack && !(synced && mineUnchanged) ? Send : Same;
            if (synced && mineUnchanged)
                return Take;
            // Changed here and not there: it goes to the site when the site
            // takes changes, and otherwise stays, to go back with an export.
            if (synced && theirsUnchanged && options.writeBack)
                return Send;
            return Keep;
        };

        if (entry.otherShelf) {
            // Where the site keeps it is its own business.
        } else if (book->shelf.isEmpty()) {
            // Nobody set a shelf here; how far it was read still counts,
            // and never moves back.
            const QString status = book->status();
            if (!entry.shelf.isEmpty() && along(entry.shelf) >= along(status)) {
                shelf = entry.shelf;
                took << "shelf " + entry.shelf;
            } else if (!status.isEmpty() && status != entry.shelf && (options.writeBack || !entry.shelf.isEmpty())) {
                if (options.writeBack)
                    out.shelf = status;
                else
                    kept << QString("%1 here, %2 on the site").arg(status, entry.shelf.isEmpty() ? QString("no shelf") : entry.shelf);
            }
        } else {
            switch (decide(false, entry.shelf.isEmpty(), entry.shelf == book->shelf, synced && book->shelf == base.at(0),
                           synced && entry.shelf == base.at(0))) {
            case Take: shelf = entry.shelf; took << "shelf " + entry.shelf; break;
            case Send: out.shelf = book->shelf; break;
            case Keep: kept << QString("%1 here, %2 on the site").arg(book->shelf, entry.shelf); differs(0); break;
            case Same: break;
            }
        }
        switch (decide(book->rating == 0, entry.rating == 0, entry.rating == book->rating, synced && book->rating == base.at(1).toInt(),
                       synced && entry.rating == base.at(1).toInt())) {
        case Take: rating = entry.rating; took << QString("%1 stars").arg(entry.rating); break;
        case Send: out.rating = book->rating; break;
        case Keep: kept << QString("%1 stars here, %2 on the site").arg(book->rating).arg(entry.rating); differs(1); break;
        case Same: break;
        }
        switch (decide(book->finished.isEmpty(), entry.finished.isEmpty(), entry.finished == book->finished,
                       synced && book->finished == base.at(2), synced && entry.finished == base.at(2))) {
        case Take: finished = entry.finished; took << "finished " + entry.finished; break;
        case Send: out.finished = book->finished; break;
        case Keep: kept << QString("finished %1 here, %2 on the site").arg(book->finished, entry.finished); differs(2); break;
        case Same: break;
        }

        const bool reading = !took.isEmpty();
        const bool link = !entry.siteId.isEmpty() && book->identifiers.value(scheme).toString() != entry.siteId;
        if (link)
            took << "linked";
        if (entry.paper() && !book->paper)
            took << "a paper copy";
        if (writes) {
            if (reading) {
                library.setReading(book->id, shelf, rating, finished);
                graph.bookNode(book->id);
            }
            if (link)
                library.setIdentifier(book->id, scheme, entry.siteId);
            if (book->identifiers.value(syncedKey).toString() != remembered.join('|'))
                library.setIdentifier(book->id, syncedKey, remembered.join('|'));
            if (entry.paper() && !book->paper)
                library.setPaper(book->id, true);
            // An ISBN the book lacked makes the next match certain.
            if (book->meta.isbn.isEmpty() && !entry.isbn.isEmpty()) {
                BookMeta filled = book->meta;
                filled.isbn = entry.isbn;
                library.setMeta(book->id, filled);
            }
            if (!book->hasCover)
                if (const QUrl cover = coverFor(entry); cover.isValid())
                    summary.covers.append({book->id, cover});
        }
        index.add(book->id, entry.siteId, entry.isbn, entry.title, entry.authors.value(0));

        if (!out.shelf.isEmpty() || out.rating > 0 || !out.finished.isEmpty()) {
            out.bookId = book->id;
            out.siteId = entry.siteId;
            out.title = book->meta.title;
            out.authors = book->meta.authors;
            out.isbn = book->meta.isbn.isEmpty() ? entry.isbn : book->meta.isbn;
            out.status = out.shelf.isEmpty() ? (shelf.isEmpty() ? book->status() : shelf) : out.shelf;
            summary.outgoing << out;
        }
        if (!kept.isEmpty())
            ++summary.kept;
        took.isEmpty() ? ++summary.unchanged : ++summary.updated;
        summary.changes.append({book->meta.title, book->id, took.isEmpty() ? "unchanged" : "updated",
                                (took + (kept.isEmpty() ? QStringList() : QStringList{"kept: " + kept.join("; ")})).join(", ")});
    }

    // What is read, being read or wanted here and was never on the site goes
    // there too, when the site takes changes. A book the site once listed
    // and no longer does was taken off there, and is not put back.
    if (options.writeBack) {
        for (const Book &book : held) {
            const QString status = book.status();
            if (onSite.contains(book.id) || status.isEmpty() || book.identifiers.contains(syncedKey))
                continue;
            Outgoing out;
            out.bookId = book.id;
            out.siteId = book.identifiers.value(scheme).toString();
            out.title = book.meta.title;
            out.authors = book.meta.authors;
            out.isbn = book.meta.isbn;
            out.shelf = status;
            out.status = status;
            out.rating = book.rating;
            out.finished = book.finished;
            out.isNew = true;
            summary.outgoing << out;
        }
    }
    return summary;
}

void ReadingSites::sent(Library &library, const QString &scheme, const QList<Outgoing> &changes) {
    const QString syncedKey = scheme + ":synced";
    for (const Outgoing &change : changes) {
        const auto book = library.book(change.bookId);
        if (!book)
            continue;
        // The site now says what was sent; the fields that were not sent
        // are as the last sync left them.
        QStringList base = book->identifiers.value(syncedKey).toString().split('|');
        if (base.size() != 3)
            base = QStringList{QString(), "0", QString()};
        if (!change.shelf.isEmpty()) base[0] = change.shelf;
        if (change.rating > 0) base[1] = QString::number(change.rating);
        if (!change.finished.isEmpty()) base[2] = change.finished;
        library.setIdentifier(book->id, syncedKey, base.join('|'));
        if (!change.siteId.isEmpty() && book->identifiers.value(scheme).toString() != change.siteId)
            library.setIdentifier(book->id, scheme, change.siteId);
    }
}

QJsonObject ReadingSites::Checked::toJson() const {
    QJsonObject object{{"title", book.title.isEmpty() ? suggestion.title : book.title},
                       {"author", book.authors.isEmpty() ? suggestion.author : book.authors.join(" & ")},
                       {"why", suggestion.why},
                       {"state", state}};
    if (state == "found") {
        object.insert("site", book.site);
        object.insert("site_id", book.siteId);
        object.insert("url", book.url.toString());
        if (book.averageRating > 0) {
            object.insert("rating", book.averageRating);
            object.insert("ratings", book.ratings);
        }
    }
    if (bookId > 0) {
        object.insert("in_library", bookId);
        object.insert("status", shelf);
        object.insert("held", held);
    }
    return object;
}

qint64 ReadingSites::addToRead(Library &library, const Checked &checked, QString *error) {
    if (const auto book = checked.bookId ? library.book(checked.bookId) : std::nullopt) {
        // Already here: it only goes on the shelf when it is on none.
        if (book->status().isEmpty()) {
            library.setReading(book->id, "to-read", book->rating, book->finished);
            Graph(library).bookNode(book->id);
        }
        return book->id;
    }
    BookMeta meta;
    meta.title = checked.book.title.isEmpty() ? checked.suggestion.title : checked.book.title;
    meta.authors = checked.book.authors.isEmpty() ? BookMeta::splitAuthors(checked.suggestion.author) : checked.book.authors;
    const Library::Added added = library.addEntry(meta, false);
    if (!added.error.isEmpty()) {
        if (error)
            *error = added.error;
        return 0;
    }
    library.setReading(added.id, "to-read", 0, {});
    if (!checked.book.siteId.isEmpty())
        library.setIdentifier(added.id, checked.book.scheme, checked.book.siteId);
    Graph(library).bookNode(added.id);
    return added.id;
}

SuggestionCheck::SuggestionCheck(const QList<ReadingSite *> &sites, QObject *parent) : QObject(parent), m_sites(sites) {
    for (ReadingSite *site : std::as_const(m_sites)) {
        connect(site, &ReadingSite::found, this, [this, site](const QList<SiteBook> &books) {
            if (m_at >= 0 && m_at < m_results.size() && m_sites.value(m_site) == site)
                answered(books, false);
        });
        connect(site, &ReadingSite::findFailed, this, [this, site] {
            if (m_at >= 0 && m_at < m_results.size() && m_sites.value(m_site) == site)
                answered({}, true);
        });
    }
}

void SuggestionCheck::check(const QList<Curation::Suggestion> &suggestions, const QList<Book> &books) {
    m_results.clear();
    m_books = books;
    const Index index(books, {});
    for (const Curation::Suggestion &suggestion : suggestions) {
        ReadingSites::Checked checked;
        checked.suggestion = suggestion;
        checked.bookId = index.find({}, {}, suggestion.title, suggestion.author);
        m_results << checked;
    }
    m_at = -1;
    next();
}

void SuggestionCheck::next() {
    ++m_at;
    m_site = 0;
    m_anyAnswered = false;
    // What the library says of the ones it already has.
    for (ReadingSites::Checked &checked : m_results) {
        for (const Book &book : std::as_const(m_books)) {
            if (book.id == checked.bookId) {
                checked.shelf = book.status();
                checked.held = book.held();
            }
        }
    }
    emit changed();
    if (m_at >= m_results.size()) {
        emit done();
        return;
    }
    ask();
}

void SuggestionCheck::ask() {
    if (m_site >= m_sites.size()) {
        m_results[m_at].state = m_anyAnswered ? "missing" : "unchecked";
        next();
        return;
    }
    const Curation::Suggestion &suggestion = m_results.at(m_at).suggestion;
    m_sites.at(m_site)->find(suggestion.title, suggestion.author);
}

void SuggestionCheck::answered(const QList<SiteBook> &books, bool failed) {
    ReadingSites::Checked &checked = m_results[m_at];
    m_anyAnswered = m_anyAnswered || !failed;
    // Of the editions that are this book, the one most readers rated.
    const SiteBook *best = nullptr;
    for (const SiteBook &book : books)
        if (ReadingSites::sameTitle(checked.suggestion.title, book.title) && ReadingSites::sameAuthor(checked.suggestion.author, book.authors)
            && (!best || book.ratings > best->ratings))
            best = &book;
    if (best) {
        const SiteBook book = *best;
        checked.state = "found";
        checked.book = book;
        // The site's own number finds a book the title's spelling missed.
        if (!checked.bookId)
            checked.bookId = Index(m_books, book.scheme).find(book.siteId, {}, book.title, book.authors.value(0));
        next();
        return;
    }
    ++m_site;
    ask();
}

CoverFetcher::CoverFetcher(QNetworkAccessManager *network, QObject *parent) : QObject(parent), m_network(network) {}

void CoverFetcher::fetch(const QList<QPair<qint64, QUrl>> &covers) {
    m_queue += covers;
    if (!m_running) {
        m_fetched = 0;
        m_running = true;
        next();
    }
}

void CoverFetcher::stop() { m_queue.clear(); }

void CoverFetcher::next() {
    if (m_queue.isEmpty()) {
        m_running = false;
        emit done(m_fetched, 0);
        return;
    }
    const auto [bookId, url] = m_queue.takeFirst();
    QNetworkReply *reply = m_network->get(ReadingSites::request(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, bookId = bookId] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Told to slow down: the rest waits for another day rather than
        // being asked for again and again.
        if (status == 403 || status == 429 || status == 503) {
            const int left = m_queue.size() + 1;
            m_queue.clear();
            m_running = false;
            emit done(m_fetched, left);
            return;
        }
        const QImage picture = reply->error() == QNetworkReply::NoError ? QImage::fromData(reply->readAll()) : QImage();
        // Catalogs answer a missing cover with a one-pixel picture.
        if (picture.width() > 50) {
            ++m_fetched;
            emit cover(bookId, picture);
        }
        next();
    });
}
