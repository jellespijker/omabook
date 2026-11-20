#include "hardcover.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <algorithm>

#include "htmlclean.h"
#include "secretstore.h"

namespace {

constexpr int pageSize = 100;

// Hardcover's statuses: 1 want to read, 2 currently reading, 3 read,
// 4 paused, 5 did not finish, 6 ignored.
QString ourShelf(int status) { return status == 1 ? "to-read" : status == 2 || status == 4 ? "reading" : status == 3 ? "read" : QString(); }
int theirStatus(const QString &shelf) { return shelf == "to-read" ? 1 : shelf == "reading" ? 2 : shelf == "read" ? 3 : 0; }

QByteArray graphql(const QString &query, const QJsonObject &variables = {}) {
    return QJsonDocument(QJsonObject{{"query", query}, {"variables", variables}}).toJson(QJsonDocument::Compact);
}

QJsonObject dataOf(const QByteArray &json, QString *error) {
    const QJsonObject answer = QJsonDocument::fromJson(json).object();
    const QString said = answer.value("errors").toArray().first().toObject().value("message").toString();
    if (error && (!said.isEmpty() || !answer.value("data").isObject()))
        *error = said.isEmpty() ? QString("Hardcover answered in a way Omabook doesn't understand; its API is in beta and may have changed.")
                                : "Hardcover answered: " + said;
    return answer.value("data").toObject();
}

// What a change answered: the entry's number, and the latest read's.
struct Changed {
    qint64 id = 0;
    qint64 read = 0;
    QString error;
};
Changed changedBy(const QByteArray &json, const QString &field) {
    Changed changed;
    const QJsonObject result = dataOf(json, &changed.error).value(field).toObject();
    if (!result.value("error").toString().isEmpty())
        changed.error = result.value("error").toString();
    changed.id = result.value("id").toInteger();
    changed.read = result.value("user_book").toObject().value("user_book_reads").toArray().first().toObject().value("id").toInteger();
    if (changed.id == 0 && changed.error.isEmpty())
        changed.error = "Hardcover didn't take the change.";
    return changed;
}

QNetworkRequest request(const QString &base, const QString &token) {
    QNetworkRequest http = ReadingSites::request(QUrl(base + "/v1/graphql"));
    http.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    http.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    return http;
}

constexpr auto readsBack = "id error user_book { id user_book_reads(order_by: {id: desc}, limit: 1) { id finished_at } }";

} // namespace

QString Hardcover::token(const QString &pasted) {
    static const QRegularExpression bearer("^bearer\\s+", QRegularExpression::CaseInsensitiveOption);
    QString token = pasted.simplified();
    token.remove(bearer);
    return token.remove(' ');
}

QString Hardcover::userName(const QString &pasted) {
    static const QRegularExpression inAddress("hardcover\\.app/@([A-Za-z0-9_.-]+)");
    static const QRegularExpression alone("^@?([A-Za-z0-9_.-]+)$");
    const QString trimmed = pasted.trimmed();
    if (const auto match = inAddress.match(trimmed); match.hasMatch())
        return match.captured(1);
    return alone.match(trimmed).captured(1);
}

Hardcover::Me Hardcover::parseMe(const QByteArray &json, QString *error) {
    Me me;
    // "me" answers a list of one.
    const QJsonValue value = dataOf(json, error).value("me");
    const QJsonObject user = value.isArray() ? value.toArray().first().toObject() : value.toObject();
    me.id = user.value("id").toInteger();
    me.username = user.value("username").toString();
    me.name = user.value("name").toString().simplified();
    if (me.id == 0 && error && error->isEmpty())
        *error = "Hardcover didn't say whose token this is.";
    return me;
}

