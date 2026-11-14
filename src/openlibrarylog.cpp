#include "openlibrarylog.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>

namespace {

struct Shelf {
    const char *theirs;
    const char *ours;
    int number; // what the shelf is called when a book is put on it
};
// "Stopped reading" is no status Omabook has; such a book comes in with
// that as a tag, and is never written back as anything else.
constexpr Shelf shelves[] = {{"already-read", "read", 3}, {"currently-reading", "reading", 2}, {"want-to-read", "to-read", 1},
                             {"stopped-reading", "", 4}};
constexpr int pageSize = 100;

int shelfNumber(const QString &ours) {
    for (const Shelf &shelf : shelves)
        if (!ours.isEmpty() && ours == shelf.ours)
            return shelf.number;
    return 0;
}

// What went wrong with a request, and which kind of wrong; empty when nothing did.
QString problemOf(QNetworkReply *reply, const QString &user, SiteProblem *kind) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401) {
        *kind = SiteProblem::Secret;
        return "Open Library no longer takes this sign-in. Connect again with your keys from archive.org/account/s3.php.";
    }
    if (status == 403) {
        *kind = SiteProblem::Private;
        return QString("%1's reading log on Open Library is private. Sign in with your keys to read it, or make it public "
                       "(openlibrary.org, Settings, Privacy).").arg(user);
    }
    if (status == 404) {
        *kind = SiteProblem::NoUser;
        return QString("Open Library knows nobody called %1. Open My Books there and copy the address.").arg(user);
    }
    if (status == 429 || status == 503) {
        *kind = SiteProblem::Refused;
        return "Open Library is turning requests away for now; try again later.";
    }
    if (reply->error() != QNetworkReply::NoError) {
        *kind = status == 0 ? SiteProblem::Unreachable : SiteProblem::Changed;
        return "Open Library can't be reached: " + reply->errorString();
    }
    return {};
}

} // namespace

QList<SiteEntry> OpenLibrary::parseLog(const QByteArray &json, const QString &shelf, int *total, bool *understood) {
    QList<SiteEntry> entries;
    const QJsonObject log = QJsonDocument::fromJson(json).object();
    if (understood)
        *understood = log.value("reading_log_entries").isArray();
    if (total)
        *total = log.value("numFound").toInt();
    for (const QJsonValue &value : log.value("reading_log_entries").toArray()) {
        const QJsonObject work = value.toObject().value("work").toObject();
        SiteEntry entry;
        // "/works/OL8193416W" is known everywhere else as OL8193416W.
        entry.siteId = work.value("key").toString().section('/', -1);
        entry.title = work.value("title").toString().simplified();
        for (const QJsonValue &author : work.value("author_names").toArray())
            if (!author.toString().simplified().isEmpty())
                entry.authors << author.toString().simplified();
        entry.shelf = shelf;
        if (work.value("first_publish_year").isDouble())
            entry.published = QString::number(work.value("first_publish_year").toInt());
        // "2026/10/22, 00:24:25"
        const QDate logged = QDate::fromString(value.toObject().value("logged_date").toString().left(10), "yyyy/MM/dd");
        if (logged.isValid())
            entry.added = logged.toString(Qt::ISODate);
        if (work.value("cover_id").isDouble())
            entry.coverUrl = QUrl(QString("https://covers.openlibrary.org/b/id/%1-L.jpg").arg(work.value("cover_id").toInteger()));
        if (!entry.title.isEmpty() && !entry.siteId.isEmpty())
            entries << entry;
    }
    return entries;
}

QList<SiteBook> OpenLibrary::parseSearch(const QByteArray &json, bool *understood) {
    QList<SiteBook> books;
    const QJsonObject answer = QJsonDocument::fromJson(json).object();
    if (understood)
        *understood = answer.value("docs").isArray();
    for (const QJsonValue &value : answer.value("docs").toArray()) {
        const QJsonObject doc = value.toObject();
        SiteBook book;
        book.site = "Open Library";
        book.scheme = "openlibrary";
        book.siteId = doc.value("key").toString().section('/', -1);
        book.title = doc.value("title").toString().simplified();
        for (const QJsonValue &author : doc.value("author_name").toArray())
            book.authors << author.toString().simplified();
        book.averageRating = doc.value("ratings_average").toDouble();
        book.ratings = doc.value("ratings_count").toInteger();
        if (doc.value("cover_i").isDouble())
            book.coverUrl = QUrl(QString("https://covers.openlibrary.org/b/id/%1-L.jpg").arg(doc.value("cover_i").toInteger()));
        book.url = QUrl("https://openlibrary.org/works/" + book.siteId);
        if (!book.title.isEmpty() && !book.siteId.isEmpty())
            books << book;
    }
    return books;
}

