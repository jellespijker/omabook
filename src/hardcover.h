#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QString>
#include <functional>

#include "metadatasource.h"
#include "readingsite.h"

class QNetworkReply;

// Hardcover (hardcover.app), through its GraphQL API and a token the reader
// makes in their own account. The API is in beta and says so: every answer
// is read defensively, and a shape that changed fails with a reason. The
// token allows sixty requests a minute, shared with everything else the
// reader uses it for, so requests go out one at a time and no faster.
namespace Hardcover {

// The token as it is sent, from however it was copied: with or without
// "Bearer" in front, with the spaces and line breaks a page adds.
QString token(const QString &pasted);
// Who somebody is there: "hardcover.app/@ann", "@ann" or "ann".
QString userName(const QString &pasted);

struct Me {
    qint64 id = 0;
    QString username;
    QString name;
};
// The answer to "who am I"; `error` says what the API said instead.
Me parseMe(const QByteArray &json, QString *error = nullptr);

// What is needed to change a book that is in somebody's library there.
struct Ids {
    qint64 userBook = 0; // their entry for the book
    qint64 read = 0;     // the latest time they read it
};
// One page of a library. Books the reader told Hardcover to ignore are left out.
QList<SiteEntry> parseLibrary(const QByteArray &json, QHash<QString, Ids> *ids = nullptr, QString *error = nullptr);

// A book as the search knows it.
struct Found {
    MetaCandidate candidate;
    QString id;
    QString slug;
    QStringList isbns;
    double rating = 0;
    qint64 ratings = 0;
};
QList<Found> parseSearch(const QByteArray &json, QString *error = nullptr);

// The requests, as they are sent.
QByteArray meQuery();
QByteArray countQuery(qint64 user);
QByteArray libraryQuery(qint64 user, int limit, int offset);
QByteArray searchQuery(const QString &words, int most = 5);
QByteArray isbnQuery(const QString &isbn);
QByteArray addQuery(qint64 book, int status, int rating);
QByteArray updateQuery(qint64 userBook, int status, int rating);
QByteArray readQuery(qint64 userBook, qint64 read, const QString &finished);

// What a request ran into, and which kind of trouble it is; empty when the
// answer can be read. `body` is the answer's.
QString problem(QNetworkReply *reply, const QByteArray &body, SiteProblem *kind);

} // namespace Hardcover

class HardcoverSite : public ReadingSite {
    Q_OBJECT

public:
    // Tests pass their own server as `base`.
    HardcoverSite(const QString &user, QNetworkAccessManager *network, QObject *parent = nullptr,
                  const QString &base = "https://api.hardcover.app");

    QString name() const override { return "Hardcover"; }
    QString scheme() const override { return "hardcover"; }
    QString account() const override { return m_user; }

    QString identityFrom(const QString &pasted) const override { return Hardcover::userName(pasted); }
    SecretUse secretUse() const override { return NeedsSecret; }
    void setSecret(const QString &token) override { m_token = Hardcover::token(token); }
    QUrl findMePage() const override { return QUrl("https://hardcover.app/account"); }
    // The page that makes a token, with only what Omabook uses ticked: who
    // you are, your library to read and to write, and the catalog.
    QUrl secretPage() const override;

    void check() override;
    void fetchShelves() override;
    void find(const QString &title, const QString &author) override;
    bool writes() const override { return !m_token.isEmpty(); }
    void write(const QList<ReadingSites::Outgoing> &changes) override;

    // The least time between two requests.
    void setPace(int milliseconds) { m_pace = milliseconds; }

private:
    using Answer = std::function<void(QNetworkReply *, const QByteArray &body)>;
    void ask(const QByteArray &query, const Answer &answer);
    // Finds out who the token is for, once, then carries on.
    void known(const std::function<void()> &then);
    bool refused(QNetworkReply *reply, const QByteArray &body);
    void fetchPage(int offset);
    void writeNext();
    void resolve(const std::function<void(qint64 book)> &then);
    void sendFinished(qint64 userBook, qint64 read);
    void written(const QString &error);

    QString m_user, m_token, m_base;
    qint64 m_userId = 0;
    QString m_name;
    QNetworkAccessManager *m_network;
    int m_pace = 1100;
    QElapsedTimer m_sinceLast;

    QList<SiteEntry> m_entries;
    QHash<QString, Hardcover::Ids> m_ids;
    bool m_fetching = false;

    QList<ReadingSites::Outgoing> m_toWrite;
    QList<SiteWritten> m_written;
    QString m_book; // the book the change in hand is about, as Hardcover numbers it
};

// Hardcover as a place to look a book's details up: one search, by ISBN or
// by title and author. Its catalog is its readers' work and free to use;
// what they rate a book is shown as Hardcover's.
class HardcoverSource : public MetadataSource {
    Q_OBJECT

public:
    // Without a token of its own it takes the one the reader connected with.
    explicit HardcoverSource(QNetworkAccessManager *network, QObject *parent = nullptr,
                             const QString &base = "https://api.hardcover.app", const QString &token = {})
        : MetadataSource(parent), m_network(network), m_base(base), m_token(Hardcover::token(token)) {}

    QString name() const override { return "Hardcover"; }
    QStringList fields() const override { return {"title", "authors", "series", "published", "isbn", "description", "tags", "cover"}; }
    void search(const BookMeta &book) override;

private:
    QNetworkAccessManager *m_network;
    QString m_base, m_token;
};