QList<SiteEntry> Hardcover::parseLibrary(const QByteArray &json, QHash<QString, Ids> *ids, QString *error) {
    QList<SiteEntry> entries;
    const QJsonObject data = dataOf(json, error);
    if (error && error->isEmpty() && !data.value("user_books").isArray())
        *error = "Hardcover answered in a way Omabook doesn't understand; its API is in beta and may have changed.";
    for (const QJsonValue &value : data.value("user_books").toArray()) {
        const QJsonObject row = value.toObject(), book = row.value("book").toObject(), edition = row.value("edition").toObject();
        const int status = row.value("status_id").toInt();
        // "Ignored" is how a reader tells Hardcover not to show them a book.
        if (status == 6)
            continue;
        SiteEntry entry;
        entry.siteId = QString::number(row.value("book_id").toInteger(book.value("id").toInteger()));
        entry.title = book.value("title").toString().simplified();
        for (const QJsonValue &contributor : book.value("cached_contributors").toArray()) {
            const QString role = contributor.toObject().value("contribution").toString();
            const QString name = contributor.toObject().value("author").toObject().value("name").toString().simplified();
            // Translators and narrators are listed too; only authors count.
            if (!name.isEmpty() && (role.isEmpty() || role.compare("Author", Qt::CaseInsensitive) == 0))
                entry.authors << name;
        }
        entry.isbn = edition.value("isbn_13").toString().isEmpty() ? edition.value("isbn_10").toString() : edition.value("isbn_13").toString();
        entry.shelf = ourShelf(status);
        if (status == 5) {
            entry.shelves << "did-not-finish";
            entry.otherShelf = true;
        }
        // Half stars are rounded: Omabook's are whole.
        entry.rating = std::clamp(qRound(row.value("rating").toDouble()), 0, 5);
        const QJsonObject read = row.value("user_book_reads").toArray().first().toObject();
        entry.finished = read.value("finished_at").toString().isEmpty() ? row.value("last_read_date").toString()
                                                                         : read.value("finished_at").toString();
        if (status != 3)
            entry.finished.clear();
        entry.added = row.value("date_added").toString().left(10);
        const int format = edition.value("reading_format_id").toInt();
        entry.binding = format == 2 ? "Audiobook" : format == 4 ? "ebook" : edition.value("edition_format").toString();
        entry.owned = row.value("owned").toBool() ? 1 : 0;
        entry.averageRating = book.value("rating").toDouble();
        if (book.value("release_year").isDouble())
            entry.published = QString::number(book.value("release_year").toInt());
        if (const QString picture = book.value("cached_image").toObject().value("url").toString(); !picture.isEmpty())
            entry.coverUrl = QUrl(picture);
        if (entry.title.isEmpty() || entry.siteId == "0")
            continue;
        if (ids)
            ids->insert(entry.siteId, {row.value("id").toInteger(), read.value("id").toInteger()});
        entries << entry;
    }
    return entries;
}

QList<Hardcover::Found> Hardcover::parseSearch(const QByteArray &json, QString *error) {
    QList<Found> books;
    const QJsonObject search = dataOf(json, error).value("search").toObject();
    if (error && error->isEmpty() && !search.value("error").toString().isEmpty())
        *error = "Hardcover answered: " + search.value("error").toString();
    for (const QJsonValue &hit : search.value("results").toObject().value("hits").toArray()) {
        const QJsonObject document = hit.toObject().value("document").toObject();
        Found found;
        // The number comes as text, or as a number; either is the number.
        found.id = document.value("id").isString() ? document.value("id").toString() : QString::number(document.value("id").toInteger());
        found.slug = document.value("slug").toString();
        BookMeta &meta = found.candidate.meta;
        meta.title = document.value("title").toString().simplified();
        for (const QJsonValue &author : document.value("author_names").toArray())
            if (!author.toString().simplified().isEmpty())
                meta.authors << author.toString().simplified();
        const QJsonObject series = document.value("featured_series").toObject();
        meta.series = series.value("series").toObject().value("name").toString().simplified();
        if (meta.series.isEmpty())
            meta.series = series.value("name").toString().simplified();
        if (!meta.series.isEmpty())
            meta.seriesIndex = document.value("featured_series_position").isDouble() ? document.value("featured_series_position").toDouble()
                                                                                    : series.value("position").toDouble();
        if (document.value("release_year").isDouble())
            meta.published = QString::number(document.value("release_year").toInt());
        const QString description = document.value("description").toString();
        meta.description = description.contains('<') ? htmlToText(description).trimmed() : description.trimmed();
        for (const QJsonValue &genre : document.value("genres").toArray())
            if (meta.tags.size() < 6 && !genre.toString().isEmpty())
                meta.tags << genre.toString();
        for (const QJsonValue &isbn : document.value("isbns").toArray())
            found.isbns << isbn.toString();
        found.rating = document.value("rating").toDouble();
        found.ratings = document.value("ratings_count").toInteger();
        if (const QString picture = document.value("image").toObject().value("url").toString(); !picture.isEmpty())
            found.candidate.coverUrl = QUrl(picture);
        found.candidate.source = "Hardcover";
        if (found.rating > 0)
            found.candidate.why = QString("Hardcover readers rate it %1 out of 5 (%2 ratings).")
                                      .arg(QString::number(found.rating, 'f', 2), QLocale(QLocale::English).toString(found.ratings));
        if (!found.id.isEmpty() && found.id != "0" && !meta.title.isEmpty())
            books << found;
    }
    return books;
}