QString OpenLibrary::userName(const QString &pasted) {
    static const QRegularExpression inAddress("people/([^/?#\\s]+)");
    static const QRegularExpression alone("^@?([A-Za-z0-9_.-]+)$");
    const QString trimmed = pasted.trimmed();
    if (const auto match = inAddress.match(trimmed); match.hasMatch())
        return match.captured(1);
    return alone.match(trimmed).captured(1);
}

QString OpenLibrary::userInCookie(const QByteArray &session) {
    // "/people/ann,2026-11-14T10:00:00,ab12c$…", percent-encoded.
    return userName(QString::fromUtf8(QByteArray::fromPercentEncoding(session)).section(',', 0, 0));
}

OpenLibrary::Keys OpenLibrary::keys(const QString &pasted) {
    static const QRegularExpression between("[\\s:,;]+");
    const QStringList parts = pasted.trimmed().split(between, Qt::SkipEmptyParts);
    return parts.size() == 2 ? Keys{parts.at(0), parts.at(1)} : Keys{};
}

QHash<QString, OpenLibrary::State> OpenLibrary::parseStates(const QByteArray &json, bool *understood) {
    QHash<QString, State> states;
    const QJsonObject answer = QJsonDocument::fromJson(json).object();
    if (understood)
        *understood = answer.value("works").isObject();
    const QJsonObject works = answer.value("works").toObject();
    for (auto it = works.begin(); it != works.end(); ++it) {
        const QJsonObject work = it.value().toObject();
        State state;
        state.rating = std::clamp(work.value("rating").toInt(), 0, 5);
        state.finished = work.value("read_date").toString();
        state.event = work.value("event_id").toInteger();
        states.insert(it.key().section('/', -1), state);
    }
    return states;
}

OpenLibraryLog::OpenLibraryLog(const QString &user, QNetworkAccessManager *network, QObject *parent, const QString &base)
    : ReadingSite(parent), m_user(OpenLibrary::userName(user)), m_base(base), m_network(network) {}

QUrl OpenLibraryLog::shelfUrl(const QString &shelf, int page, int limit) const {
    QList<QPair<QString, QString>> items{{"page", QString::number(page)}};
    if (limit != pageSize)
        items.emplaceBack("limit", QString::number(limit));
    return ReadingSites::address(m_base + "/people/" + QString::fromLatin1(QUrl::toPercentEncoding(m_user)) + "/books/" + shelf + ".json",
                                 items);
}

QUrl OpenLibraryLog::findUrl(const QString &title, const QString &author) const {
    QList<QPair<QString, QString>> items{{"title", title}};
    if (!author.trimmed().isEmpty())
        items.emplaceBack("author", author);
    items.emplaceBack("limit", "3");
    items.emplaceBack("fields", "key,title,author_name,ratings_average,ratings_count,cover_i");
    return ReadingSites::address(m_base + "/search.json", items);
}

// One request at a time, and no sooner after the last than the pace allows.
void OpenLibraryLog::paced(const std::function<QNetworkReply *()> &send, const Answer &answer) {
    const qint64 wait = m_sinceLast.isValid() ? qMax<qint64>(0, m_pace - m_sinceLast.elapsed()) : 0;
    QTimer::singleShot(wait, this, [this, send, answer] {
        m_sinceLast.start();
        QNetworkReply *reply = send();
        connect(reply, &QNetworkReply::finished, this, [reply, answer] {
            reply->deleteLater();
            answer(reply);
        });
    });
}

