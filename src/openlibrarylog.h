#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QNetworkAccessManager>
#include <QString>
#include <functional>

#include "readingsite.h"

class QNetworkReply;

// Open Library's reading log: the shelves every account there has. A log
// that is public can be read by anyone, as JSON. Signed in, with the keys
// Open Library gives every account for this, a private log can be read too,
// with the reader's own stars and finished dates, and changes can be
// written: a book put on a shelf, rated, or given the day it was finished.
// Nothing is ever taken off a shelf there.
namespace OpenLibrary {

// One page of a shelf: its entries, and how many the shelf holds in all.
// `understood` is false when the answer is not a reading log.
QList<SiteEntry> parseLog(const QByteArray &json, const QString &shelf, int *total = nullptr, bool *understood = nullptr);
// The books a search found, with what Open Library's readers rate them.
QList<SiteBook> parseSearch(const QByteArray &json, bool *understood = nullptr);

// Who somebody is there, from a link to their profile or their books
// ("openlibrary.org/people/ann/books/already-read"), or the name alone.
QString userName(const QString &pasted);
// The account a sign-in was for, which the cookie it answers with names.
QString userInCookie(const QByteArray &session);

// The pair of keys an account signs in with: "access:secret", or the two
// on lines of their own, as they are copied from the page that shows them.
struct Keys {
    QString access, secret;
    bool both() const { return !access.isEmpty() && !secret.isEmpty(); }
};
Keys keys(const QString &pasted);

// What the signed-in reader made of a work.
struct State {
    int rating = 0;
    QString finished;   // "2024", "2024-05" or "2024-05-17", as much as they said
    qint64 event = 0;   // the finished date's own number, to change it rather than add another
};
QHash<QString, State> parseStates(const QByteArray &json, bool *understood = nullptr);

} // namespace OpenLibrary

class OpenLibraryLog : public ReadingSite {
    Q_OBJECT

public:
    // Tests pass their own server as `base`.
    OpenLibraryLog(const QString &user, QNetworkAccessManager *network, QObject *parent = nullptr,
                   const QString &base = "https://openlibrary.org");

    QString name() const override { return "Open Library"; }
    QString scheme() const override { return "openlibrary"; }
    QString account() const override { return m_user; }

    QString identityFrom(const QString &pasted) const override { return OpenLibrary::userName(pasted); }
    SecretUse secretUse() const override { return OptionalSecret; }
    void setSecret(const QString &keys) override { m_keys = OpenLibrary::keys(keys); }
    // "My Books", which takes whoever is signed in to their own log.
    QUrl findMePage() const override { return QUrl("https://openlibrary.org/account/books"); }
    // Where every account finds its pair of keys.
    QUrl secretPage() const override { return QUrl("https://archive.org/account/s3.php"); }

    void check() override;
    void fetchShelves() override;
    void find(const QString &title, const QString &author) override;
    bool writes() const override { return m_keys.both(); }
    void write(const QList<ReadingSites::Outgoing> &changes) override;

    // The least time between two requests. Open Library asks clients that
    // don't say who to write to for one request a second.
    void setPace(int milliseconds) { m_pace = milliseconds; }

    // Public so the addresses can be tested without the network.
    QUrl shelfUrl(const QString &shelf, int page, int limit = 100) const;
    QUrl findUrl(const QString &title, const QString &author) const;

private:
    using Answer = std::function<void(QNetworkReply *)>;
    void ask(const QUrl &url, const Answer &answer);
    void post(const QUrl &url, const QByteArray &type, const QByteArray &body, const Answer &answer);
    void paced(const std::function<QNetworkReply *()> &send, const Answer &answer);
    // Signs in when there are keys, then carries on; fails when the keys are refused.
    void signedIn(const std::function<void()> &then);
    bool refused(QNetworkReply *reply, bool reading);
    void countShelf(int shelf, SiteProfile profile);
    void fetchPage();
    void fetchStates(int from);
    void writeNext();
    void resolve(const std::function<void()> &then);
    void sendRating();
    void sendShelf();
    void sendFinished();
    void written(const QString &error);

    QString m_user, m_base;
    OpenLibrary::Keys m_keys;
    QByteArray m_session; // the cookie a sign-in answered with; never kept anywhere else
    QNetworkAccessManager *m_network;
    int m_pace = 1100;
    QElapsedTimer m_sinceLast;

    int m_shelf = 0;
    int m_page = 1;
    QList<SiteEntry> m_entries;
    bool m_fetching = false;
    // What is known of the reader's own stars and dates, once signed in.
    QHash<QString, OpenLibrary::State> m_states;
    bool m_statesKnown = false;

    QList<ReadingSites::Outgoing> m_toWrite;
    QList<SiteWritten> m_written;
    QString m_work;       // the work the change in hand is about
    QStringList m_errors; // what went wrong with it so far
};