QByteArray Hardcover::meQuery() { return graphql("query { me { id username name } }"); }

QByteArray Hardcover::countQuery(qint64 user) {
    return graphql("query($user: Int!) { user_books(where: {user_id: {_eq: $user}}) { status_id } }", {{"user", user}});
}

// Authors and the cover come from the copies Hardcover keeps with the book
// itself, which keeps the question shallow: it means to limit how deep one
// may go.
QByteArray Hardcover::libraryQuery(qint64 user, int limit, int offset) {
    return graphql("query($user: Int!, $limit: Int!, $offset: Int!) { "
                   "user_books(where: {user_id: {_eq: $user}}, order_by: [{id: asc}], limit: $limit, offset: $offset) { "
                   "id book_id status_id rating owned date_added last_read_date "
                   "user_book_reads(order_by: {id: desc}, limit: 1) { id finished_at } "
                   "book { id title slug rating release_year cached_contributors cached_image } "
                   "edition { isbn_13 isbn_10 edition_format reading_format_id } } }",
                   {{"user", user}, {"limit", limit}, {"offset", offset}});
}

QByteArray Hardcover::searchQuery(const QString &words, int most) {
    return graphql("query($words: String!, $most: Int!) { search(query: $words, query_type: \"Book\", per_page: $most, page: 1) { results error } }",
                   {{"words", words}, {"most", most}});
}

QByteArray Hardcover::isbnQuery(const QString &isbn) {
    return graphql("query($isbn: String!) { editions(where: {_or: [{isbn_13: {_eq: $isbn}}, {isbn_10: {_eq: $isbn}}]}, "
                   "order_by: {users_count: desc_nulls_last}, limit: 1) { book { id title } } }",
                   {{"isbn", isbn}});
}

QByteArray Hardcover::addQuery(qint64 book, int status, int rating) {
    QJsonObject object{{"book_id", book}};
    if (status > 0) object.insert("status_id", status);
    if (rating > 0) object.insert("rating", rating);
    return graphql(QString("mutation($object: UserBookCreateInput!) { insert_user_book(object: $object) { %1 } }").arg(readsBack),
                   {{"object", object}});
}

QByteArray Hardcover::updateQuery(qint64 userBook, int status, int rating) {
    QJsonObject object;
    if (status > 0) object.insert("status_id", status);
    if (rating > 0) object.insert("rating", rating);
    return graphql(QString("mutation($id: Int!, $object: UserBookUpdateInput!) { update_user_book(id: $id, object: $object) { %1 } }")
                       .arg(readsBack),
                   {{"id", userBook}, {"object", object}});
}

QByteArray Hardcover::readQuery(qint64 userBook, qint64 read, const QString &finished) {
    // "2024" and "2024-05" are days too, to Hardcover, with how exact they are beside them.
    const QStringList parts = finished.split('-', Qt::SkipEmptyParts);
    const QJsonObject dates{{"finished_at", QString("%1-%2-%3").arg(parts.value(0), parts.value(1, "01"), parts.value(2, "01"))},
                            {"finished_at_precision", parts.size() >= 3 ? 1 : parts.size() == 2 ? 2 : 3}};
    if (read > 0)
        return graphql("mutation($id: Int!, $dates: DatesReadInput!) { update_user_book_read(id: $id, object: $dates) { id error } }",
                       {{"id", read}, {"dates", dates}});
    return graphql("mutation($id: Int!, $dates: DatesReadInput!) { insert_user_book_read(user_book_id: $id, user_book_read: $dates) { id error } }",
                   {{"id", userBook}, {"dates", dates}});
}