namespace {

// The sign-in travels with each request by hand, and never through the
// cookies the app's other requests share: it is gone with this object.
QNetworkRequest withSession(const QUrl &url, const QByteArray &session) {
    QNetworkRequest request = ReadingSites::request(url);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (!session.isEmpty())
        request.setRawHeader("Cookie", "session=" + session);
    return request;
}

} // namespace

void OpenLibraryLog::ask(const QUrl &url, const Answer &answer) {
    paced([this, url] { return m_network->get(withSession(url, m_session)); }, answer);
}

void OpenLibraryLog::post(const QUrl &url, const QByteArray &type, const QByteArray &body, const Answer &answer) {
    paced([this, url, type, body] {
        QNetworkRequest request = withSession(url, m_session);
        request.setHeader(QNetworkRequest::ContentTypeHeader, type);
        return m_network->post(request, body);
    }, answer);
}

void OpenLibraryLog::signedIn(const std::function<void()> &then) {
    if (!m_keys.both() || !m_session.isEmpty()) {
        then();
        return;
    }
    const QByteArray body = QJsonDocument(QJsonObject{{"access", m_keys.access}, {"secret", m_keys.secret}}).toJson(QJsonDocument::Compact);
    post(QUrl(m_base + "/account/login"), "application/json", body, [this, then](QNetworkReply *reply) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        for (const QNetworkCookie &cookie : reply->header(QNetworkRequest::SetCookieHeader).value<QList<QNetworkCookie>>())
            if (cookie.name() == "session")
                m_session = cookie.value();
        if (m_session.isEmpty()) {
            m_fetching = false;
            const QString said = QJsonDocument::fromJson(reply->readAll()).object().value("errorDisplayString").toString();
            if (status == 0)
                fail(SiteProblem::Unreachable, "Open Library can't be reached: " + reply->errorString());
            else
                fail(SiteProblem::Secret, "Open Library didn't take these keys" + (said.isEmpty() ? QString(".") : ": " + said)
                                              + " Copy both from archive.org/account/s3.php.");
            return;
        }
        // The keys say whose log this is, whatever name was typed.
        if (const QString user = OpenLibrary::userInCookie(m_session); !user.isEmpty())
            m_user = user;
        then();
    });
}

bool OpenLibraryLog::refused(QNetworkReply *reply, bool) {
    SiteProblem kind = SiteProblem::None;
    const QString problem = problemOf(reply, m_user, &kind);
    if (problem.isEmpty())
        return false;
    m_fetching = false;
    fail(kind, problem);
    return true;
}

void OpenLibraryLog::check() {
    clearProblem();
    if (m_user.isEmpty() && !m_keys.both()) {
        QMetaObject::invokeMethod(this, [this] {
            fail(SiteProblem::NoIdentity, "Open Library doesn't know whose reading log to read: give your user name, or sign in with your keys.");
        }, Qt::QueuedConnection);
        return;
    }
    signedIn([this] { countShelf(0, {}); });
}

// A shelf asked for one book says how many it holds.
void OpenLibraryLog::countShelf(int shelf, SiteProfile profile) {
    if (shelf >= int(std::size(shelves))) {
        profile.identity = m_user;
        profile.name = m_user;
        emit checked(profile);
        return;
    }
    ask(shelfUrl(shelves[shelf].theirs, 1, 1), [this, shelf, profile](QNetworkReply *reply) mutable {
        if (refused(reply, true))
            return;
        int total = 0;
        bool understood = false;
        OpenLibrary::parseLog(reply->readAll(), {}, &total, &understood);
        if (!understood) {
            fail(SiteProblem::Changed, "Open Library answered in a way Omabook doesn't understand.");
            return;
        }
        profile.books = qMax(0, profile.books) + total;
        if (total > 0)
            ++profile.shelves;
        countShelf(shelf + 1, profile);
    });
}

void OpenLibraryLog::fetchShelves() {
    if (m_fetching)
        return;
    clearProblem();
    if (m_user.isEmpty() && !m_keys.both()) {
        QMetaObject::invokeMethod(this, [this] {
            fail(SiteProblem::NoIdentity, "Open Library doesn't know whose reading log to read: put your user name in the settings (openlibrary_user).");
        }, Qt::QueuedConnection);
        return;
    }
    m_fetching = true;
    m_shelf = 0;
    m_page = 1;
    m_entries.clear();
    m_states.clear();
    m_statesKnown = false;
    signedIn([this] { fetchPage(); });
}

