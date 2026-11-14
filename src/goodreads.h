#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QString>
#include <QStringList>
#include <optional>

#include "metadatasource.h"
#include "readingsite.h"

// Goodreads, natively. It closed its API in 2020 and has no way to write to
// it, so this reads what it still offers anyone: the file a user downloads
// of their own shelves, the feed of a public profile's shelves, the search
// box's suggestions and the book pages themselves. What goes back to
// Goodreads goes as a file for its own import page.
namespace Goodreads {

// The number in a profile address ("https://www.goodreads.com/user/show/
// 12345-ann"), a feed address, or the number alone; empty when there is none.
QString userId(const QString &text);
// Who somebody is on Goodreads, from whatever they have: the number in any
// of those addresses, or the name a profile goes by (goodreads.com/ann).
QString identity(const QString &text);
// The key in a shelf's feed address, which opens a profile that isn't public.
QString feedKey(const QString &text);
// What a profile page says of its owner: their name and how many books they
// keep. False when the page is not a profile.
bool parseProfile(const QByteArray &html, SiteProfile *profile);

// goodreads_library_export.csv. ISBNs are written ="0306406152" there so a
// spreadsheet leaves them alone.
ReadingSite::Export parseCsv(const QByteArray &file);
// The same columns, for Goodreads' import page: every book that is to read,
// being read or read, with its stars, dates and ISBN.
QByteArray writeCsv(const QList<Book> &books);

// One page of a shelf's feed; `shelf` is the shelf that was asked for.
// `understood` is false when the answer is not a feed at all.
QList<SiteEntry> parseFeed(const QByteArray &xml, const QString &shelf, bool *understood = nullptr);

// A book as its own page or the search box describes it.
struct Found {
    MetaCandidate candidate;
    QString id;
    double averageRating = 0;
    qint64 ratings = 0;
};
// The search box's suggestions. `understood` is false when the answer is
// not the list it used to be.
QList<Found> parseSuggestions(const QByteArray &json, bool *understood = nullptr);
// A book's page: the data the page is built from, or what it tells search
// engines when that is gone. Nothing, with `problem`, when neither is there.
std::optional<Found> parseBookPage(const QByteArray &html, QString *problem = nullptr);

} // namespace Goodreads

class GoodreadsSite : public ReadingSite {
    Q_OBJECT

public:
    // `user` is whatever names the person: an id, a profile address, a
    // name, or a feed address (whose key is taken too). Tests pass their
    // own server as `base`.
    GoodreadsSite(const QString &user, QNetworkAccessManager *network, QObject *parent = nullptr,
                  const QString &base = "https://www.goodreads.com");

    QString name() const override { return "Goodreads"; }
    QString scheme() const override { return "goodreads"; }
    QString account() const override { return m_user; }

    QString identityFrom(const QString &pasted) const override { return Goodreads::identity(pasted); }
    QString secretIn(const QString &pasted) const override { return Goodreads::feedKey(pasted); }
    // The key of a shelf feed, for those who have one: it opens the shelves
    // of a profile that isn't public.
    SecretUse secretUse() const override { return OptionalSecret; }
    void setSecret(const QString &key) override { m_key = key; }
    // "My Books", which takes whoever is signed in to their own shelves.
    QUrl findMePage() const override;
    // Where Goodreads hands out the file of somebody's shelves, and takes one back.
    static QUrl exportPage();

    void check() override;
    void fetchShelves() override;
    void find(const QString &title, const QString &author) override;
    Export readExport(const QByteArray &file) const override { return Goodreads::parseCsv(file); }
    QByteArray writeExport(const QList<Book> &books) const override { return Goodreads::writeCsv(books); }

    // Public so the addresses can be tested without the network.
    QUrl shelfUrl(const QString &shelf, int page) const;
    QUrl profileUrl() const;
    QUrl findUrl(const QString &title, const QString &author) const;

private:
    bool named();
    void checkShelves(SiteProfile profile);
    void fetchPage();

    QString m_user, m_key, m_base;
    QNetworkAccessManager *m_network;
    // The fetch in hand: which shelf, which page, what came so far.
    int m_shelf = 0;
    int m_page = 1;
    QString m_firstOfLastPage;
    QList<SiteEntry> m_entries;
    bool m_fetching = false;
};

// Goodreads as a place to look a book's details up. The search box finds
// the book, by ISBN or by title and author, and the book's own page has
// the rest: two requests, however many results there are.
class GoodreadsSource : public MetadataSource {
    Q_OBJECT

public:
    explicit GoodreadsSource(QNetworkAccessManager *network, QObject *parent = nullptr,
                             const QString &base = "https://www.goodreads.com")
        : MetadataSource(parent), m_network(network), m_base(base) {}

    QString name() const override { return "Goodreads"; }
    QStringList fields() const override {
        return {"title", "authors", "series", "publisher", "published", "language", "isbn", "description", "tags", "cover"};
    }
    void search(const BookMeta &book) override;
    // How long to wait before asking again for a page that came back empty.
    void setRetryAfter(int milliseconds) { m_retryAfter = milliseconds; }

    // Public so the addresses can be tested without the network.
    QUrl isbnUrl(const QString &isbn) const;
    QUrl searchUrl(const BookMeta &book, bool withAuthor = true) const;
    QUrl bookUrl(const QString &id) const;

private:
    void searchByTitle(const BookMeta &book, bool withAuthor);
    void fetchPage(const QString &id, const QList<MetaCandidate> &others, int attempt);

    QNetworkAccessManager *m_network;
    QString m_base;
    int m_retryAfter = 700;
};