QString Hardcover::problem(QNetworkReply *reply, const QByteArray &body, SiteProblem *kind) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QJsonObject answer = QJsonDocument::fromJson(body).object();
    // Without a token it will read, Hardcover answers "invalid_request" or "invalid_token".
    if (status == 401 || (status == 400 && answer.value("error").toString().startsWith("invalid_"))) {
        *kind = SiteProblem::Secret;
        return "Hardcover didn't take the token: it is wrong, was revoked or has run out. Make a new one and connect again.";
    }
    if (status == 403) {
        *kind = SiteProblem::Secret;
        return "The Hardcover token isn't allowed to do this" + (answer.value("scope").toString().isEmpty()
                                                                     ? QString() : " (it lacks " + answer.value("scope").toString() + ")")
               + ". Make one with the page Omabook opens, which ticks what is needed.";
    }
    if (status == 429 || status == 503) {
        *kind = SiteProblem::Refused;
        return "Hardcover is turning requests away for now (it allows sixty a minute); try again in a moment.";
    }
    if (reply->error() != QNetworkReply::NoError) {
        *kind = status == 0 ? SiteProblem::Unreachable : SiteProblem::Changed;
        return "Hardcover can't be reached: " + reply->errorString();
    }
    QString error;
    dataOf(body, &error);
    if (!error.isEmpty())
        *kind = SiteProblem::Changed;
    return error;
}

HardcoverSite::HardcoverSite(const QString &user, QNetworkAccessManager *network, QObject *parent, const QString &base)
    : ReadingSite(parent), m_user(Hardcover::userName(user)), m_base(base), m_network(network) {}

QUrl HardcoverSite::secretPage() const {
    return QUrl("https://hardcover.app/account/api/keys/new?scope=read:me:content+read:library+write:library+read:catalog");
}

void HardcoverSite::ask(const QByteArray &query, const Answer &answer) {
    const qint64 wait = m_sinceLast.isValid() ? qMax<qint64>(0, m_pace - m_sinceLast.elapsed()) : 0;
    QTimer::singleShot(wait, this, [this, query, answer] {
        m_sinceLast.start();
        QNetworkReply *reply = m_network->post(request(m_base, m_token), query);
        connect(reply, &QNetworkReply::finished, this, [reply, answer] {
            reply->deleteLater();
            answer(reply, reply->readAll());
        });
    });
}

bool HardcoverSite::refused(QNetworkReply *reply, const QByteArray &body) {
    SiteProblem kind = SiteProblem::None;
    const QString problem = Hardcover::problem(reply, body, &kind);
    if (problem.isEmpty())
        return false;
    m_fetching = false;
    fail(kind, problem);
    return true;
}

void HardcoverSite::known(const std::function<void()> &then) {
    clearProblem();
    if (m_token.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] {
            m_fetching = false;
            fail(SiteProblem::Secret, "Hardcover needs a token: make one in your account there and connect with it.");
        }, Qt::QueuedConnection);
        return;
    }
    if (m_userId > 0) {
        then();
        return;
    }
    ask(Hardcover::meQuery(), [this, then](QNetworkReply *reply, const QByteArray &body) {
        if (refused(reply, body))
            return;
        QString error;
        const Hardcover::Me me = Hardcover::parseMe(body, &error);
        if (me.id == 0) {
            m_fetching = false;
            fail(SiteProblem::Changed, error);
            return;
        }
        m_userId = me.id;
        m_user = me.username;
        m_name = me.name.isEmpty() ? me.username : me.name;
        then();
    });
}

void HardcoverSite::check() {
    known([this] {
        ask(Hardcover::countQuery(m_userId), [this](QNetworkReply *reply, const QByteArray &body) {
            if (refused(reply, body))
                return;
            SiteProfile profile;
            profile.identity = m_user;
            profile.name = m_name;
            profile.books = 0;
            QSet<int> statuses;
            for (const QJsonValue &row : QJsonDocument::fromJson(body).object().value("data").toObject().value("user_books").toArray()) {
                const int status = row.toObject().value("status_id").toInt();
                if (status == 6)
                    continue;
                ++profile.books;
                statuses << status;
            }
            profile.shelves = statuses.size();
            emit checked(profile);
        });
    });
}