void OpenLibraryLog::fetchPage() {
    const Shelf shelf = shelves[m_shelf];
    ask(shelfUrl(shelf.theirs, m_page), [this, shelf](QNetworkReply *reply) {
        if (refused(reply, true))
            return;
        bool understood = false;
        QList<SiteEntry> page = OpenLibrary::parseLog(reply->readAll(), shelf.ours, nullptr, &understood);
        if (!understood) {
            m_fetching = false;
            fail(SiteProblem::Changed, "Open Library answered in a way Omabook doesn't understand.");
            return;
        }
        for (SiteEntry &entry : page) {
            if (entry.shelf.isEmpty()) {
                entry.shelves << shelf.theirs;
                entry.otherShelf = true;
            }
        }
        m_entries += page;
        // The count a shelf gives of itself can't be trusted on a short
        // page; a page that isn't full is the last.
        if (page.size() >= pageSize && m_page < 500) {
            ++m_page;
        } else {
            m_page = 1;
            ++m_shelf;
        }
        if (m_shelf < int(std::size(shelves)))
            fetchPage();
        else if (!m_session.isEmpty() && !m_entries.isEmpty())
            fetchStates(0);
        else {
            m_fetching = false;
            emit fetched(m_entries);
        }
    });
}

// The log itself has no stars and no dates; those come a hundred works at a
// time from what the site's own pages use, which is promised to nobody. If
// it stops answering, the shelves still come in, without them.
void OpenLibraryLog::fetchStates(int from) {
    QStringList works;
    for (const SiteEntry &entry : m_entries.mid(from, pageSize))
        works << entry.siteId;
    auto finish = [this] {
        for (SiteEntry &entry : m_entries) {
            const OpenLibrary::State state = m_states.value(entry.siteId);
            entry.rating = state.rating;
            entry.finished = state.finished;
        }
        m_fetching = false;
        emit fetched(m_entries);
    };
    if (works.isEmpty()) {
        m_statesKnown = true;
        finish();
        return;
    }
    ask(ReadingSites::address(m_base + "/partials/ReadingState.json", {{"work_ids", works.join(',')}}),
        [this, from, finish](QNetworkReply *reply) {
            bool understood = false;
            const auto states = reply->error() == QNetworkReply::NoError ? OpenLibrary::parseStates(reply->readAll(), &understood)
                                                                         : QHash<QString, OpenLibrary::State>();
            if (!understood) {
                m_states.clear();
                finish();
                return;
            }
            m_states.insert(states);
            fetchStates(from + pageSize);
        });
}

void OpenLibraryLog::find(const QString &title, const QString &author) {
    ask(findUrl(title, author), [this](QNetworkReply *reply) {
        SiteProblem kind = SiteProblem::None;
        if (const QString problem = problemOf(reply, m_user, &kind); !problem.isEmpty()) {
            emit findFailed(problem);
            return;
        }
        bool understood = false;
        const QList<SiteBook> books = OpenLibrary::parseSearch(reply->readAll(), &understood);
        if (understood)
            emit found(books);
        else
            emit findFailed("Open Library answered in a way Omabook doesn't understand.");
    });
}

void OpenLibraryLog::write(const QList<ReadingSites::Outgoing> &changes) {
    if (!writes()) {
        ReadingSite::write(changes);
        return;
    }
    clearProblem();
    m_toWrite = changes;
    m_written.clear();
    signedIn([this] { writeNext(); });
}

void OpenLibraryLog::writeNext() {
    if (m_toWrite.isEmpty()) {
        emit wrote(m_written);
        return;
    }
    m_work = m_toWrite.first().siteId;
    m_errors.clear();
    resolve([this] { sendRating(); });
}

void OpenLibraryLog::written(const QString &error) {
    if (!error.isEmpty())
        m_errors << error;
    m_written.append({m_toWrite.first().bookId, m_work, m_errors.join("; ")});
    m_toWrite.removeFirst();
    writeNext();
}