void HardcoverSite::fetchShelves() {
    if (m_fetching)
        return;
    m_fetching = true;
    m_entries.clear();
    m_ids.clear();
    known([this] { fetchPage(0); });
}

void HardcoverSite::fetchPage(int offset) {
    ask(Hardcover::libraryQuery(m_userId, pageSize, offset), [this, offset](QNetworkReply *reply, const QByteArray &body) {
        if (refused(reply, body))
            return;
        const int rows = QJsonDocument::fromJson(body).object().value("data").toObject().value("user_books").toArray().size();
        m_entries += Hardcover::parseLibrary(body, &m_ids);
        // A page that isn't full is the last; a library has no end to ask for.
        if (rows >= pageSize && offset < 100000) {
            fetchPage(offset + pageSize);
            return;
        }
        m_fetching = false;
        emit fetched(m_entries);
    });
}

void HardcoverSite::find(const QString &title, const QString &author) {
    if (m_token.isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { emit findFailed("Hardcover needs a token."); }, Qt::QueuedConnection);
        return;
    }
    ask(Hardcover::searchQuery((title + " " + author).simplified()), [this](QNetworkReply *reply, const QByteArray &body) {
        SiteProblem kind = SiteProblem::None;
        if (const QString problem = Hardcover::problem(reply, body, &kind); !problem.isEmpty()) {
            emit findFailed(problem);
            return;
        }
        QList<SiteBook> books;
        for (const Hardcover::Found &answer : Hardcover::parseSearch(body)) {
            SiteBook book;
            book.site = name();
            book.scheme = scheme();
            book.siteId = answer.id;
            book.title = answer.candidate.meta.title;
            book.authors = answer.candidate.meta.authors;
            book.averageRating = answer.rating;
            book.ratings = answer.ratings;
            book.coverUrl = answer.candidate.coverUrl;
            book.url = QUrl(answer.slug.isEmpty() ? "https://hardcover.app/id/book/" + answer.id : "https://hardcover.app/books/" + answer.slug);
            books << book;
        }
        emit found(books);
    });
}

void HardcoverSite::write(const QList<ReadingSites::Outgoing> &changes) {
    if (!writes()) {
        ReadingSite::write(changes);
        return;
    }
    m_toWrite = changes;
    m_written.clear();
    known([this] { writeNext(); });
}

void HardcoverSite::written(const QString &error) {
    m_written.append({m_toWrite.first().bookId, m_book, error});
    m_toWrite.removeFirst();
    writeNext();
}

void HardcoverSite::writeNext() {
    if (m_toWrite.isEmpty()) {
        emit wrote(m_written);
        return;
    }
    const ReadingSites::Outgoing change = m_toWrite.first();
    m_book = change.siteId;
    const int status = theirStatus(change.shelf);
    auto after = [this, change](const QByteArray &body, const QString &field) {
        const Changed changed = changedBy(body, field);
        if (!changed.error.isEmpty()) {
            written(changed.error);
            return;
        }
        m_ids.insert(m_book, {changed.id, changed.read});
        sendFinished(changed.id, changed.read);
    };
    // A book already in their library is changed; one that isn't is added.
    if (const Hardcover::Ids ids = m_ids.value(m_book); ids.userBook > 0) {
        if (status == 0 && change.rating == 0) {
            sendFinished(ids.userBook, ids.read);
            return;
        }
        ask(Hardcover::updateQuery(ids.userBook, status, change.rating), [this, after](QNetworkReply *reply, const QByteArray &body) {
            SiteProblem kind = SiteProblem::None;
            if (const QString problem = Hardcover::problem(reply, body, &kind); !problem.isEmpty())
                written(problem);
            else
                after(body, "update_user_book");
        });
        return;
    }
    resolve([this, change, status, after](qint64 book) {
        m_book = QString::number(book);
        ask(Hardcover::addQuery(book, status == 0 ? theirStatus(change.status) : status, change.rating),
            [this, after](QNetworkReply *reply, const QByteArray &body) {
                SiteProblem kind = SiteProblem::None;
                if (const QString problem = Hardcover::problem(reply, body, &kind); !problem.isEmpty())
                    written(problem);
                else
                    after(body, "insert_user_book");
            });
    });
}

// Which of Hardcover's books a book is that their library doesn't hold yet:
// by its number when that is known, by ISBN, else by title and author.
void HardcoverSite::resolve(const std::function<void(qint64 book)> &then) {
    const ReadingSites::Outgoing change = m_toWrite.first();
    if (const qint64 book = m_book.toLongLong(); book > 0) {
        then(book);
        return;
    }
    auto byTitle = [this, change, then] {
        ask(Hardcover::searchQuery((ReadingSites::splitSeries(change.title).title + " " + change.authors.value(0)).simplified()),
            [this, change, then](QNetworkReply *, const QByteArray &body) {
                for (const Hardcover::Found &found : Hardcover::parseSearch(body)) {
                    if (ReadingSites::sameTitle(change.title, found.candidate.meta.title)
                        && ReadingSites::sameAuthor(change.authors.value(0), found.candidate.meta.authors)) {
                        then(found.id.toLongLong());
                        return;
                    }
                }
                written("Hardcover doesn't know this book.");
            });
    };
    const QString isbn = ReadingSites::isbn13(change.isbn);
    if (isbn.isEmpty()) {
        byTitle();
        return;
    }
    ask(Hardcover::isbnQuery(isbn), [then, byTitle](QNetworkReply *, const QByteArray &body) {
        const qint64 book = QJsonDocument::fromJson(body).object().value("data").toObject().value("editions").toArray().first()
                                .toObject().value("book").toObject().value("id").toInteger();
        book > 0 ? then(book) : byTitle();
    });
}

void HardcoverSite::sendFinished(qint64 userBook, qint64 read) {
    const ReadingSites::Outgoing change = m_toWrite.first();
    if (change.finished.isEmpty() || change.finished.left(4).toInt() == 0) {
        written({});
        return;
    }
    ask(Hardcover::readQuery(userBook, read, change.finished), [this, read](QNetworkReply *reply, const QByteArray &body) {
        SiteProblem kind = SiteProblem::None;
        QString error = Hardcover::problem(reply, body, &kind);
        if (error.isEmpty())
            error = changedBy(body, read > 0 ? "update_user_book_read" : "insert_user_book_read").error;
        written(error.isEmpty() ? QString() : "the finished date was not taken: " + error);
    });
}

void HardcoverSource::search(const BookMeta &book) {
    const QString token = m_token.isEmpty() ? Hardcover::token(Secrets::shared().get("hardcover")) : m_token;
    const QString isbn = ReadingSites::isbn13(book.isbn);
    const QString words = !isbn.isEmpty() ? isbn : (ReadingSites::splitSeries(book.title).title + " " + book.authors.value(0)).simplified();
    if (token.isEmpty() || words.isEmpty()) {
        QMetaObject::invokeMethod(this, [this, token] {
            if (token.isEmpty())
                emit failed(Failure(Failure::Code::SourceNeedsToken, {name()}));
            else
                emit found({});
        }, Qt::QueuedConnection);
        return;
    }
    QNetworkReply *reply = m_network->post(request(m_base, token), Hardcover::searchQuery(words, 4));
    connect(reply, &QNetworkReply::finished, this, [this, reply, book, isbn] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        SiteProblem kind = SiteProblem::None;
        if (const QString problem = Hardcover::problem(reply, body, &kind); !problem.isEmpty()) {
            emit failed(Failure::said(problem));
            return;
        }
        QList<MetaCandidate> candidates;
        for (const Hardcover::Found &answer : Hardcover::parseSearch(body)) {
            // Asked by ISBN, what comes back is the book; asked by title,
            // only what shares the title or the author is.
            if (isbn.isEmpty() && !ReadingSites::sameTitle(book.title, answer.candidate.meta.title)
                && !(!book.authors.isEmpty() && ReadingSites::sameAuthor(book.authors.first(), answer.candidate.meta.authors)))
                continue;
            MetaCandidate candidate = answer.candidate;
            // The search lists the ISBNs of every edition; the one that was
            // asked for is the only one known to be this book's.
            if (!isbn.isEmpty() && answer.isbns.contains(isbn))
                candidate.meta.isbn = isbn;
            candidates << candidate;
        }
        emit found(candidates);
    });
}