// Which work a book is that the log doesn't list yet: by ISBN, else by its
// title and author.
void OpenLibraryLog::resolve(const std::function<void()> &then) {
    if (!m_work.isEmpty()) {
        then();
        return;
    }
    const ReadingSites::Outgoing change = m_toWrite.first();
    auto byTitle = [this, change, then] {
        ask(findUrl(ReadingSites::splitSeries(change.title).title, change.authors.value(0)), [this, change, then](QNetworkReply *reply) {
            for (const SiteBook &book : OpenLibrary::parseSearch(reply->readAll()))
                if (m_work.isEmpty() && ReadingSites::sameTitle(change.title, book.title)
                    && ReadingSites::sameAuthor(change.authors.value(0), book.authors))
                    m_work = book.siteId;
            if (m_work.isEmpty())
                written("Open Library doesn't know this book.");
            else
                then();
        });
    };
    const QString isbn = ReadingSites::isbn13(change.isbn);
    if (isbn.isEmpty()) {
        byTitle();
        return;
    }
    ask(ReadingSites::address(m_base + "/search.json", {{"q", "isbn:" + isbn}, {"fields", "key,title,author_name"}, {"limit", "1"}}),
        [this, then, byTitle](QNetworkReply *reply) {
            m_work = OpenLibrary::parseSearch(reply->readAll()).value(0).siteId;
            m_work.isEmpty() ? byTitle() : then();
        });
}

void OpenLibraryLog::sendRating() {
    const ReadingSites::Outgoing change = m_toWrite.first();
    if (change.rating == 0) {
        sendShelf();
        return;
    }
    post(QUrl(m_base + "/works/" + m_work + "/ratings.json"), "application/x-www-form-urlencoded",
         "rating=" + QByteArray::number(change.rating), [this](QNetworkReply *reply) {
             if (!QJsonDocument::fromJson(reply->readAll()).object().contains("success"))
                 m_errors << "the stars were not taken";
             sendShelf();
         });
}

void OpenLibraryLog::sendShelf() {
    const ReadingSites::Outgoing change = m_toWrite.first();
    // Rating a book that is wanted moves it to "already read" there; it is
    // put back on the shelf it is on here.
    const QString shelf = !change.shelf.isEmpty() ? change.shelf : change.rating > 0 && change.status == "to-read" ? change.status : QString();
    const int number = shelfNumber(shelf);
    if (number == 0) {
        sendFinished();
        return;
    }
    // "dont_remove": without it, putting a book on the shelf it is already
    // on takes it off, and its dates with it.
    post(QUrl(m_base + "/works/" + m_work + "/bookshelves.json"), "application/x-www-form-urlencoded",
         "bookshelf_id=" + QByteArray::number(number) + "&dont_remove=true", [this](QNetworkReply *reply) {
             if (!QJsonDocument::fromJson(reply->readAll()).object().contains("bookshelves_affected"))
                 m_errors << "the shelf was not taken";
             sendFinished();
         });
}

void OpenLibraryLog::sendFinished() {
    const ReadingSites::Outgoing change = m_toWrite.first();
    const QStringList date = change.finished.split('-', Qt::SkipEmptyParts);
    if (date.isEmpty() || date.first().toInt() == 0) {
        written({});
        return;
    }
    // A date written without knowing of the one already there would be a second one.
    if (!m_statesKnown) {
        written("the finished date was not sent: the dates already there couldn't be read");
        return;
    }
    QJsonObject event{{"event_type", 3}, {"year", date.at(0).toInt()}};
    if (date.size() > 1) event.insert("month", date.at(1).toInt());
    if (date.size() > 2) event.insert("day", date.at(2).toInt());
    const qint64 existing = m_states.value(m_work).event;
    event.insert("event_id", existing > 0 ? QJsonValue(existing) : QJsonValue());
    post(QUrl(m_base + "/works/" + m_work + "/check-ins.json"), "application/json", QJsonDocument(event).toJson(QJsonDocument::Compact),
         [this](QNetworkReply *reply) {
             const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
             if (answer.value("status").toString() != "ok") {
                 written("the finished date was not taken");
                 return;
             }
             m_states[m_work].event = answer.value("id").toInteger();
             written({});
         });
}
