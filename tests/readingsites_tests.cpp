#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkCookieJar>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>

#include "curation.h"
#include "fixtures.h"
#include "goodreads.h"
#include "hardcover.h"
#include "library.h"
#include "openlibrarylog.h"
#include "plugins.h"
#include "readingsite.h"
#include "secretstore.h"
#include "settings.h"
#include "sitedesk.h"

namespace {

// A web site for the tests: every request is answered by `answer`, which is
// handed the path with its query, or by `respond`, which is handed the whole
// request, and says what comes back. What was asked is kept, so a test can
// see that a site was asked once and politely, and what it was sent.
class FakeSite : public QTcpServer {
public:
    struct Reply {
        Reply(int status = 200, const QByteArray &body = {}, const QByteArray &headers = {})
            : status(status), body(body), headers(headers) {}
        int status;
        QByteArray body;
        QByteArray headers; // further lines of the answer's head, each ending in \r\n
    };
    struct Request {
        QByteArray method;
        QString path;
        QByteArray head;
        QByteArray body;
        // A header's value, whatever case it was sent in.
        QByteArray header(const QByteArray &name) const {
            for (const QByteArray &line : head.split('\n'))
                if (line.toLower().startsWith(name.toLower() + ":"))
                    return line.mid(name.size() + 1).trimmed();
            return {};
        }
    };
    std::function<Reply(const QString &path)> answer;
    std::function<Reply(const Request &)> respond;
    QStringList asked;
    QList<QByteArray> headers;
    QList<Request> requests;

    FakeSite() {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            QTcpSocket *socket = nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                const QByteArray request = socket->property("request").toByteArray() + socket->readAll();
                socket->setProperty("request", request);
                const qsizetype headEnd = request.indexOf("\r\n\r\n");
                if (headEnd < 0)
                    return;
                Request asking;
                asking.head = request.left(headEnd);
                asking.method = asking.head.left(asking.head.indexOf(' '));
                asking.path = QString::fromLatin1(asking.head.left(asking.head.indexOf("\r\n")).split(' ').value(1));
                const qsizetype length = asking.header("content-length").toLongLong();
                if (request.size() < headEnd + 4 + length)
                    return;
                asking.body = request.mid(headEnd + 4, length);
                asked << asking.path;
                headers << request;
                requests << asking;
                const Reply reply = respond ? respond(asking) : answer ? answer(asking.path) : Reply{404, "nothing here"};
                socket->write("HTTP/1.1 " + QByteArray::number(reply.status) + (reply.status == 200 ? " OK" : " Other") + "\r\n" + reply.headers
                              + "Content-Length: " + QByteArray::number(reply.body.size()) + "\r\nConnection: close\r\n\r\n" + reply.body);
                socket->disconnectFromHost();
            });
        });
    }

    QString url() const { return QString("http://127.0.0.1:%1").arg(serverPort()); }
    // Everything every request carried, to look for what must not be there.
    QByteArray everything() const { return headers.join('\n'); }
};

// A keyring for the tests: it forgets with the test, but says it lasts.
class TestSecrets : public SessionSecrets {
public:
    bool lasting(QString * = nullptr) const override { return true; }
};

// What a stand-in site does when asked, set by the test, and what it was
// asked to do, for the test to read.
struct Script {
    QList<SiteEntry> entries;
    SiteProfile profile;
    SiteProblem problem = SiteProblem::None;
    QString message;
    bool writes = false;
    QString refuses; // the title of a book whose change the site won't take
    QList<ReadingSites::Outgoing> written;
    QStringList secrets; // every secret a site was made with
    int fetches = 0;
};

// A reading site that needs no network and does as its script says.
class ScriptedSite : public ReadingSite {
public:
    ScriptedSite(const QString &scheme, const QString &user, Script *script, QObject *parent)
        : ReadingSite(parent), m_scheme(scheme), m_user(user), m_script(script) {}
    QString name() const override { return m_scheme == "hardcover" ? "Hardcover" : m_scheme == "openlibrary" ? "Open Library" : "Goodreads"; }
    QString scheme() const override { return m_scheme; }
    QString account() const override { return m_user; }
    SecretUse secretUse() const override { return m_scheme == "hardcover" ? NeedsSecret : OptionalSecret; }
    void setSecret(const QString &secret) override {
        m_secret = secret;
        m_script->secrets << secret;
    }
    QUrl findMePage() const override { return QUrl("https://example.org/" + m_scheme + "/me"); }
    QUrl secretPage() const override { return QUrl("https://example.org/" + m_scheme + "/token"); }
    void check() override {
        QMetaObject::invokeMethod(this, [this] {
            if (m_script->problem != SiteProblem::None)
                fail(m_script->problem, m_script->message);
            else
                emit checked(m_script->profile);
        }, Qt::QueuedConnection);
    }
    void fetchShelves() override {
        ++m_script->fetches;
        QMetaObject::invokeMethod(this, [this] {
            if (m_script->problem != SiteProblem::None)
                fail(m_script->problem, m_script->message);
            else
                emit fetched(m_script->entries);
        }, Qt::QueuedConnection);
    }
    void find(const QString &, const QString &) override { emit found({}); }
    bool writes() const override { return m_script->writes && !m_secret.isEmpty(); }
    void write(const QList<ReadingSites::Outgoing> &changes) override {
        QList<SiteWritten> results;
        for (const ReadingSites::Outgoing &change : changes) {
            const bool refused = change.title == m_script->refuses;
            if (!refused)
                m_script->written << change;
            results.append({change.bookId, change.siteId.isEmpty() ? "new-" + QString::number(change.bookId) : change.siteId,
                            refused ? QString("the site said no") : QString()});
        }
        QMetaObject::invokeMethod(this, [this, results] { emit wrote(results); }, Qt::QueuedConnection);
    }

private:
    QString m_scheme, m_user, m_secret;
    Script *m_script;
};

// A Goodreads export as it is downloaded: quoted titles with commas, a
// review over two lines with doubled quotes, ISBNs as ="...", several
// authors, the three shelves and one of the user's own.
const QByteArray exportFile =
    "\xEF\xBB\xBF"
    "Book Id,Title,Author,Author l-f,Additional Authors,ISBN,ISBN13,My Rating,Average Rating,Publisher,Binding,"
    "Number of Pages,Year Published,Original Publication Year,Date Read,Date Added,Bookshelves,Bookshelves with positions,"
    "Exclusive Shelf,My Review,Spoiler,Private Notes,Read Count,Owned Copies\r\n"
    "101,\"The Long Way, Home (The Road, #2)\",Ann Author,\"Author, Ann\",\"Bo Writer, Cy Scribe\",=\"0306406152\","
    "=\"9780306406157\",5,4.21,Test House,Paperback,320,2001,1999,2023/05/14,2023/04/01,\"favorites, sea-stories\","
    "\"favorites (#1), sea-stories (#2)\",read,\"A \"\"classic\"\",\nread on two lines.\",,,1,1\r\n"
    "102,Quiet Rivers,Dee Poet,\"Poet, Dee\",,=\"\",=\"\",0,3.90,Small Press,Kindle Edition,120,2019,2019,,2024/01/02,to-read,"
    "to-read (#1),to-read,,,,0,1\r\n"
    "103,Half Read,Eve Teller,\"Teller, Eve\",,=\"\",=\"9781594480003\",0,4.00,Big House,Hardcover,400,2004,2003,,2024/02/03,"
    "currently-reading,currently-reading (#1),currently-reading,,,,0,0\r\n"
    "104,Left Unfinished,Flo Quit,\"Quit, Flo\",,=\"\",=\"\",2,3.10,,Paperback,,2010,2010,,2022/07/08,did-not-finish,"
    "did-not-finish (#1),did-not-finish,,,,0,0\r\n";

// One page of a shelf's feed, as Goodreads writes it.
QByteArray feed(const QList<QPair<QString, QString>> &books) {
    QByteArray xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<rss version=\"2.0\"><channel><title>Ann's bookshelf: read</title>\n";
    for (const auto &[id, title] : books)
        xml += "<item><guid><![CDATA[https://example.org/review/" + id.toUtf8() + "]]></guid>"
               "<pubDate><![CDATA[Sun, 14 May 2023 09:00:00 -0700]]></pubDate>"
               "<title><![CDATA[" + title.toUtf8() + "]]></title>"
               "<book_id>" + id.toUtf8() + "</book_id>"
               "<book_image_url><![CDATA[https://images.example.org/books/1i/" + id.toUtf8() + "._SY75_.jpg]]></book_image_url>"
               "<book_large_image_url><![CDATA[https://images.example.org/books/1i/" + id.toUtf8() + "._SY475_.jpg]]></book_large_image_url>"
               "<book_description><![CDATA[About <b>it</b>.]]></book_description>"
               "<book id=\"" + id.toUtf8() + "\"><num_pages>320</num_pages></book>"
               "<author_name>Ann Author</author_name><isbn>0306406152</isbn><user_name>Ann</user_name>"
               "<user_rating>4</user_rating><user_read_at><![CDATA[Sat, 13 May 2023 00:00:00 +0000]]></user_read_at>"
               "<user_date_added><![CDATA[Sat, 01 Apr 2023 10:11:12 -0700]]></user_date_added>"
               "<user_shelves>favorites, sea-stories</user_shelves><user_review></user_review>"
               "<average_rating>4.21</average_rating><book_published>1999</book_published></item>\n";
    return xml + "</channel></rss>\n";
}

// What the search box answers.
const QByteArray searchAnswer = R"json([
 {"imageUrl":"https://images.example.org/books/9i/900._SY75_.jpg","bookId":"900","workId":"9","bookUrl":"/book/show/900",
  "title":"The Long Way Home: A Study Guide","bookTitleBare":"The Long Way Home: A Study Guide","numPages":40,"avgRating":"5.00",
  "ratingsCount":3,"author":{"id":7,"name":"Quick Notes"},"description":{"html":"A guide to…","truncated":true}},
 {"imageUrl":"https://images.example.org/books/1i/101._SY75_.jpg","bookId":"101","workId":"1","bookUrl":"/book/show/101",
  "title":"The Long Way Home (The Road, #2)","bookTitleBare":"The Long Way Home","numPages":320,"avgRating":"4.21",
  "ratingsCount":123456,"author":{"id":1,"name":"Ann Author"},"description":{"html":"Short and <i>whole</i>.","truncated":false}}
])json";

// A book's page: the data it is built from, and what it tells search engines.
QByteArray bookPage(bool withData = true, bool withLinkedData = true) {
    const QByteArray data = R"json({"props":{"pageProps":{"apolloState":{
      "ROOT_QUERY":{"__typename":"Query","getBookByLegacyId({\"legacyId\":\"101\"})":{"__ref":"Book:kca://book/a"}},
      "Book:kca://book/other":{"__typename":"Book","title":"Another Edition","details":{"publisher":"Wrong"}},
      "Contributor:kca://author/1":{"__typename":"Contributor","name":"Ann Author"},
      "Contributor:kca://author/2":{"__typename":"Contributor","name":"Ivy Drawer"},
      "Contributor:kca://author/3":{"__typename":"Contributor","name":"Bo Writer"},
      "Series:kca://series/1":{"__typename":"Series","title":"The Road"},
      "Book:kca://book/a":{"__typename":"Book","legacyId":101,"title":"The Long Way Home","titleComplete":"The Long Way Home",
        "description":"Two travellers.<br />One road.",
        "description({\"stripped\":true})":"Two travellers.\r\nOne road.",
        "primaryContributorEdge":{"node":{"__ref":"Contributor:kca://author/1"},"role":"Author"},
        "secondaryContributorEdges":[{"node":{"__ref":"Contributor:kca://author/2"},"role":"Illustrator"},
                                     {"node":{"__ref":"Contributor:kca://author/3"},"role":"Author"}],
        "imageUrl":"https://images.example.org/books/1i/101.jpg",
        "bookSeries":[{"userPosition":"2","series":{"__ref":"Series:kca://series/1"}}],
        "bookGenres":[{"genre":{"name":"Fiction"}},{"genre":{"name":"Audiobook"}},{"genre":{"name":"Travel"}}],
        "details":{"format":"Paperback","numPages":320,"publicationTime":988675200000,"publisher":"Test House",
                   "isbn":"0306406152","isbn13":"9780306406157","language":{"name":"English"}},
        "work":{"__ref":"Work:kca://work/1"}},
      "Work:kca://work/1":{"__typename":"Work","details":{"publicationTime":-1018627200000},
        "stats":{"averageRating":4.21,"ratingsCount":123456}}
    }}}})json";
    const QByteArray linked = R"json({"@context":"https://schema.org","@type":"Book","name":"The Long Way Home",
      "image":"https://images.example.org/books/1i/101.jpg","bookFormat":"Paperback","inLanguage":"English","isbn":"9780306406157",
      "author":[{"@type":"Person","name":"Ann Author"},{"@type":"Person","name":"Ivy Drawer"}],
      "aggregateRating":{"@type":"AggregateRating","ratingValue":4.21,"ratingCount":123456}})json";
    QByteArray page = "<!DOCTYPE html><html><head><title>The Long Way Home</title>";
    if (withLinkedData)
        page += "<script type=\"application/ld+json\">" + linked + "</script>";
    page += "</head><body><h1>The Long Way Home</h1>";
    if (withData)
        page += "<script id=\"__NEXT_DATA__\" type=\"application/json\">" + data + "</script>";
    return page + "</body></html>";
}

SiteEntry entry(const QString &id, const QString &title, const QString &author, const QString &shelf, int rating = 0,
                const QString &finished = {}) {
    SiteEntry entry;
    entry.siteId = id;
    entry.title = title;
    entry.authors = QStringList{author};
    entry.shelf = shelf;
    entry.rating = rating;
    entry.finished = finished;
    return entry;
}

BookMeta meta(const QString &title, const QString &author, const QString &isbn = {}) {
    BookMeta meta;
    meta.title = title;
    meta.authors = QStringList{author};
    meta.isbn = isbn;
    return meta;
}

// A reading site that knows the books it was told of, and can be made to fail.
class StubSite : public ReadingSite {
public:
    using ReadingSite::ReadingSite;
    QList<SiteBook> known;
    bool down = false;
    QStringList askedFor;

    QString name() const override { return "Stub"; }
    QString scheme() const override { return "stub"; }
    QString account() const override { return "ann"; }
    void fetchShelves() override {}
    void find(const QString &title, const QString &) override {
        askedFor << title;
        if (down)
            emit findFailed("The stub is down.");
        else
            emit found(known);
    }
};

} // namespace

// The sites people keep their reading on: Goodreads' export file, feed, search
// box and book pages, Open Library's reading log, how their entries become
// books here, and the suggestions that use them. Nothing here touches the
// network: every answer is canned or comes from a server on this computer.
class ReadingSitesTests : public QObject {
    Q_OBJECT

private slots:
    void aGoodreadsExportIsRead() {
        const ReadingSite::Export read = Goodreads::parseCsv(exportFile);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QCOMPARE(read.entries.size(), 4);

        const SiteEntry &first = read.entries.at(0);
        QCOMPARE(first.siteId, QString("101"));
        // A comma inside quotes is part of the title.
        QCOMPARE(first.title, QString("The Long Way, Home (The Road, #2)"));
        QCOMPARE(first.authors, (QStringList{"Ann Author", "Bo Writer", "Cy Scribe"}));
        // ="..." is how the file keeps a spreadsheet from mangling an ISBN.
        QCOMPARE(first.isbn, QString("9780306406157"));
        QCOMPARE(first.shelf, QString("read"));
        QCOMPARE(first.rating, 5);
        QCOMPARE(first.finished, QString("2023-05-14"));
        QCOMPARE(first.added, QString("2023-04-01"));
        QCOMPARE(first.shelves, (QStringList{"favorites", "sea-stories"}));
        QCOMPARE(first.averageRating, 4.21);
        QCOMPARE(first.published, QString("1999"));
        QVERIFY(first.paper());
        // The series comes out of the title.
        QCOMPARE(first.meta().title, QString("The Long Way, Home"));
        QCOMPARE(first.meta().series, QString("The Road"));
        QCOMPARE(first.meta().seriesIndex, 2.0);

        // An owned ebook is not a paper book; a hardcover nobody owns isn't either.
        QCOMPARE(read.entries.at(1).shelf, QString("to-read"));
        QCOMPARE(read.entries.at(1).isbn, QString());
        QVERIFY(!read.entries.at(1).paper());
        QCOMPARE(read.entries.at(2).shelf, QString("reading"));
        QCOMPARE(read.entries.at(2).isbn, QString("9781594480003"));
        QVERIFY(!read.entries.at(2).paper());
        // A shelf of the user's own that replaces the three is kept as a tag.
        QCOMPARE(read.entries.at(3).shelf, QString());
        QCOMPARE(read.entries.at(3).shelves, QStringList{"did-not-finish"});

        // Any other file is refused, with what to do instead.
        const ReadingSite::Export other = Goodreads::parseCsv("name,price\nbook,3\n");
        QVERIFY(other.entries.isEmpty());
        QVERIFY(other.error.contains("not a Goodreads export"));
    }

    void csvFieldsSurviveQuotesAndLineBreaks() {
        const QList<QStringList> rows = ReadingSites::parseCsv("a,\"b, c\",\"d \"\"e\"\"\nf\",=\"0123\"\r\n\r\n1,,3,\n");
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0), (QStringList{"a", "b, c", "d \"e\"\nf", "=\"0123\""}));
        QCOMPARE(rows.at(1), (QStringList{"1", "", "3", ""}));
        QCOMPARE(ReadingSites::csvField("plain"), QString("plain"));
        QCOMPARE(ReadingSites::csvField("a, \"b\""), QString("\"a, \"\"b\"\"\""));
    }

    void aShelfFeedIsRead() {
        bool understood = false;
        const QList<SiteEntry> entries = Goodreads::parseFeed(feed({{"101", "The Long Way Home (The Road, #2)"}, {"102", "Quiet Rivers"}}),
                                                              "read", &understood);
        QVERIFY(understood);
        QCOMPARE(entries.size(), 2);
        const SiteEntry &first = entries.first();
        QCOMPARE(first.siteId, QString("101"));
        QCOMPARE(first.title, QString("The Long Way Home (The Road, #2)"));
        QCOMPARE(first.authors, QStringList{"Ann Author"});
        QCOMPARE(first.isbn, QString("0306406152"));
        QCOMPARE(first.shelf, QString("read"));
        QCOMPARE(first.rating, 4);
        QCOMPARE(first.finished, QString("2023-05-13"));
        QCOMPARE(first.added, QString("2023-04-01"));
        QCOMPARE(first.shelves, (QStringList{"favorites", "sea-stories"}));
        // The picture's address names a thumbnail; the cover is the whole one.
        QCOMPARE(first.coverUrl, QUrl("https://images.example.org/books/1i/101.jpg"));
        // The feed says nothing of copies owned, so nothing is taken for paper.
        QVERIFY(!first.paper());

        // A page to sign in on is not a feed.
        QVERIFY(Goodreads::parseFeed("<!DOCTYPE html><html><body>Sign in</body></html>", "read", &understood).isEmpty());
        QVERIFY(!understood);
    }

    void theUserIsReadFromWhateverWasPasted() {
        QCOMPARE(Goodreads::userId("12345"), QString("12345"));
        QCOMPARE(Goodreads::userId(" 12345-ann-reader "), QString("12345"));
        QCOMPARE(Goodreads::userId("https://www.goodreads.com/user/show/12345-ann-reader"), QString("12345"));
        QCOMPARE(Goodreads::userId("https://www.goodreads.com/review/list/12345?shelf=read"), QString("12345"));
        QCOMPARE(Goodreads::userId("https://www.goodreads.com/review/list_rss/12345?key=AbC-1_2&shelf=read"), QString("12345"));
        QCOMPARE(Goodreads::feedKey("https://www.goodreads.com/review/list_rss/12345?key=AbC-1_2&shelf=read"), QString("AbC-1_2"));
        QCOMPARE(Goodreads::userId("ann"), QString());
        QCOMPARE(Goodreads::feedKey("12345"), QString());

        QNetworkAccessManager network;
        const GoodreadsSite site("https://www.goodreads.com/review/list_rss/12345?key=AbC-1_2&shelf=read", &network);
        QCOMPARE(site.shelfUrl("to-read", 2).toString(), QString("https://www.goodreads.com/review/list_rss/12345?shelf=to-read&page=2&key=AbC-1_2"));
        // What is searched for is encoded once, quotes and ampersands and all.
        QCOMPARE(site.findUrl("Rock & \"Roll\"", "Ann Author").toString(QUrl::FullyEncoded),
                 QString("https://www.goodreads.com/book/auto_complete?format=json&q=Rock%20%26%20%22Roll%22%20Ann%20Author"));
    }

    void entriesAreMatchedByNumberThenIsbnThenTitle() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        const qint64 byNumber = library.addEntry(meta("A Title Goodreads Spells Otherwise", "Ann Author"), false).id;
        QVERIFY(library.setIdentifier(byNumber, "goodreads", "101"));
        const qint64 byIsbn = library.addEntry(meta("Quiet Rivers: Poems", "D. Poet", "0-306-40615-2"), false).id;
        const qint64 byTitle = library.addEntry(meta("Half Read", "J. R. R. Teller"), false).id;
        const qint64 sameTitle = library.addEntry(meta("Common Title", "First Writer"), false).id;
        const qint64 authorless = library.addEntry(meta("Anonymous Sagas", {}), false).id;
        const QList<Book> books = library.books();

        // The site's own number wins over everything else it says.
        SiteEntry numbered = entry("101", "The Long Way Home", "Somebody Else", "read");
        numbered.isbn = "9780306406157";
        QCOMPARE(ReadingSites::match(books, numbered, "goodreads"), byNumber);
        // Then the ISBN, in either form and however it is punctuated.
        SiteEntry isbn = entry("555", "Quiet Rivers", "Dee Poet", "read");
        isbn.isbn = "9780306406157";
        QCOMPARE(ReadingSites::match(books, isbn, "goodreads"), byIsbn);
        QCOMPARE(ReadingSites::isbn13("0-306-40615-2"), QString("9780306406157"));
        QCOMPARE(ReadingSites::isbn13("not an isbn"), QString());
        // Then the title without its series, and the first author however spelled.
        QCOMPARE(ReadingSites::match(books, entry("556", "Half Read (The Halves, #1)", "J.R.R. Teller", "read"), "goodreads"), byTitle);
        QCOMPARE(ReadingSites::match(books, entry("557", "HALF READ", "Teller, J. R. R.", "read"), "goodreads"), byTitle);
        // The same title by someone else is another book.
        QCOMPARE(ReadingSites::match(books, entry("558", "Common Title", "Second Writer", "read"), "goodreads"), 0);
        QCOMPARE(ReadingSites::match(books, entry("559", "Common Title", "First Writer", "read"), "goodreads"), sameTitle);
        // A book with no author here is found by its title alone.
        QCOMPARE(ReadingSites::match(books, entry("560", "Anonymous Sagas", "Unknown", "read"), "goodreads"), authorless);
        // Another site's numbers are not this site's.
        QCOMPARE(ReadingSites::match(books, entry("101", "Nothing Like It", "Nobody", "read"), "openlibrary"), 0);
    }

    void anImportAddsPaperBooksAndBooksOnlyKnownOf() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        const ReadingSites::Summary summary = ReadingSites::merge(library, Goodreads::parseCsv(exportFile).entries, "goodreads");
        QCOMPARE(summary.added, 4);
        QCOMPARE(summary.updated + summary.unchanged + summary.unmatched + summary.kept, 0);
        QVERIFY(summary.problems.isEmpty());

        const QList<Book> paper = library.books("is:paper");
        QCOMPARE(paper.size(), 1);
        const Book &owned = paper.first();
        QCOMPARE(owned.meta.title, QString("The Long Way, Home"));
        QCOMPARE(owned.meta.series, QString("The Road"));
        QCOMPARE(owned.meta.authors, (QStringList{"Ann Author", "Bo Writer", "Cy Scribe"}));
        QCOMPARE(owned.meta.isbn, QString("9780306406157"));
        QCOMPARE(owned.held(), QString("paper"));
        QCOMPARE(owned.shelf, QString("read"));
        QCOMPARE(owned.rating, 5);
        QCOMPARE(owned.finished, QString("2023-05-14"));
        QCOMPARE(owned.identifiers.value("goodreads").toString(), QString("101"));
        QVERIFY(owned.meta.tags.contains("sea-stories"));

        // The rest are known of, not held: a wishlist and a record of reading.
        const QList<Book> wished = library.books("is:wishlist");
        QCOMPARE(wished.size(), 3);
        QCOMPARE(library.books("status:to-read").size(), 1);
        QCOMPARE(library.books("status:reading").size(), 1);
        for (const Book &book : wished)
            QCOMPARE(book.held(), QString("none"));

        // A cover is asked for where one can be had: by ISBN, from Open Library.
        QCOMPARE(summary.covers.size(), 2);
        QCOMPARE(summary.covers.first().first, owned.id);
        QCOMPARE(summary.covers.first().second, QUrl("https://covers.openlibrary.org/b/isbn/9780306406157-L.jpg?default=false"));

        // The same file again changes nothing and adds nothing.
        const ReadingSites::Summary again = ReadingSites::merge(library, Goodreads::parseCsv(exportFile).entries, "goodreads");
        QCOMPARE(again.added, 0);
        QCOMPARE(again.updated, 0);
        QCOMPARE(again.unchanged, 4);
        QCOMPARE(library.books().size(), 4);
        QCOMPARE(again.toJson().value("changes").toArray().size(), 0);
    }

    void aSyncUpdatesWhatItMatchesAndKeepsWhatWasChangedHere() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("one.epub"), "The Test Book"));
        const qint64 digital = library.add(dir.filePath("one.epub")).id;
        const qint64 plain = library.addEntry(meta("Quiet Rivers", "Dee Poet"), false).id;
        const qint64 mine = library.addEntry(meta("Half Read", "Eve Teller"), true).id;
        QVERIFY(library.setReading(mine, "read", 3, "2024-06-01"));

        // First sync: a book with nothing said of it takes what the site
        // says; one that already says something keeps it.
        SiteEntry file = entry("201", "The Test Book", "Ann Author", "read", 4, "2023-01-02");
        file.owned = 1;
        file.binding = "Paperback";
        ReadingSites::Summary summary = ReadingSites::merge(
            library, {file, entry("202", "Quiet Rivers", "Dee Poet", "to-read"), entry("203", "Half Read", "Eve Teller", "reading", 5)},
            "goodreads");
        QCOMPARE(summary.added, 0);
        QCOMPARE(summary.updated, 3);
        QCOMPARE(summary.kept, 1);
        Book book = *library.book(digital);
        QCOMPARE(book.shelf, QString("read"));
        QCOMPARE(book.rating, 4);
        QCOMPARE(book.finished, QString("2023-01-02"));
        QCOMPARE(book.identifiers.value("goodreads").toString(), QString("201"));
        // A paper copy beside the file makes it both.
        QCOMPARE(book.held(), QString("both"));
        QCOMPARE(library.book(plain)->shelf, QString("to-read"));
        book = *library.book(mine);
        QCOMPARE(book.shelf, QString("read"));
        QCOMPARE(book.rating, 3);
        QCOMPARE(book.finished, QString("2024-06-01"));
        QCOMPARE(book.identifiers.value("goodreads").toString(), QString("203"));

        // The site moves on and Omabook didn't: the site is followed.
        summary = ReadingSites::merge(library, {entry("202", "Quiet Rivers", "Dee Poet", "read", 5, "2024-09-09")}, "goodreads");
        QCOMPARE(summary.updated, 1);
        QCOMPARE(summary.kept, 0);
        book = *library.book(plain);
        QCOMPARE(book.shelf, QString("read"));
        QCOMPARE(book.rating, 5);
        QCOMPARE(book.finished, QString("2024-09-09"));

        // Omabook moves on and the site didn't: Omabook's stays, and is said to.
        QVERIFY(library.setReading(plain, "read", 2, "2024-09-09"));
        summary = ReadingSites::merge(library, {entry("202", "Quiet Rivers", "Dee Poet", "read", 5, "2024-09-09")}, "goodreads");
        QCOMPARE(summary.updated, 0);
        QCOMPARE(summary.unchanged, 1);
        QCOMPARE(summary.kept, 1);
        QCOMPARE(library.book(plain)->rating, 2);
        QVERIFY(summary.changes.first().note.contains("2 stars here, 5 on the site"));
        // And still when the site changes too: what was set here is newer to Omabook.
        summary = ReadingSites::merge(library, {entry("202", "Quiet Rivers", "Dee Poet", "read", 4, "2024-09-09")}, "goodreads");
        QCOMPARE(library.book(plain)->rating, 2);
        QCOMPARE(summary.kept, 1);

        // Nothing the site leaves empty empties a field here.
        summary = ReadingSites::merge(library, {entry("201", "The Test Book", "Ann Author", "read")}, "goodreads");
        book = *library.book(digital);
        QCOMPARE(book.rating, 4);
        QCOMPARE(book.finished, QString("2023-01-02"));
        QVERIFY(book.paper);
    }

    void readingDoneHereIsNotMovedBack() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("one.epub"), "The Test Book"));
        const qint64 id = library.add(dir.filePath("one.epub")).id;
        // Read to the end in Omabook, with no shelf set by anyone.
        library.setProgress(id, {1, 1.0, 100, 0});
        QCOMPARE(library.book(id)->status(), QString("read"));

        ReadingSites::Summary summary = ReadingSites::merge(library, {entry("201", "The Test Book", "Ann Author", "to-read")}, "goodreads");
        QCOMPARE(library.book(id)->status(), QString("read"));
        QCOMPARE(summary.kept, 1);
        // The same or further along is taken.
        summary = ReadingSites::merge(library, {entry("201", "The Test Book", "Ann Author", "read", 5)}, "goodreads");
        QCOMPARE(library.book(id)->shelf, QString("read"));
        QCOMPARE(library.book(id)->rating, 5);
    }

    void aSyncNeverRemovesAnything() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("one.epub"), "The Test Book"));
        const qint64 file = library.add(dir.filePath("one.epub")).id;
        ReadingSites::merge(library, Goodreads::parseCsv(exportFile).entries, "goodreads");
        QCOMPARE(library.books().size(), 5);

        // The shelves come back with one book only: the others were taken
        // off the site, or the feed was cut short. They all stay.
        const ReadingSites::Summary summary = ReadingSites::merge(library, {entry("102", "Quiet Rivers", "Dee Poet", "read", 4)}, "goodreads");
        QCOMPARE(summary.updated, 1);
        QCOMPARE(library.books().size(), 5);
        QVERIFY(library.book(file));
        QCOMPARE(library.books("status:reading").size(), 1);
        // And an empty answer changes nothing at all.
        QCOMPARE(ReadingSites::merge(library, {}, "goodreads").changes.size(), 0);
        QCOMPARE(library.books().size(), 5);

        // With no new books wanted, an entry that is no book here is only counted.
        ReadingSites::Options options;
        options.addNew = false;
        const ReadingSites::Summary strict = ReadingSites::merge(library, {entry("999", "Not Here", "No One", "read")}, "goodreads", options);
        QCOMPARE(strict.unmatched, 1);
        QCOMPARE(strict.added, 0);
        QCOMPARE(library.books().size(), 5);
    }

    void anExportReadsBackAsItWasWritten() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("one.epub"), "The Test Book"));
        const qint64 digital = library.add(dir.filePath("one.epub")).id;
        QVERIFY(library.setReading(digital, "read", 4, "2024-03-09"));
        QVERIFY(library.setIdentifier(digital, "goodreads", "301"));
        const qint64 paper = library.addEntry(meta("Rock & \"Roll\", a Life", "Ann Author", "0306406152"), true).id;
        QVERIFY(library.setReading(paper, "reading", 0, {}));
        const qint64 wished = library.addEntry(meta("Quiet Rivers", "Dee Poet"), false).id;
        QVERIFY(library.setReading(wished, "to-read", 0, {}));
        // A book nothing is said of has nothing to tell Goodreads.
        library.addEntry(meta("Never Opened", "No One"), true);

        const QByteArray file = Goodreads::writeCsv(library.books());
        // The header is the one Goodreads' own export has, which its import takes.
        QVERIFY(file.startsWith("Book Id,Title,Author,Author l-f,Additional Authors,ISBN,ISBN13,My Rating,"));
        QVERIFY(file.contains("=\"0306406152\",=\"9780306406157\""));
        QVERIFY(file.contains("2024/03/09"));
        QVERIFY(!file.contains("Never Opened"));

        const ReadingSite::Export read = Goodreads::parseCsv(file);
        QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
        QCOMPARE(read.entries.size(), 3);
        QHash<QString, SiteEntry> byTitle;
        for (const SiteEntry &entry : read.entries)
            byTitle.insert(entry.title, entry);
        const SiteEntry first = byTitle.value("The Test Book");
        QCOMPARE(first.siteId, QString("301"));
        QCOMPARE(first.authors, (QStringList{"Ann Author", "Bo Writer"}));
        QCOMPARE(first.isbn, QString("9780000000002"));
        QCOMPARE(first.shelf, QString("read"));
        QCOMPARE(first.rating, 4);
        QCOMPARE(first.finished, QString("2024-03-09"));
        QVERIFY(!first.paper());
        const SiteEntry second = byTitle.value("Rock & \"Roll\", a Life");
        QCOMPARE(second.shelf, QString("reading"));
        QCOMPARE(second.isbn, QString("9780306406157"));
        QVERIFY(second.paper());
        QCOMPARE(byTitle.value("Quiet Rivers").shelf, QString("to-read"));
        QCOMPARE(byTitle.value("Quiet Rivers").owned, 0);

        // Brought into a library that has none of it, it is the same reading.
        Library other(dir.filePath("other"));
        QVERIFY(other.open());
        const ReadingSites::Summary summary = ReadingSites::merge(other, read.entries, "goodreads");
        QCOMPARE(summary.added, 3);
        QCOMPARE(other.books("status:read rating:4").size(), 1);
        QCOMPARE(other.books("is:paper status:reading").size(), 1);
        QCOMPARE(other.books("is:wishlist status:to-read").size(), 1);
        // And back here, every book is found again and nothing changes.
        const ReadingSites::Summary home = ReadingSites::merge(library, read.entries, "goodreads");
        QCOMPARE(home.added, 0);
        QCOMPARE(home.unchanged + home.updated, 3);
        QCOMPARE(library.books().size(), 4);
    }

    void aBookPageIsReadForItsDetails() {
        QString problem;
        const auto found = Goodreads::parseBookPage(bookPage(), &problem);
        QVERIFY2(found, qPrintable(problem));
        const BookMeta &meta = found->candidate.meta;
        QCOMPARE(found->id, QString("101"));
        QCOMPARE(found->candidate.source, QString("Goodreads"));
        // The book the page is about, not another edition it mentions.
        QCOMPARE(meta.title, QString("The Long Way Home"));
        // Authors, without the illustrator.
        QCOMPARE(meta.authors, (QStringList{"Ann Author", "Bo Writer"}));
        QCOMPARE(meta.series, QString("The Road"));
        QCOMPARE(meta.seriesIndex, 2.0);
        QCOMPARE(meta.description, QString("Two travellers.\r\nOne road."));
        QCOMPARE(meta.publisher, QString("Test House"));
        // When the book first came out, not when this edition did.
        QCOMPARE(meta.published, QString("1937-09-21"));
        QCOMPARE(meta.isbn, QString("9780306406157"));
        QCOMPARE(meta.language, QString("en"));
        QCOMPARE(meta.tags, (QStringList{"Fiction", "Travel"}));
        QCOMPARE(found->candidate.coverUrl, QUrl("https://images.example.org/books/1i/101.jpg"));
        QCOMPARE(found->averageRating, 4.21);
        QCOMPARE(found->ratings, 123456);
        QCOMPARE(found->candidate.why, QString("Goodreads readers rate it 4.21 out of 5 (123,456 ratings)."));

        // With the page's data gone, what it tells search engines still serves.
        const auto less = Goodreads::parseBookPage(bookPage(false, true), &problem);
        QVERIFY(less);
        QCOMPARE(less->candidate.meta.title, QString("The Long Way Home"));
        QCOMPARE(less->candidate.meta.authors, QStringList{"Ann Author"});
        QCOMPARE(less->candidate.meta.isbn, QString("9780306406157"));
        QCOMPARE(less->averageRating, 4.21);
        QVERIFY(less->candidate.meta.series.isEmpty());

        // A page that has neither, or isn't a book's page, says so rather than guessing.
        QVERIFY(!Goodreads::parseBookPage(bookPage(false, false), &problem));
        QVERIFY(problem.contains("doesn't understand"));
        QVERIFY(!Goodreads::parseBookPage("<html><script id=\"__NEXT_DATA__\">{\"props\": 3}</script></html>", &problem));
        QVERIFY(!Goodreads::parseBookPage("", &problem));
    }

    void theSearchBoxIsRead() {
        bool understood = false;
        const QList<Goodreads::Found> found = Goodreads::parseSuggestions(searchAnswer, &understood);
        QVERIFY(understood);
        QCOMPARE(found.size(), 2);
        QCOMPARE(found.at(1).id, QString("101"));
        QCOMPARE(found.at(1).candidate.meta.title, QString("The Long Way Home"));
        QCOMPARE(found.at(1).candidate.meta.authors, QStringList{"Ann Author"});
        QCOMPARE(found.at(1).averageRating, 4.21);
        QCOMPARE(found.at(1).ratings, 123456);
        QCOMPARE(found.at(1).candidate.coverUrl, QUrl("https://images.example.org/books/1i/101.jpg"));
        QCOMPARE(found.at(1).candidate.meta.description, QString("Short and whole."));
        // Half a description is left out.
        QVERIFY(found.at(0).candidate.meta.description.isEmpty());

        QVERIFY(Goodreads::parseSuggestions("<html>Sign in</html>", &understood).isEmpty());
        QVERIFY(!understood);
        QVERIFY(Goodreads::parseSuggestions("[]", &understood).isEmpty());
        QVERIFY(understood);
    }

    void aLookUpAsksTheSearchBoxAndThenTheBooksPage() {
        // The search box as it answers an ISBN: the one edition that has it,
        // and a mug that was given an ISBN too.
        const QByteArray byIsbn = QJsonDocument(QJsonArray{
            QJsonObject{{"bookId", "7"}, {"title", "A Mug"}, {"bookTitleBare", "A Mug"}, {"author", QJsonObject{{"name", "NOT A BOOK"}}}},
            QJsonObject{{"bookId", "101"}, {"title", "The Long Way Home (The Road, #2)"}, {"bookTitleBare", "The Long Way Home"},
                        {"avgRating", "4.21"}, {"ratingsCount", 123456}, {"author", QJsonObject{{"id", 1}, {"name", "Ann Author"}}},
                        {"description", QJsonObject{{"html", "x"}, {"truncated", true}}}}}).toJson();
        FakeSite goodreads;
        int shells = 0;
        goodreads.answer = [&](const QString &path) -> FakeSite::Reply {
            if (path == "/book/auto_complete?format=json&q=9780306406157")
                return {200, byIsbn};
            if (path == "/book/auto_complete?format=json&q=9781594480003" || path.contains("Somebody%20Else"))
                return {200, "[]"};
            if (path.startsWith("/book/auto_complete"))
                return {200, searchAnswer};
            // The page under its bare address is what the firewall answers
            // with nothing; with ".xml" on it, it is the page.
            if (path == "/book/show/101.xml")
                return shells-- > 0 ? FakeSite::Reply{202, ""} : FakeSite::Reply{200, bookPage()};
            return {404, "Page not found"};
        };
        QNetworkAccessManager network;
        GoodreadsSource source(&network, nullptr, goodreads.url());
        source.setRetryAfter(0);
        QCOMPARE(source.name(), QString("Goodreads"));
        QVERIFY(source.fields().contains("series"));
        QSignalSpy found(&source, &MetadataSource::found), failed(&source, &MetadataSource::failed);

        // By title: the search box, then the page of the book by this author,
        // though the search box put a study guide first. The series the
        // title carries and its subtitle stay out of the question.
        BookMeta book;
        book.title = "The Long Way Home: A Novel (The Road, #2)";
        book.authors = QStringList{"Ann Author"};
        source.search(book);
        QTRY_COMPARE(found.count(), 1);
        QCOMPARE(goodreads.asked, (QStringList{"/book/auto_complete?format=json&q=The%20Long%20Way%20Home%20Ann%20Author", "/book/show/101.xml"}));
        QVERIFY(goodreads.headers.first().contains("User-Agent: Omabook/"));
        QList<MetaCandidate> candidates = found.first().first().value<QList<MetaCandidate>>();
        // The study guide shares neither this title nor this author, and is no candidate.
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().meta.series, QString("The Road"));
        QCOMPARE(candidates.first().meta.publisher, QString("Test House"));
        // Under the bare title it could be an edition with a subtitle, and stays.
        book.title = "The Long Way Home";
        goodreads.asked.clear();
        source.search(book);
        QTRY_COMPARE(found.count(), 2);
        candidates = found.at(1).first().value<QList<MetaCandidate>>();
        QCOMPARE(candidates.size(), 2);
        QCOMPARE(candidates.at(1).meta.title, QString("The Long Way Home: A Study Guide"));

        // By ISBN: the search box says which book has it, and what is not a
        // book at all is passed over.
        goodreads.asked.clear();
        book.isbn = "0-306-40615-2";
        source.search(book);
        QTRY_COMPARE(found.count(), 3);
        QCOMPARE(goodreads.asked, (QStringList{"/book/auto_complete?format=json&q=9780306406157", "/book/show/101.xml"}));
        QCOMPARE(found.at(2).first().value<QList<MetaCandidate>>().size(), 1);
        QCOMPARE(found.at(2).first().value<QList<MetaCandidate>>().first().meta.publisher, QString("Test House"));

        // An ISBN Goodreads has no edition of falls back to the title.
        goodreads.asked.clear();
        book.isbn = "9781594480003";
        source.search(book);
        QTRY_COMPARE(found.count(), 4);
        QCOMPARE(goodreads.asked.size(), 3);
        QCOMPARE(found.at(3).first().value<QList<MetaCandidate>>().first().meta.series, QString("The Road"));

        // An author spelled so that nothing is found is left out of a second try.
        goodreads.asked.clear();
        book.isbn.clear();
        book.authors = QStringList{"Somebody Else"};
        source.search(book);
        QTRY_COMPARE(found.count(), 5);
        QCOMPARE(goodreads.asked.value(1), QString("/book/auto_complete?format=json&q=The%20Long%20Way%20Home"));
        QCOMPARE(found.at(4).first().value<QList<MetaCandidate>>().first().meta.series, QString("The Road"));
        book.authors = QStringList{"Ann Author"};

        // A page that comes back empty is asked for again, and is there.
        goodreads.asked.clear();
        shells = 2;
        source.search(book);
        QTRY_COMPARE(found.count(), 6);
        QCOMPARE(goodreads.asked.size(), 4);
        QCOMPARE(found.at(5).first().value<QList<MetaCandidate>>().first().meta.publisher, QString("Test House"));

        // A page that changed beyond reading still leaves what the search
        // box said, after no more than three tries.
        goodreads.asked.clear();
        goodreads.answer = [](const QString &path) -> FakeSite::Reply {
            return path.startsWith("/book/auto_complete") ? FakeSite::Reply{200, searchAnswer} : FakeSite::Reply{200, "<html>new design</html>"};
        };
        source.search(book);
        QTRY_COMPARE(found.count(), 7);
        QCOMPARE(goodreads.asked.size(), 4);
        candidates = found.at(6).first().value<QList<MetaCandidate>>();
        QCOMPARE(candidates.first().meta.title, QString("The Long Way Home"));
        QVERIFY(candidates.first().meta.publisher.isEmpty());
        QCOMPARE(failed.count(), 0);

        // And a search box that changed fails with a reason, not with rubbish.
        goodreads.answer = [](const QString &) -> FakeSite::Reply { return {200, "<html>Sign in</html>"}; };
        source.search(book);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains("doesn't understand"));
    }

    void shelvesAreFetchedPageByPage() {
        FakeSite goodreads;
        goodreads.answer = [](const QString &path) -> FakeSite::Reply {
            if (path == "/review/list_rss/12345?shelf=read&page=1")
                return {200, feed({{"101", "The Long Way Home (The Road, #2)"}, {"102", "Quiet Rivers"}})};
            if (path == "/review/list_rss/12345?shelf=read&page=2")
                return {200, feed({{"103", "Half Read"}})};
            if (path == "/review/list_rss/12345?shelf=to-read&page=1")
                return {200, feed({{"104", "One Day"}})};
            // Past the end the to-read shelf repeats itself; the others are empty.
            if (path.startsWith("/review/list_rss/12345?shelf=to-read"))
                return {200, feed({{"104", "One Day"}})};
            return {200, feed({})};
        };
        QNetworkAccessManager network;
        GoodreadsSite site("https://www.goodreads.com/user/show/12345-ann", &network, nullptr, goodreads.url());
        QCOMPARE(site.account(), QString("12345"));
        QSignalSpy fetched(&site, &ReadingSite::fetched), failed(&site, &ReadingSite::failed);
        site.fetchShelves();
        QTRY_COMPARE(fetched.count(), 1);
        QCOMPARE(failed.count(), 0);
        const QList<SiteEntry> entries = fetched.first().first().value<QList<SiteEntry>>();
        QCOMPARE(entries.size(), 4);
        QCOMPARE(entries.at(0).shelf, QString("read"));
        QCOMPARE(entries.at(2).siteId, QString("103"));
        QCOMPARE(entries.at(3).shelf, QString("to-read"));
        QCOMPARE(entries.at(3).title, QString("One Day"));
        // Three pages of read, one of reading, two of to-read: no more.
        QCOMPARE(goodreads.asked.size(), 6);

        // A profile that isn't public says what to do instead.
        goodreads.answer = [](const QString &) -> FakeSite::Reply { return {404, "<html>Page not found</html>"}; };
        site.fetchShelves();
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains("import that file instead"));
        QCOMPARE(site.problem(), SiteProblem::Private);
        // So does a page to sign in on.
        goodreads.answer = [](const QString &) -> FakeSite::Reply { return {200, "<html>Sign in</html>"}; };
        site.fetchShelves();
        QTRY_COMPARE(failed.count(), 2);

        // With nobody named, nothing is asked.
        goodreads.asked.clear();
        GoodreadsSite nobody("", &network, nullptr, goodreads.url());
        QSignalSpy unnamed(&nobody, &ReadingSite::failed);
        nobody.fetchShelves();
        QTRY_COMPARE(unnamed.count(), 1);
        QVERIFY(unnamed.first().first().toString().contains("goodreads_user"));
        QCOMPARE(nobody.problem(), SiteProblem::NoIdentity);
        QVERIFY(goodreads.asked.isEmpty());
    }

    void anOpenLibraryReadingLogIsFetched() {
        auto log = [](int total, const QList<QPair<QString, QString>> &works) {
            QJsonArray entries;
            for (const auto &[key, title] : works)
                entries << QJsonObject{{"work", QJsonObject{{"title", title}, {"key", "/works/" + key},
                                                           {"author_names", QJsonArray{"Ann Author"}}, {"first_publish_year", 1999},
                                                           {"cover_id", 42}}},
                                       {"logged_edition", "/books/OL1M"}, {"logged_date", "2024/10/22, 00:24:25"}};
            return QJsonDocument(QJsonObject{{"page", 1}, {"numFound", total}, {"reading_log_entries", entries}}).toJson();
        };
        int total = 0;
        bool understood = false;
        const QList<SiteEntry> parsed = OpenLibrary::parseLog(log(7, {{"OL1W", "The Long Way Home"}}), "to-read", &total, &understood);
        QVERIFY(understood);
        QCOMPARE(total, 7);
        QCOMPARE(parsed.size(), 1);
        QCOMPARE(parsed.first().siteId, QString("OL1W"));
        QCOMPARE(parsed.first().authors, QStringList{"Ann Author"});
        QCOMPARE(parsed.first().shelf, QString("to-read"));
        QCOMPARE(parsed.first().added, QString("2024-10-22"));
        QCOMPARE(parsed.first().published, QString("1999"));
        QCOMPARE(parsed.first().coverUrl, QUrl("https://covers.openlibrary.org/b/id/42-L.jpg"));
        QVERIFY(OpenLibrary::parseLog("{\"error\": \"Shelf is private\"}", "read", &total, &understood).isEmpty());
        QVERIFY(!understood);

        // A full page of a hundred, so there is a second one to ask for.
        QList<QPair<QString, QString>> full;
        for (int i = 0; i < 100; ++i)
            full.append({QString("OL9%1W").arg(i), QString("Book %1").arg(i)});
        FakeSite openLibrary;
        openLibrary.answer = [&](const QString &path) -> FakeSite::Reply {
            if (path == "/people/ann/books/already-read.json?page=1")
                return {200, log(101, full)};
            if (path == "/people/ann/books/already-read.json?page=2")
                return {200, log(1, {{"OL2W", "Quiet Rivers"}})};
            if (path == "/people/ann/books/want-to-read.json?page=1")
                return {200, log(1, {{"OL3W", "One Day"}})};
            if (path == "/people/ann/books/stopped-reading.json?page=1")
                return {200, log(1, {{"OL4W", "Left Off"}})};
            if (path.startsWith("/search.json"))
                return {200, R"json({"docs":[{"key":"/works/OL1W","title":"The Long Way Home","author_name":["Ann Author"],
                                 "ratings_average":3.9,"ratings_count":12,"cover_i":42}]})json"};
            return {200, log(0, {})};
        };
        QNetworkAccessManager network;
        OpenLibraryLog site("https://openlibrary.org/people/ann", &network, nullptr, openLibrary.url());
        site.setPace(0);
        QCOMPARE(site.account(), QString("ann"));
        QVERIFY(!site.writes());
        QSignalSpy fetched(&site, &ReadingSite::fetched), failed(&site, &ReadingSite::failed);
        site.fetchShelves();
        QTRY_COMPARE(fetched.count(), 1);
        QList<SiteEntry> entries = fetched.first().first().value<QList<SiteEntry>>();
        QCOMPARE(entries.size(), 103);
        QCOMPARE(entries.at(100).shelf, QString("read"));
        QCOMPARE(entries.at(101).shelf, QString("to-read"));
        // "Stopped reading" is no status here: it comes as a tag, and its
        // place there is left alone.
        QCOMPARE(entries.at(102).shelf, QString());
        QCOMPARE(entries.at(102).shelves, QStringList{"stopped-reading"});
        QVERIFY(entries.at(102).otherShelf);
        // Two pages of read, and one each of the other three shelves.
        QCOMPARE(openLibrary.asked.size(), 5);
        entries = entries.mid(100);

        // Its entries become books the same way Goodreads' do, under its own numbers.
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        const ReadingSites::Summary summary = ReadingSites::merge(library, entries, site.scheme());
        QCOMPARE(summary.added, 3);
        QCOMPARE(library.bookWithIdentifier("openlibrary", "OL3W"), library.books("status:to-read").first().id);
        QCOMPARE(summary.covers.size(), 3);
        QVERIFY(library.books("title:Left").first().meta.tags.contains("stopped-reading"));

        // It knows books by title too, with what its readers make of them.
        QSignalSpy found(&site, &ReadingSite::found);
        site.find("The Long Way Home", "Ann Author");
        QTRY_COMPARE(found.count(), 1);
        const QList<SiteBook> books = found.first().first().value<QList<SiteBook>>();
        QCOMPARE(books.size(), 1);
        QCOMPARE(books.first().siteId, QString("OL1W"));
        QCOMPARE(books.first().averageRating, 3.9);
        QCOMPARE(books.first().ratings, 12);

        // It has no file to read or write, and says so.
        QVERIFY(site.readExport("anything").error.contains("no export file"));
        QVERIFY(site.writeExport(library.books()).isNull());

        // Somebody it doesn't know, and a log that is private, are told apart.
        openLibrary.answer = [](const QString &) -> FakeSite::Reply { return {404, "{\"detail\": \"User not found\"}"}; };
        site.fetchShelves();
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(site.problem(), SiteProblem::NoUser);
        QVERIFY(failed.first().first().toString().contains("knows nobody called ann"));
        openLibrary.answer = [](const QString &) -> FakeSite::Reply { return {403, "{\"detail\": \"This reading log is private\"}"}; };
        site.fetchShelves();
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(site.problem(), SiteProblem::Private);
        QVERIFY(failed.last().first().toString().contains("private"));
    }

    void goodreadsIsOnePluginThatCanBeSwitchedOff() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        Plugins plugins(&settings, dir.filePath("plugins"), "-");
        auto goodreads = [&] {
            for (const PluginInfo &plugin : plugins.known())
                if (plugin.id == "builtin:Goodreads")
                    return plugin;
            return PluginInfo();
        };
        QCOMPARE(goodreads().kind, QString("metadata"));
        QVERIFY(goodreads().enabled && goodreads().switchable);
        QVERIFY(goodreads().fields.contains("series"));

        QNetworkAccessManager network;
        QObject owner;
        auto names = [&] {
            QStringList names;
            for (MetadataSource *source : plugins.sources(plugins.known(), &network, &owner))
                names << source->name();
            return names;
        };
        QVERIFY(names().contains("Goodreads"));
        settings.setGoodreadsUser("https://www.goodreads.com/user/show/12345-ann");
        QList<ReadingSite *> sites = ReadingSites::all(settings, &network, &owner);
        QCOMPARE(sites.size(), 2);
        QCOMPARE(sites.first()->name(), QString("Goodreads"));
        QCOMPARE(sites.first()->account(), QString("12345"));

        // Off, it is neither a look-up source nor a reading site: nothing
        // is asked of goodreads.com.
        QVERIFY(plugins.setEnabled("builtin:Goodreads", false));
        QVERIFY(!names().contains("Goodreads"));
        sites = ReadingSites::all(settings, &network, &owner);
        QCOMPARE(sites.size(), 1);
        QCOMPARE(sites.first()->scheme(), QString("openlibrary"));
        QVERIFY(!ReadingSites::one(settings, "goodreads", &network, &owner));

        // The user is kept in config.json, with the other settings.
        Settings reopened(dir.filePath("config.json"));
        QCOMPARE(reopened.goodreadsUser(), QString("https://www.goodreads.com/user/show/12345-ann"));
    }

    void suggestionsAreAskedForFromAllTheReading() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("one.epub"), "The Test Book"));
        const qint64 file = library.add(dir.filePath("one.epub")).id;
        library.setProgress(file, {1, 0.5, 40, 0});
        // Read on paper and rated, read elsewhere and disliked, wanted, and merely owned.
        const qint64 loved = library.addEntry(meta("A Wizard of the Sea", "Ula Guin"), true).id;
        QVERIFY(library.setReading(loved, "read", 5, "2024-03-09"));
        const qint64 disliked = library.addEntry(meta("Dull Days", "Flo Quit"), false).id;
        QVERIFY(library.setReading(disliked, "read", 1, "2022-01-05"));
        const qint64 wanted = library.addEntry(meta("One Day", "Dee Poet"), false).id;
        QVERIFY(library.setReading(wanted, "to-read", 0, {}));
        library.addEntry(meta("On the Shelf", "Cy Scribe"), true);

        QList<Curation::BookNotes> books;
        for (const Book &book : library.books())
            books.append({book, book.id == file ? QString("Two travellers set out.") : QString()});
        const AiRequest request = Curation::suggestionRequest(books);
        const QString asked = request.messages.first().text;
        const qsizetype start = asked.indexOf('{'), end = asked.lastIndexOf('}');
        const QJsonObject summary = QJsonDocument::fromJson(asked.mid(start, end - start + 1).toUtf8()).object();
        QVERIFY(!summary.isEmpty());

        // What was read, with the reader's own stars and when; paper books
        // and books only known of count as much as files.
        const QJsonArray read = summary.value("read").toArray();
        QCOMPARE(read.size(), 2);
        QCOMPARE(read.at(0).toObject().value("title").toString(), QString("A Wizard of the Sea"));
        QCOMPARE(read.at(0).toObject().value("stars").toInt(), 5);
        QCOMPARE(read.at(0).toObject().value("finished").toString(), QString("2024-03"));
        QCOMPARE(read.at(0).toObject().value("copy").toString(), QString("paper"));
        QCOMPARE(read.at(1).toObject().value("stars").toInt(), 1);
        QCOMPARE(read.at(1).toObject().value("copy").toString(), QString("none"));
        // What is being read, how far, with the notes on it.
        const QJsonObject reading = summary.value("reading").toArray().first().toObject();
        QCOMPARE(reading.value("title").toString(), QString("The Test Book"));
        QCOMPARE(reading.value("at").toString(), QString("40%"));
        QCOMPARE(reading.value("notes_so_far").toString(), QString("Two travellers set out."));
        // The to-read list, and what else is here unread.
        QCOMPARE(summary.value("to_read").toArray(), (QJsonArray{"One Day — Dee Poet"}));
        QCOMPARE(summary.value("also_has_unread").toArray(), (QJsonArray{"On the Shelf — Cy Scribe"}));
        const QJsonObject counts = summary.value("counts").toObject();
        QCOMPARE(counts.value("books").toInt(), 5);
        QCOMPARE(counts.value("read").toInt(), 2);
        QCOMPARE(counts.value("paper").toInt(), 2);
        QCOMPARE(counts.value("only_known_of").toInt(), 2);
        QVERIFY(summary.value("authors_most_read").toArray().contains("Ula Guin (1)"));

        QVERIFY(request.system.contains("Never suggest a book that is already in the list"));
        QVERIFY(request.system.contains("\"suggestions\""));
        QVERIFY(asked.contains("Suggest what I should read next"));
    }

    void aLargeLibraryIsSummedUpNotSentWhole() {
        QList<Curation::BookNotes> books;
        for (int i = 0; i < 1500; ++i) {
            Book book;
            book.id = i + 1;
            book.meta.title = QString("A Reasonably Long Title for Book Number %1").arg(i);
            book.meta.authors = QStringList{QString("Author Number %1").arg(i % 90)};
            book.meta.tags = QStringList{QString("Tag %1").arg(i % 30)};
            book.meta.description = QString("word ").repeated(300);
            book.shelf = i % 3 == 0 ? "read" : i % 3 == 1 ? "to-read" : "";
            book.rating = i % 3 == 0 ? (i / 3) % 6 : 0;
            book.finished = i % 3 == 0 ? QString("20%1-01-01").arg(10 + (i / 3) % 15) : QString();
            books.append({book, i % 2 ? QString("notes ").repeated(200) : QString()});
        }
        const AiRequest request = Curation::suggestionRequest(books);
        const QString asked = request.messages.first().text;
        // Roughly four characters to a token: a few thousand tokens, where
        // the whole library with its descriptions would be several hundred thousand.
        QVERIFY2(asked.size() < 32000, qPrintable(QString::number(asked.size())));
        const QJsonObject summary = QJsonDocument::fromJson(asked.mid(asked.indexOf('{'), asked.lastIndexOf('}') - asked.indexOf('{') + 1).toUtf8()).object();
        QCOMPARE(summary.value("counts").toObject().value("books").toInt(), 1500);
        QCOMPARE(summary.value("counts").toObject().value("read").toInt(), 500);
        QCOMPARE(summary.value("read").toArray().size(), 60);
        QCOMPARE(summary.value("to_read").toArray().size(), 60);
        QVERIFY(!asked.contains("word word"));
        // The latest reading leads, and the strongest feelings fill the rest.
        QCOMPARE(summary.value("read").toArray().first().toObject().value("finished").toString(), QString("2024-01"));
        int strong = 0;
        for (const QJsonValue &book : summary.value("read").toArray())
            if (const int stars = book.toObject().value("stars").toInt(); stars == 5 || stars == 1 || stars == 2)
                ++strong;
        QVERIFY2(strong >= 35, qPrintable(QString::number(strong)));
    }

    void suggestionsAreReadFromTheAnswer() {
        const Curation::Suggestions suggested = Curation::suggestionsFrom(
            "Here you go:\n```json\n{\"suggestions\": [{\"title\": \"The Long Way Home\", \"author\": \"Ann Author\", \"why\": \"You like roads.\"},"
            "{\"title\": \"\", \"author\": \"Nobody\"}, {\"title\": \"Quiet Rivers\"}], \"first\": \"Start with One Day.\"}\n```");
        QCOMPARE(suggested.books.size(), 2);
        QCOMPARE(suggested.books.first().title, QString("The Long Way Home"));
        QCOMPARE(suggested.books.first().author, QString("Ann Author"));
        QCOMPARE(suggested.books.first().why, QString("You like roads."));
        QCOMPARE(suggested.first, QString("Start with One Day."));
        // Prose is no list of books; the caller shows it as it is.
        QVERIFY(Curation::suggestionsFrom("Try Moby-Dick.").books.isEmpty());
        QVERIFY(Curation::suggestionsFrom("{not json}").books.isEmpty());
    }

    void suggestionsAreCheckedAgainstTheSites() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        const qint64 wanted = library.addEntry(meta("Quiet Rivers", "Dee Poet"), false).id;
        QVERIFY(library.setReading(wanted, "to-read", 0, {}));
        const qint64 linked = library.addEntry(meta("The Middle of the Road", "Eve Teller"), true).id;
        QVERIFY(library.setIdentifier(linked, "stub", "77"));

        StubSite down, site;
        down.down = true;
        auto known = [](const QString &id, const QString &title, const QString &author, double rating, qint64 ratings) {
            SiteBook book;
            book.site = "Stub";
            book.scheme = "stub";
            book.siteId = id;
            book.title = title;
            book.authors = QStringList{author};
            book.averageRating = rating;
            book.ratings = ratings;
            book.coverUrl = QUrl("https://images.example.org/" + id + ".jpg");
            return book;
        };
        site.known = {known("1", "The Long Way Home: A Study Guide", "Quick Notes", 5.0, 3),
                      known("2", "The Long Way Home", "Ann B. Author", 4.21, 123456),
                      known("3", "The Long Way Home", "Ann Author", 4.0, 12),
                      known("77", "Half Way", "Eve Teller", 3.5, 100),
                      known("5", "Quiet Rivers", "Dee Poet", 3.9, 40)};

        SuggestionCheck check({&down, &site});
        QSignalSpy done(&check, &SuggestionCheck::done);
        check.check({{"The Long Way Home", "Ann Author", "You like roads."},
                     {"Quiet Rivers", "Dee Poet", "Calm."},
                     {"Half Way", "Eve Teller", "Short."},
                     {"A Book That Was Never Written", "Ann Author", "Made up."}},
                    library.books());
        QCOMPARE(done.count(), 1);
        const QList<ReadingSites::Checked> results = check.results();
        QCOMPARE(results.size(), 4);
        // Found, by this author, in the edition most readers rated; a site
        // that is down is passed over for the next.
        QCOMPARE(results.at(0).state, QString("found"));
        QCOMPARE(results.at(0).book.siteId, QString("2"));
        QCOMPARE(results.at(0).book.averageRating, 4.21);
        QCOMPARE(results.at(0).bookId, 0);
        QCOMPARE(results.at(0).toJson().value("rating").toDouble(), 4.21);
        QCOMPARE(down.askedFor.size(), 4);
        // Already wanted.
        QCOMPARE(results.at(1).state, QString("found"));
        QCOMPARE(results.at(1).bookId, wanted);
        QCOMPARE(results.at(1).shelf, QString("to-read"));
        QCOMPARE(results.at(1).held, QString("none"));
        // Already owned, known here under another spelling but the same number.
        QCOMPARE(results.at(2).bookId, linked);
        QCOMPARE(results.at(2).held, QString("paper"));
        QCOMPARE(results.at(2).toJson().value("in_library").toInteger(), linked);
        // A book no site knows is marked, not passed off as real.
        QCOMPARE(results.at(3).state, QString("missing"));
        QCOMPARE(results.at(3).toJson().value("state").toString(), QString("missing"));

        // With every site down nothing is known either way, and that is said.
        SuggestionCheck blind({&down});
        blind.check({{"The Long Way Home", "Ann Author", {}}}, library.books());
        QCOMPARE(blind.results().first().state, QString("unchecked"));
        SuggestionCheck none(QList<ReadingSite *>{});
        none.check({{"The Long Way Home", "Ann Author", {}}}, library.books());
        QCOMPARE(none.results().first().state, QString("unchecked"));

        // "Add to to-read" makes a book that is only known of, under the
        // site's number, so the next sync finds it again.
        const qint64 added = ReadingSites::addToRead(library, results.at(0));
        QVERIFY(added > 0);
        const Book book = *library.book(added);
        QCOMPARE(book.meta.title, QString("The Long Way Home"));
        QCOMPARE(book.meta.authors, QStringList{"Ann B. Author"});
        QCOMPARE(book.shelf, QString("to-read"));
        QCOMPARE(book.held(), QString("none"));
        QCOMPARE(book.identifiers.value("stub").toString(), QString("2"));
        QCOMPARE(library.books("is:wishlist status:to-read").size(), 2);
        // One that is here already only goes on the shelf, when it is on none.
        QCOMPARE(ReadingSites::addToRead(library, results.at(2)), linked);
        QCOMPARE(library.book(linked)->shelf, QString("to-read"));
        QVERIFY(library.book(linked)->paper);
        QCOMPARE(library.books().size(), 3);
    }

    void coversAreFetchedOneByOneAndStopWhenToldTo() {
        FakeSite covers;
        const QByteArray picture = Fixtures::picture(120, 180, Qt::darkBlue);
        covers.answer = [&](const QString &path) -> FakeSite::Reply {
            if (path == "/1.png" || path == "/2.png")
                return {200, picture};
            if (path == "/tiny.png")
                return {200, Fixtures::picture(1, 1, Qt::white)};
            if (path == "/slow-down.png")
                return {429, "Too many requests"};
            return {404, "no cover"};
        };
        QNetworkAccessManager network;
        CoverFetcher fetcher(&network);
        QSignalSpy cover(&fetcher, &CoverFetcher::cover), done(&fetcher, &CoverFetcher::done);
        auto at = [&](const QString &path) { return QUrl(covers.url() + path); };
        fetcher.fetch({{1, at("/1.png")}, {2, at("/missing.png")}, {3, at("/tiny.png")}, {4, at("/2.png")}});
        QTRY_COMPARE(done.count(), 1);
        // A missing cover and a blank one are passed over.
        QCOMPARE(cover.count(), 2);
        QCOMPARE(cover.at(0).first().toLongLong(), 1);
        QCOMPARE(cover.at(1).first().toLongLong(), 4);
        QCOMPARE(done.first().at(0).toInt(), 2);
        QCOMPARE(done.first().at(1).toInt(), 0);

        // Told to slow down, it stops asking and says how many are left.
        covers.asked.clear();
        fetcher.fetch({{5, at("/1.png")}, {6, at("/slow-down.png")}, {7, at("/2.png")}, {8, at("/2.png")}});
        QTRY_COMPARE(done.count(), 2);
        QCOMPARE(done.at(1).at(0).toInt(), 1);
        QCOMPARE(done.at(1).at(1).toInt(), 3);
        QCOMPARE(covers.asked.size(), 2);
    }

    void whoSomebodyIsIsReadFromWhateverTheyHave() {
        // Goodreads: any link that carries the number, the number, or the
        // name a profile goes by.
        QCOMPARE(Goodreads::identity("https://www.goodreads.com/user/show/12345-ann-reader"), QString("12345"));
        QCOMPARE(Goodreads::identity("goodreads.com/review/list/12345?shelf=to-read"), QString("12345"));
        QCOMPARE(Goodreads::identity("https://www.goodreads.com/review/list_rss/12345?key=AbC-1_2&shelf=read"), QString("12345"));
        QCOMPARE(Goodreads::identity("https://www.goodreads.com/user/updates_rss/12345"), QString("12345"));
        QCOMPARE(Goodreads::identity(" 12345 "), QString("12345"));
        QCOMPARE(Goodreads::identity("12345-ann-reader"), QString("12345"));
        QCOMPARE(Goodreads::identity("https://www.goodreads.com/ann_reader"), QString("ann_reader"));
        QCOMPARE(Goodreads::identity("www.goodreads.com/ann_reader/"), QString("ann_reader"));
        QCOMPARE(Goodreads::identity("ann_reader"), QString("ann_reader"));
        // A book's page, a page of the site's own, or a sentence names nobody.
        QCOMPARE(Goodreads::identity("https://www.goodreads.com/book/show/5907.The_Hobbit"), QString());
        QCOMPARE(Goodreads::identity("https://www.goodreads.com/user"), QString());
        QCOMPARE(Goodreads::identity("that is me, over there"), QString());
        QCOMPARE(Goodreads::identity(""), QString());

        QCOMPARE(OpenLibrary::userName("https://openlibrary.org/people/ann/books/already-read"), QString("ann"));
        QCOMPARE(OpenLibrary::userName("openlibrary.org/people/ann_b"), QString("ann_b"));
        QCOMPARE(OpenLibrary::userName("/people/ann"), QString("ann"));
        QCOMPARE(OpenLibrary::userName("@ann"), QString("ann"));
        QCOMPARE(OpenLibrary::userName(" ann "), QString("ann"));
        QCOMPARE(OpenLibrary::userName("ann reader"), QString());
        // The account a sign-in was for is in the cookie it answers with.
        QCOMPARE(OpenLibrary::userInCookie("/people/ann%2C2026-11-14T10%3A00%3A00%2Cab12c%24hash"), QString("ann"));
        // Its keys come as a pair, however they were copied.
        QVERIFY(OpenLibrary::keys("ACCESSKEY:SECRETKEY").both());
        QCOMPARE(OpenLibrary::keys("ACCESSKEY\n SECRETKEY \n").secret, QString("SECRETKEY"));
        QVERIFY(!OpenLibrary::keys("ACCESSKEY").both());
        QVERIFY(!OpenLibrary::keys("").both());

        QCOMPARE(Hardcover::userName("https://hardcover.app/@ann"), QString("ann"));
        QCOMPARE(Hardcover::userName("@ann"), QString("ann"));
        QCOMPARE(Hardcover::userName("ann"), QString("ann"));
        QCOMPARE(Hardcover::userName("ann reader"), QString());
        // A token is taken as it was copied, with or without "Bearer".
        QCOMPARE(Hardcover::token("Bearer hc_pat_abc123\n"), QString("hc_pat_abc123"));
        QCOMPARE(Hardcover::token("  bearer   hc_pat_abc123 "), QString("hc_pat_abc123"));
        QCOMPARE(Hardcover::token("hc_pat_abc123"), QString("hc_pat_abc123"));

        // The sites say the same of themselves through the one interface.
        QNetworkAccessManager network;
        QObject owner;
        for (const QString &scheme : ReadingSites::schemes()) {
            const ReadingSite *site = ReadingSites::make(scheme, {}, {}, &network, &owner);
            QVERIFY2(site, qPrintable(scheme));
            QCOMPARE(site->scheme(), scheme);
            QVERIFY(site->findMePage().isValid());
        }
        const ReadingSite *goodreads = ReadingSites::make("goodreads", {}, {}, &network, &owner);
        QCOMPARE(goodreads->identityFrom("https://www.goodreads.com/review/list_rss/12345?key=AbC-1_2&shelf=read"), QString("12345"));
        // The key a feed address carries is a secret, and is kept apart.
        QCOMPARE(goodreads->secretIn("https://www.goodreads.com/review/list_rss/12345?key=AbC-1_2&shelf=read"), QString("AbC-1_2"));
        QCOMPARE(ReadingSites::make("hardcover", {}, {}, &network, &owner)->secretUse(), ReadingSite::NeedsSecret);
        QCOMPARE(ReadingSites::make("openlibrary", {}, {}, &network, &owner)->secretUse(), ReadingSite::OptionalSecret);
        QVERIFY(!ReadingSites::make("nowhere", {}, {}, &network, &owner));
    }

    void aSeriesIsTakenOutOfATitle() {
        auto parts = [](const QString &title) {
            const ReadingSites::TitleParts split = ReadingSites::splitSeries(title);
            return QString("%1|%2|%3").arg(split.title, split.series).arg(split.index);
        };
        QCOMPARE(parts("Dune Messiah (Dune, #2)"), QString("Dune Messiah|Dune|2"));
        QCOMPARE(parts("The Test (A Series #2.5)"), QString("The Test|A Series|2.5"));
        // An omnibus counts from where it starts.
        QCOMPARE(parts("The Trilogy (The Road, #1-3)"), QString("The Trilogy|The Road|1"));
        // Brackets inside the series' name.
        QCOMPARE(parts("Night Watch (Discworld (City Watch), #6)"), QString("Night Watch|Discworld (City Watch)|6"));
        // Of two series the first is the one the book is known by.
        QCOMPARE(parts("Guards! (Discworld, #8; City Watch, #1)"), QString("Guards!|Discworld|8"));
        // Brackets that name no series stay part of the title.
        QCOMPARE(parts("The Collection (#1-7)"), QString("The Collection (#1-7)||0"));
        QCOMPARE(parts("A Title (Revised Edition)"), QString("A Title (Revised Edition)||0"));
        QCOMPARE(parts("Plain Title"), QString("Plain Title||0"));
        QCOMPARE(parts("(Dune, #2)"), QString("(Dune, #2)||0"));
    }

    void anEntryIsMatchedToTheBookItCanBe() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        // Two copies of one book: one already known to the site as another
        // edition, one not known to it at all.
        const qint64 linked = library.addEntry(meta("The Long Way Home", "Ann Author"), true).id;
        QVERIFY(library.setIdentifier(linked, "goodreads", "500"));
        const qint64 loose = library.addEntry(meta("Long Way Home, The", "Ann Author"), false).id;
        const QList<Book> books = library.books();
        // An entry with a number of its own is the book that has no number
        // yet, not the one that is another edition there.
        QCOMPARE(ReadingSites::match(books, entry("501", "The Long Way Home", "Ann Author", "read"), "goodreads"), loose);
        QCOMPARE(ReadingSites::match(books, entry("500", "Another Spelling", "Ann Author", "read"), "goodreads"), linked);
        // "The" in front, or behind a comma, makes no other title.
        QCOMPARE(ReadingSites::titleKey("The Long Way Home"), ReadingSites::titleKey("Long Way Home, The"));
        QCOMPARE(ReadingSites::titleKey("A Wizard of Earthsea"), ReadingSites::titleKey("Wizard of Earthsea"));
        QVERIFY(!ReadingSites::titleKey("The").isEmpty());

        // Two editions of one work on a shelf are one book here, counted once.
        const ReadingSites::Summary summary = ReadingSites::merge(
            library, {entry("601", "Quiet Rivers", "Dee Poet", "read", 4), entry("602", "Quiet Rivers (Poems, #1)", "Dee Poet", "read", 4)},
            "goodreads");
        QCOMPARE(summary.added, 1);
        QCOMPARE(library.books().size(), 3);
    }

    void aProfileIsLookedAtAndSaysWhatItFound() {
        const QByteArray profile = "<!DOCTYPE html><html><head><title>Ann Reader (ann) - Utrecht (1,312 books)</title>"
                                   "<meta property=\"og:title\" content=\"Ann Reader\"/></head><body>Ann's shelves</body></html>";
        FakeSite goodreads;
        bool feed = true;
        goodreads.respond = [&](const FakeSite::Request &request) -> FakeSite::Reply {
            if (request.path == "/user/show/12345" || request.path == "/ann_reader")
                return {302, "", "Location: /user/show/12345-ann-reader\r\n"};
            if (request.path == "/user/show/12345-ann-reader")
                return {200, profile};
            if (request.path == "/user/show/777")
                return {302, "", "Location: /user/sign_in?returnurl=%2Fuser%2Fshow%2F777\r\n"};
            if (request.path.startsWith("/user/sign_in"))
                return {200, "<html><head><title>Sign in</title></head></html>"};
            if (request.path == "/user/show/888")
                return {200, "<html><body>a page of another kind</body></html>"};
            if (request.path == "/user/show/429")
                return {429, "slow down"};
            if (request.path.startsWith("/review/list_rss/12345"))
                return feed ? FakeSite::Reply{200, ::feed({{"101", "The Long Way Home"}})} : FakeSite::Reply{404, "<html>Page not found</html>"};
            return {404, "<html>Page not found</html>"};
        };
        QNetworkAccessManager network;
        auto look = [&](const QString &who, SiteProfile *found, QString *why) {
            GoodreadsSite site(who, &network, nullptr, goodreads.url());
            QSignalSpy checked(&site, &ReadingSite::checked), failed(&site, &ReadingSite::failed);
            site.check();
            if (!QTest::qWaitFor([&] { return checked.count() + failed.count() > 0; }))
                return SiteProblem::Changed;
            if (checked.count() == 1)
                *found = checked.first().first().value<SiteProfile>();
            else
                *why = failed.first().first().toString();
            return site.problem();
        };

        // Found: who they are, how many books, and that the shelves can be read.
        SiteProfile found;
        QString why;
        QCOMPARE(look("https://www.goodreads.com/user/show/12345", &found, &why), SiteProblem::None);
        QCOMPARE(found.name, QString("Ann Reader"));
        QCOMPARE(found.books, 1312);
        QCOMPARE(found.identity, QString("12345"));
        QVERIFY(found.shelvesReadable);
        // By the name the profile goes by: the number is found out.
        found = {};
        QCOMPARE(look("goodreads.com/ann_reader", &found, &why), SiteProblem::None);
        QCOMPARE(found.identity, QString("12345"));
        // Found, but the shelves are not shown to apps: said, not failed.
        feed = false;
        found = {};
        QCOMPARE(look("12345", &found, &why), SiteProblem::None);
        QCOMPARE(found.name, QString("Ann Reader"));
        QVERIFY(!found.shelvesReadable);

        // And each way of not being found says which it is.
        QCOMPARE(look("999", &found, &why), SiteProblem::NoUser);
        QVERIFY2(why.contains("knows nobody called 999"), qPrintable(why));
        QCOMPARE(look("777", &found, &why), SiteProblem::Private);
        QVERIFY2(why.contains("signed in"), qPrintable(why));
        QCOMPARE(look("888", &found, &why), SiteProblem::Changed);
        QCOMPARE(look("429", &found, &why), SiteProblem::Refused);
        {
            // Nothing answers at all.
            GoodreadsSite nowhere("12345", &network, nullptr, "http://127.0.0.1:1");
            QSignalSpy failed(&nowhere, &ReadingSite::failed);
            nowhere.check();
            QTRY_COMPARE(failed.count(), 1);
            QCOMPARE(nowhere.problem(), SiteProblem::Unreachable);
            QVERIFY(failed.first().first().toString().contains("can't be reached"));
        }

        // Open Library says how many books a shelf holds when asked for one.
        FakeSite openLibrary;
        int status = 200;
        openLibrary.respond = [&](const FakeSite::Request &request) -> FakeSite::Reply {
            if (status != 200)
                return {status, "{\"detail\": \"no\"}"};
            const int total = request.path.contains("already-read") ? 95 : request.path.contains("currently-reading") ? 21
                              : request.path.contains("want-to-read") ? 390 : 0;
            QJsonArray one;
            if (total > 0)
                one << QJsonObject{{"work", QJsonObject{{"title", "A Book"}, {"key", "/works/OL1W"}}}};
            return {200, QJsonDocument(QJsonObject{{"page", 1}, {"numFound", total}, {"reading_log_entries", one}}).toJson()};
        };
        OpenLibraryLog log("ann", &network, nullptr, openLibrary.url());
        log.setPace(0);
        QSignalSpy checked(&log, &ReadingSite::checked), failed(&log, &ReadingSite::failed);
        log.check();
        QTRY_COMPARE(checked.count(), 1);
        found = checked.first().first().value<SiteProfile>();
        QCOMPARE(found.name, QString("ann"));
        QCOMPARE(found.books, 506);
        QCOMPARE(found.shelves, 3);
        // One book of each shelf was asked for, not the shelves themselves.
        QCOMPARE(openLibrary.asked.size(), 4);
        QVERIFY(openLibrary.asked.first().endsWith("?page=1&limit=1"));
        status = 404;
        log.check();
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(log.problem(), SiteProblem::NoUser);
        status = 403;
        log.check();
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(log.problem(), SiteProblem::Private);
        status = 429;
        log.check();
        QTRY_COMPARE(failed.count(), 3);
        QCOMPARE(log.problem(), SiteProblem::Refused);
    }

    void theDownloadsFolderIsWatchedForTheExport() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath("Downloads"));
        const QString downloads = dir.filePath("Downloads"), root = dir.filePath("lib");
        {
            Library library(root);
            QVERIFY(library.open());
        }
        Settings settings(dir.filePath("config.json"));
        QNetworkAccessManager network;
        TestSecrets secrets;
        SiteDesk desk(&settings, &network, [root] { return root; });
        desk.setSecrets(&secrets);
        desk.setDownloads(downloads);
        QSignalSpy offered(&desk, &SiteDesk::offeredFileChanged), done(&desk, &SiteDesk::done);

        // An export from long ago is not the one being fetched now.
        const QString old = downloads + "/goodreads_library_export (old).csv";
        QVERIFY(Fixtures::write(old, exportFile));
        QFile stale(old);
        QVERIFY(stale.open(QIODevice::ReadWrite));
        QVERIFY(stale.setFileTime(QDateTime::currentDateTime().addDays(-3), QFileDevice::FileModificationTime));
        stale.close();
        desk.watchDownloads(true);
        QCOMPARE(desk.offeredFile(), QString());

        // A download under way has its name already, empty, with the
        // browser's own file beside it: not yet.
        const QString file = downloads + "/goodreads_library_export.csv";
        QVERIFY(Fixtures::write(file + ".part", "half"));
        QVERIFY(Fixtures::write(file, ""));
        QTest::qWait(150);
        QCOMPARE(desk.offeredFile(), QString());
        // Whole, it is offered at once, and only offered.
        QVERIFY(Fixtures::write(file, exportFile));
        QVERIFY(QFile::remove(file + ".part"));
        QTRY_COMPARE(desk.offeredFile(), file);
        QCOMPARE(desk.offeredFileName(), QString("goodreads_library_export.csv"));
        QTest::qWait(100);
        {
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.books().size(), 0);
        }
        QCOMPARE(done.count(), 0);

        // Turned down, it is not offered again; another one is.
        desk.dismissOffer();
        QCOMPARE(desk.offeredFile(), QString());
        QVERIFY(Fixtures::write(downloads + "/something-else.txt", "x"));
        QTest::qWait(150);
        QCOMPARE(desk.offeredFile(), QString());
        const QString second = downloads + "/goodreads_library_export (1).csv";
        QVERIFY(Fixtures::write(second, exportFile));
        QTRY_COMPARE(desk.offeredFile(), second);

        // The click brings it in, and the offer is done with.
        desk.importFile(desk.offeredFile());
        QTRY_COMPARE(done.count(), 1);
        const QJsonObject result = done.first().at(1).toJsonObject();
        QVERIFY(result.value("ok").toBool());
        QCOMPARE(result.value("action").toString(), QString("import"));
        QCOMPARE(result.value("new").toInt(), 4);
        QCOMPARE(desk.offeredFile(), QString());
        {
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.books().size(), 4);
        }
        // Goodreads is connected by its file from now on, and says when.
        const QVariantMap row = desk.rows().first().toMap();
        QCOMPARE(row.value("scheme").toString(), QString("goodreads"));
        QCOMPARE(row.value("state").toString(), QString("connected"));
        QCOMPARE(row.value("syncedText").toString(), QString("Last brought in today"));

        // With the guide closed nothing is watched.
        desk.watchDownloads(false);
        QVERIFY(Fixtures::write(downloads + "/goodreads_library_export (2).csv", exportFile));
        QTest::qWait(150);
        QCOMPARE(desk.offeredFile(), QString());

        // A file that is not Goodreads' says so and changes nothing.
        desk.importFile(downloads + "/something-else.txt");
        QTRY_COMPARE(done.count(), 2);
        QVERIFY(!done.last().at(1).toJsonObject().value("ok").toBool());
        QVERIFY(done.last().at(1).toJsonObject().value("error").toString().contains("not a Goodreads export"));
    }

    void aSiteIsConnectedLookedAtAndForgotten() {
        QTemporaryDir dir;
        const QString root = dir.filePath("lib"), config = dir.filePath("config.json");
        qint64 kept = 0;
        {
            Library library(root);
            QVERIFY(library.open());
            kept = library.addEntry(meta("Kept Whatever Happens", "Ann Author"), true).id;
        }
        Settings settings(config);
        QNetworkAccessManager network;
        TestSecrets secrets;
        QHash<QString, Script> scripts;
        SiteDesk desk(&settings, &network, [root] { return root; });
        desk.setSecrets(&secrets);
        desk.setMaker([&](const QString &scheme, const QString &identity, const QString &secret, QObject *parent) -> ReadingSite * {
            auto *site = new ScriptedSite(scheme, identity, &scripts[scheme], parent);
            if (!secret.isEmpty())
                site->setSecret(secret);
            return site;
        });
        QList<QUrl> opened;
        desk.setOpener([&](const QUrl &url) { opened << url; });
        QSignalSpy done(&desk, &SiteDesk::done);
        auto row = [&](const QString &scheme) {
            for (const QVariant &value : desk.rows())
                if (value.toMap().value("scheme").toString() == scheme)
                    return value.toMap();
            return QVariantMap();
        };
        auto last = [&] { return done.last().at(1).toJsonObject(); };
        const QString token = "hc_pat_SECRET_TOKEN_1";

        // Every site starts the same way: not connected.
        QCOMPARE(desk.rows().size(), 3);
        for (const QString &scheme : ReadingSites::schemes()) {
            QCOMPARE(row(scheme).value("state").toString(), QString("none"));
            QCOMPARE(row(scheme).value("stateText").toString(), QString("Not connected"));
        }
        QCOMPARE(desk.keyringNote(), QString());

        // A site that wants a token says so, and stays as it was.
        desk.connectSite("hardcover", "", "");
        QCOMPARE(done.count(), 1);
        QVERIFY(!last().value("ok").toBool());
        QCOMPARE(last().value("problem").toString(), QString("token"));
        QCOMPARE(row("hardcover").value("state").toString(), QString("none"));

        // With the token, it is looked at at once and says what it found.
        scripts["hardcover"].profile.identity = "ann";
        scripts["hardcover"].profile.name = "Ann Reader";
        scripts["hardcover"].profile.books = 312;
        scripts["hardcover"].profile.shelves = 3;
        desk.connectSite("hardcover", "", token);
        QVERIFY(desk.busy("hardcover"));
        QVERIFY(row("hardcover").value("busy").toBool());
        QTRY_COMPARE(done.count(), 2);
        QVERIFY(last().value("ok").toBool());
        QCOMPARE(last().value("books").toInt(), 312);
        QVariantMap hardcover = row("hardcover");
        QCOMPARE(hardcover.value("state").toString(), QString("connected"));
        QCOMPARE(hardcover.value("stateText").toString(), QString("Connected as Ann Reader"));
        QCOMPARE(hardcover.value("detail").toString(), QString("312 books on 3 shelves"));
        QCOMPARE(hardcover.value("identity").toString(), QString("ann"));
        QVERIFY(hardcover.value("hasSecret").toBool());
        QVERIFY(!hardcover.value("busy").toBool());
        QCOMPARE(scripts["hardcover"].secrets.last(), token);
        // The token is in the keyring and nowhere else: not in the settings
        // file, not in what is listed, not in what any action answered.
        QCOMPARE(secrets.get("hardcover"), token);
        QFile written(config);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QByteArray onDisk = written.readAll();
        QVERIFY(onDisk.contains("\"hardcover\""));
        QVERIFY2(!onDisk.contains("SECRET_TOKEN"), onDisk.constData());
        QVERIFY(!QJsonDocument(desk.list()).toJson().contains("SECRET_TOKEN"));
        for (const QList<QVariant> &answer : done)
            QVERIFY(!QJsonDocument(answer.at(1).toJsonObject()).toJson().contains("SECRET_TOKEN"));
        QVERIFY(!QJsonDocument(QJsonArray::fromVariantList(desk.rows())).toJson().contains("SECRET_TOKEN"));
        const QJsonObject listed = desk.list().at(2).toObject();
        QCOMPARE(listed.value("site").toString(), QString("hardcover"));
        QCOMPARE(listed.value("state").toString(), QString("connected"));
        QCOMPARE(listed.value("connected_as").toString(), QString("Ann Reader"));
        QVERIFY(listed.value("has_token").toBool());

        // The token runs out: it needs attention, and says why.
        scripts["hardcover"].problem = SiteProblem::Secret;
        scripts["hardcover"].message = "Hardcover didn't take the token.";
        desk.checkSite("hardcover");
        QTRY_COMPARE(done.count(), 3);
        QVERIFY(!last().value("ok").toBool());
        QCOMPARE(row("hardcover").value("state").toString(), QString("attention"));
        QCOMPARE(row("hardcover").value("stateText").toString(), QString("Needs attention"));
        QCOMPARE(row("hardcover").value("why").toString(), QString("Hardcover didn't take the token."));
        QCOMPARE(row("hardcover").value("problem").toString(), QString("token"));
        // Put right, it is connected again.
        scripts["hardcover"].problem = SiteProblem::None;
        desk.checkSite("hardcover");
        QTRY_COMPARE(done.count(), 4);
        QCOMPARE(row("hardcover").value("state").toString(), QString("connected"));
        QCOMPARE(row("hardcover").value("why").toString(), QString());
        // A site that can't be reached for a moment is no less connected.
        scripts["hardcover"].problem = SiteProblem::Unreachable;
        scripts["hardcover"].message = "Hardcover can't be reached.";
        desk.checkSite("hardcover");
        QTRY_COMPARE(done.count(), 5);
        QVERIFY(!last().value("ok").toBool());
        QCOMPARE(last().value("problem").toString(), QString("unreachable"));
        QCOMPARE(row("hardcover").value("state").toString(), QString("connected"));
        QCOMPARE(row("hardcover").value("status").toString(), QString("Hardcover can't be reached."));
        scripts["hardcover"].problem = SiteProblem::None;

        // Somebody found whose shelves can't be read needs the file: said
        // as attention, with the profile's own numbers.
        scripts["goodreads"].profile.identity = "12345";
        scripts["goodreads"].profile.name = "Ann Reader";
        scripts["goodreads"].profile.books = 1312;
        scripts["goodreads"].profile.shelvesReadable = false;
        desk.connectSite("goodreads", "12345");
        QTRY_COMPARE(done.count(), 6);
        QVERIFY(last().value("ok").toBool());
        QCOMPARE(row("goodreads").value("state").toString(), QString("attention"));
        QCOMPARE(row("goodreads").value("problem").toString(), QString("file"));
        QVERIFY(row("goodreads").value("why").toString().contains("Ann Reader's profile (1312 books)"));
        QVERIFY(row("goodreads").value("usesFile").toBool());

        // Rubbish pasted over a connection leaves the connection alone.
        ReadingSite *real = ReadingSites::make("goodreads", {}, {}, &network, &desk);
        QCOMPARE(real->identityFrom("that is me, over there"), QString());
        QCOMPARE(desk.identityIn("openlibrary", " ann "), QString("ann"));

        // The pages a site has for finding oneself and one's token open in the browser.
        desk.openPage("hardcover", "find");
        desk.openPage("hardcover", "secret");
        desk.openPage("goodreads", "export");
        QCOMPARE(opened.size(), 3);
        QCOMPARE(opened.at(1), QUrl("https://example.org/hardcover/token"));
        QCOMPARE(opened.at(2), QUrl("https://www.goodreads.com/review/import"));

        // Disconnecting forgets who and the token, and touches no book.
        desk.disconnectSite("hardcover");
        QCOMPARE(last().value("action").toString(), QString("disconnect"));
        QCOMPARE(secrets.get("hardcover"), QString());
        QCOMPARE(row("hardcover").value("state").toString(), QString("none"));
        QCOMPARE(row("hardcover").value("identity").toString(), QString());
        QVERIFY(!row("hardcover").value("hasSecret").toBool());
        QVERIFY(settings.site("hardcover").isEmpty());
        desk.disconnectSite("goodreads");
        QCOMPARE(settings.goodreadsUser(), QString());
        {
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.books().size(), 1);
            QVERIFY(library.book(kept));
        }

        // A site whose plugin is off is asked nothing.
        settings.setPluginEnabled("builtin:Goodreads", false);
        QCOMPARE(row("goodreads").value("state").toString(), QString("off"));
        const int fetches = scripts["goodreads"].fetches;
        desk.syncSite("goodreads");
        QVERIFY(!last().value("ok").toBool());
        QVERIFY(last().value("error").toString().contains("plugin is off"));
        QCOMPARE(scripts["goodreads"].fetches, fetches);

        // Without a keyring the dialog says that tokens won't last.
        SessionSecrets session;
        desk.setSecrets(&session);
        QVERIFY(desk.keyringNote().contains("only until Omabook closes"));
        QVERIFY(session.put("hardcover", token));
        QCOMPARE(session.get("hardcover"), token);
        QVERIFY(session.forget("hardcover"));
        QCOMPARE(session.get("hardcover"), QString());
    }

    void changesGoBothWaysAndNothingIsLost() {
        QTemporaryDir dir;
        Library library(dir.filePath("lib"));
        QVERIFY(library.open());
        const QString scheme = "hardcover";
        // A first sync, one way, brings five books in.
        QList<SiteEntry> site{entry("1", "Alpha", "Ann Author", "read", 4, "2024-01-01"), entry("2", "Beta", "Ann Author", "to-read"),
                              entry("3", "Gamma", "Ann Author", "reading"), entry("4", "Delta", "Ann Author", "read", 3),
                              entry("7", "Omega", "Ann Author", "read", 2)};
        QCOMPARE(ReadingSites::merge(library, site, scheme).added, 5);
        auto id = [&](const QString &title) { return library.books("title:" + title).first().id; };
        auto book = [&](const QString &title) { return *library.book(id(title)); };
        // Two books the site never heard of: one read, one nothing is said of.
        const qint64 epsilon = library.addEntry(meta("Epsilon", "Ann Author", "9780306406157"), true).id;
        QVERIFY(library.setReading(epsilon, "read", 4, {}));
        library.addEntry(meta("Zeta", "Ann Author"), true);

        // Changed here: Alpha's stars, Gamma finished. Changed there: Beta
        // is being read. Changed on both sides: Delta's stars. And Omega was
        // taken off the site, though its stars changed here.
        QVERIFY(library.setReading(id("Alpha"), "read", 5, "2024-01-01"));
        QVERIFY(library.setReading(id("Gamma"), "read", 0, "2024-06-01"));
        QVERIFY(library.setReading(id("Delta"), "read", 5, {}));
        QVERIFY(library.setReading(id("Omega"), "read", 5, {}));
        site = {entry("1", "Alpha", "Ann Author", "read", 4, "2024-01-01"), entry("2", "Beta", "Ann Author", "reading"),
                entry("3", "Gamma", "Ann Author", "reading"), entry("4", "Delta", "Ann Author", "read", 2)};
        ReadingSites::Options both;
        both.writeBack = true;

        // A dry run says all of it and changes none of it.
        both.dryRun = true;
        ReadingSites::Summary summary = ReadingSites::merge(library, site, scheme, both);
        QCOMPARE(summary.updated, 1);
        QCOMPARE(summary.kept, 1);
        QCOMPARE(summary.outgoing.size(), 3);
        QCOMPARE(book("Beta").shelf, QString("to-read"));
        QCOMPARE(book("Beta").identifiers.value("hardcover:synced").toString(), QString("to-read|0|"));
        QCOMPARE(library.books().size(), 7);
        both.dryRun = false;

        summary = ReadingSites::merge(library, site, scheme, both);
        // What changed there is taken.
        QCOMPARE(book("Beta").shelf, QString("reading"));
        // What changed on both sides stays as it is here, is said, and is not sent.
        QCOMPARE(book("Delta").rating, 5);
        QCOMPARE(summary.kept, 1);
        bool said = false;
        for (const ReadingSites::Change &change : summary.changes)
            said = said || (change.title == "Delta" && change.note.contains("5 stars here, 2 on the site"));
        QVERIFY(said);
        // What changed here goes out: only the fields that changed, and the
        // book the site never had, whole.
        QHash<QString, ReadingSites::Outgoing> out;
        for (const ReadingSites::Outgoing &change : summary.outgoing)
            out.insert(change.title, change);
        QCOMPARE(out.size(), 3);
        QCOMPARE(out.value("Alpha").rating, 5);
        QCOMPARE(out.value("Alpha").shelf, QString());
        QCOMPARE(out.value("Alpha").finished, QString());
        QCOMPARE(out.value("Alpha").siteId, QString("1"));
        QCOMPARE(out.value("Alpha").status, QString("read"));
        QVERIFY(!out.value("Alpha").isNew);
        QCOMPARE(out.value("Gamma").shelf, QString("read"));
        QCOMPARE(out.value("Gamma").finished, QString("2024-06-01"));
        QCOMPARE(out.value("Gamma").rating, 0);
        QVERIFY(out.value("Epsilon").isNew);
        QCOMPARE(out.value("Epsilon").shelf, QString("read"));
        QCOMPARE(out.value("Epsilon").rating, 4);
        QCOMPARE(out.value("Epsilon").isbn, QString("9780306406157"));
        QCOMPARE(out.value("Epsilon").siteId, QString());
        // Not the book nothing is said of, and not the one taken off the
        // site: it is not put back there, and it stays here.
        QVERIFY(!out.contains("Zeta") && !out.contains("Omega") && !out.contains("Delta"));
        QCOMPARE(book("Omega").rating, 5);
        QCOMPARE(summary.toJson().value("to_send").toArray().size(), 3);
        QCOMPARE(out.value("Gamma").note(), QString("shelf read, finished 2024-06-01"));

        // Not sent (the site was down): the same changes are still to send.
        QCOMPARE(ReadingSites::merge(library, site, scheme, both).outgoing.size(), 3);
        // Sent: the site now says what Omabook says, and nothing is left to send.
        QList<ReadingSites::Outgoing> taken = summary.outgoing;
        for (ReadingSites::Outgoing &change : taken)
            if (change.isNew)
                change.siteId = "55";
        ReadingSites::sent(library, scheme, taken);
        QCOMPARE(book("Epsilon").identifiers.value("hardcover").toString(), QString("55"));
        site = {entry("1", "Alpha", "Ann Author", "read", 5, "2024-01-01"), entry("2", "Beta", "Ann Author", "reading"),
                entry("3", "Gamma", "Ann Author", "read", 0, "2024-06-01"), entry("4", "Delta", "Ann Author", "read", 2),
                entry("55", "Epsilon", "Ann Author", "read", 4)};
        summary = ReadingSites::merge(library, site, scheme, both);
        QCOMPARE(summary.outgoing.size(), 0);
        QCOMPARE(summary.updated, 0);
        // Delta still differs on both sides, and still says so.
        QCOMPARE(summary.kept, 1);

        // Stars taken off on the site are neither put back there nor taken
        // off here: an empty field never replaces a value, either way.
        site[0].rating = 0;
        summary = ReadingSites::merge(library, site, scheme, both);
        QCOMPARE(summary.outgoing.size(), 0);
        QCOMPARE(book("Alpha").rating, 5);
        // A shelf the site has that Omabook has no word for is left alone
        // there, whatever the book's shelf is here.
        site[1].shelf.clear();
        site[1].otherShelf = true;
        site[1].shelves = QStringList{"did-not-finish"};
        QVERIFY(library.setReading(id("Beta"), "read", 0, {}));
        summary = ReadingSites::merge(library, site, scheme, both);
        for (const ReadingSites::Outgoing &change : summary.outgoing)
            QVERIFY2(change.title != "Beta" || change.shelf.isEmpty(), qPrintable(change.note()));
        QCOMPARE(book("Beta").shelf, QString("read"));

        // With writing off, nothing is gathered to send at all.
        QVERIFY(library.setReading(id("Gamma"), "read", 3, "2024-06-01"));
        QCOMPARE(ReadingSites::merge(library, site, scheme).outgoing.size(), 0);
        // And through all of it no book went away.
        QCOMPARE(library.books().size(), 7);
    }

    void openLibrarySignsInReadsAPrivateLogAndWritesBack() {
        const QByteArray cookie = "%2Fpeople%2Fann%2C2026-11-14T10%3A00%3A00%2Csalt%24hash";
        auto log = [](const QList<QPair<QString, QString>> &works) {
            QJsonArray entries;
            for (const auto &[key, title] : works)
                entries << QJsonObject{{"work", QJsonObject{{"title", title}, {"key", "/works/" + key}, {"author_names", QJsonArray{"Ann Author"}}}},
                                       {"logged_date", "2024/10/22, 00:24:25"}};
            return QJsonDocument(QJsonObject{{"page", 1}, {"numFound", entries.size()}, {"reading_log_entries", entries}}).toJson();
        };
        FakeSite openLibrary;
        bool states = true;
        openLibrary.respond = [&](const FakeSite::Request &request) -> FakeSite::Reply {
            const bool signedIn = request.header("cookie").contains("session=" + cookie);
            if (request.path == "/account/login") {
                const QJsonObject keys = QJsonDocument::fromJson(request.body).object();
                if (request.method == "POST" && keys.value("access").toString() == "ACCESSKEY" && keys.value("secret").toString() == "SECRETKEY")
                    return {200, "", "Set-Cookie: session=" + cookie + "; Path=/; HttpOnly\r\n"};
                return {400, "{\"error\": \"invalid_s3keys\", \"errorDisplayString\": \"These keys are not known\"}"};
            }
            if (request.path.startsWith("/people/")) {
                // The log is private: only its owner, signed in, reads it.
                if (!signedIn)
                    return {403, "{\"detail\": \"This reading log is private\"}"};
                if (request.path.contains("already-read")) return {200, log({{"OL1W", "The Long Way Home"}})};
                if (request.path.contains("currently-reading")) return {200, log({{"OL2W", "Quiet Rivers"}})};
                if (request.path.contains("want-to-read")) return {200, log({{"OL3W", "One Day"}})};
                return {200, log({{"OL4W", "Left Off"}})};
            }
            if (request.path.startsWith("/partials/ReadingState.json")) {
                if (!signedIn) return {401, "{}"};
                if (!states) return {404, "<html>gone</html>"};
                return {200, QJsonDocument(QJsonObject{{"user_key", "/people/ann"}, {"works", QJsonObject{
                    {"OL1W", QJsonObject{{"shelf", 3}, {"rating", 4}, {"read_date", "2024-05-17"}, {"event_id", 77}}},
                    {"OL2W", QJsonObject{{"shelf", 2}, {"rating", QJsonValue()}, {"read_date", QJsonValue()}, {"event_id", QJsonValue()}}},
                    {"OL3W", QJsonObject{{"shelf", 1}}}, {"OL4W", QJsonObject{{"shelf", 4}}}}}}).toJson()};
            }
            if (request.path.startsWith("/search.json?q=isbn%3A9780306406157"))
                return {200, "{\"docs\": [{\"key\": \"/works/OL9W\", \"title\": \"Brand New\", \"author_name\": [\"Ann Author\"]}]}"};
            if (request.path.startsWith("/search.json"))
                return {200, "{\"docs\": []}"};
            if (request.method == "POST" && !signedIn)
                return {401, "{\"detail\": \"Authentication required\"}"};
            if (request.path.endsWith("/ratings.json")) return {200, "{\"success\": \"rating added\"}"};
            if (request.path.endsWith("/bookshelves.json")) return {200, "{\"bookshelves_affected\": 1}"};
            if (request.path.endsWith("/check-ins.json")) return {200, "{\"status\": \"ok\", \"id\": 88}"};
            return {404, "{}"};
        };
        QNetworkAccessManager network;

        // Not signed in, a private log says so, and how to read it.
        OpenLibraryLog stranger("ann", &network, nullptr, openLibrary.url());
        stranger.setPace(0);
        QSignalSpy refused(&stranger, &ReadingSite::failed);
        stranger.fetchShelves();
        QTRY_COMPARE(refused.count(), 1);
        QCOMPARE(stranger.problem(), SiteProblem::Private);
        QVERIFY(refused.first().first().toString().contains("Sign in with your keys"));
        QVERIFY(!stranger.writes());

        // Keys that are wrong are refused, in Open Library's own words, and
        // are not repeated back.
        OpenLibraryLog wrong("ann", &network, nullptr, openLibrary.url());
        wrong.setPace(0);
        wrong.setSecret("ACCESSKEY:NOTTHESECRET");
        QSignalSpy wrongly(&wrong, &ReadingSite::failed);
        wrong.fetchShelves();
        QTRY_COMPARE(wrongly.count(), 1);
        QCOMPARE(wrong.problem(), SiteProblem::Secret);
        QVERIFY(wrongly.first().first().toString().contains("These keys are not known"));
        QVERIFY(!wrongly.first().first().toString().contains("NOTTHESECRET"));

        // Signed in: the log is read, with the reader's own stars and dates.
        openLibrary.requests.clear();
        openLibrary.headers.clear();
        OpenLibraryLog site("", &network, nullptr, openLibrary.url());
        site.setPace(0);
        site.setSecret("ACCESSKEY\nSECRETKEY");
        QVERIFY(site.writes());
        QSignalSpy fetched(&site, &ReadingSite::fetched), failed(&site, &ReadingSite::failed), wrote(&site, &ReadingSite::wrote);
        site.fetchShelves();
        QTRY_COMPARE(fetched.count(), 1);
        QCOMPARE(failed.count(), 0);
        // Whose log it is, the keys said.
        QCOMPARE(site.account(), QString("ann"));
        const QList<SiteEntry> entries = fetched.first().first().value<QList<SiteEntry>>();
        QCOMPARE(entries.size(), 4);
        QCOMPARE(entries.at(0).siteId, QString("OL1W"));
        QCOMPARE(entries.at(0).shelf, QString("read"));
        QCOMPARE(entries.at(0).rating, 4);
        QCOMPARE(entries.at(0).finished, QString("2024-05-17"));
        QCOMPARE(entries.at(1).rating, 0);
        QVERIFY(entries.at(3).otherShelf);
        // The keys were sent once, to sign in, and never again; the sign-in
        // goes along as a cookie of this site's own.
        QCOMPARE(openLibrary.requests.first().path, QString("/account/login"));
        QCOMPARE(openLibrary.requests.first().header("content-type"), QByteArray("application/json"));
        for (const FakeSite::Request &request : openLibrary.requests.mid(1)) {
            QVERIFY(!request.head.contains("SECRETKEY") && !request.body.contains("SECRETKEY"));
            QVERIFY(request.header("cookie").contains("session="));
        }
        QVERIFY(network.cookieJar()->cookiesForUrl(QUrl(openLibrary.url() + "/")).isEmpty());

        // Changes go back: stars, a shelf, a finished date; a date that is
        // there already is changed rather than given a twin; a book the log
        // doesn't have is found by its ISBN; and a wanted book that is rated
        // is put back among the wanted, where rating it moved it from.
        openLibrary.requests.clear();
        auto change = [](qint64 book, const QString &work, const QString &title, const QString &shelf, int rating, const QString &finished,
                         const QString &status) {
            ReadingSites::Outgoing out;
            out.bookId = book;
            out.siteId = work;
            out.title = title;
            out.authors = QStringList{"Ann Author"};
            out.shelf = shelf;
            out.rating = rating;
            out.finished = finished;
            out.status = status;
            return out;
        };
        ReadingSites::Outgoing fresh = change(5, {}, "Brand New", "to-read", 0, {}, "to-read");
        fresh.isbn = "0-306-40615-2";
        fresh.isNew = true;
        site.write({change(1, "OL1W", "The Long Way Home", {}, 5, "2024-05-18", "read"),
                    change(2, "OL2W", "Quiet Rivers", "read", 0, "2024-06", "read"),
                    change(3, "OL3W", "One Day", {}, 3, {}, "to-read"), fresh,
                    change(6, {}, "A Book Nobody Catalogued", "read", 0, {}, "read")});
        QTRY_COMPARE(wrote.count(), 1);
        const QList<SiteWritten> results = wrote.first().first().value<QList<SiteWritten>>();
        QCOMPARE(results.size(), 5);
        for (const SiteWritten &result : results.mid(0, 4))
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QCOMPARE(results.at(3).siteId, QString("OL9W"));
        QVERIFY(results.at(4).error.contains("doesn't know this book"));
        QStringList sent;
        for (const FakeSite::Request &request : openLibrary.requests)
            if (request.method == "POST")
                sent << request.path + " " + QString::fromUtf8(request.body);
        QCOMPARE(sent, (QStringList{
            "/works/OL1W/ratings.json rating=5",
            "/works/OL1W/check-ins.json {\"day\":18,\"event_id\":77,\"event_type\":3,\"month\":5,\"year\":2024}",
            "/works/OL2W/bookshelves.json bookshelf_id=3&dont_remove=true",
            "/works/OL2W/check-ins.json {\"event_id\":null,\"event_type\":3,\"month\":6,\"year\":2024}",
            "/works/OL3W/ratings.json rating=3",
            "/works/OL3W/bookshelves.json bookshelf_id=1&dont_remove=true",
            "/works/OL9W/bookshelves.json bookshelf_id=1&dont_remove=true",
        }));
        // Nothing is ever taken off a shelf there: no removal, and no shelf
        // sent without the word that keeps a repeat from being one.
        for (const FakeSite::Request &request : openLibrary.requests) {
            QVERIFY(request.method != "DELETE");
            QVERIFY(!request.body.contains("bookshelf_id=-1"));
            if (request.path.endsWith("/bookshelves.json"))
                QVERIFY(request.body.contains("dont_remove=true"));
        }

        // Where the dates that are there can't be read, none is written: it
        // could be a second one.
        states = false;
        site.fetchShelves();
        QTRY_COMPARE(fetched.count(), 2);
        QCOMPARE(fetched.last().first().value<QList<SiteEntry>>().size(), 4);
        QCOMPARE(fetched.last().first().value<QList<SiteEntry>>().first().rating, 0);
        openLibrary.requests.clear();
        site.write({change(1, "OL1W", "The Long Way Home", {}, 0, "2024-05-19", "read")});
        QTRY_COMPARE(wrote.count(), 2);
        QVERIFY(wrote.last().first().value<QList<SiteWritten>>().first().error.contains("was not sent"));
        QCOMPARE(openLibrary.requests.size(), 0);
    }

    void hardcoverIsReadSearchedAndWrittenTo() {
        const QString token = "hc_pat_TESTTOKEN";
        auto data = [](const QJsonObject &fields) { return QJsonDocument(QJsonObject{{"data", fields}}).toJson(); };
        const QJsonArray library{
            QJsonObject{{"id", 501}, {"book_id", 11}, {"status_id", 3}, {"rating", 4.5}, {"owned", true}, {"date_added", "2024-01-02"},
                        {"last_read_date", "2024-03-09"},
                        {"user_book_reads", QJsonArray{QJsonObject{{"id", 900}, {"finished_at", "2024-03-09"}}}},
                        {"book", QJsonObject{{"id", 11}, {"title", "The Long Way Home"}, {"slug", "the-long-way-home"}, {"rating", 4.2},
                                             {"release_year", 1999},
                                             {"cached_contributors", QJsonArray{QJsonObject{{"author", QJsonObject{{"name", "Ann Author"}}}, {"contribution", QJsonValue()}},
                                                                                QJsonObject{{"author", QJsonObject{{"name", "Tom Translator"}}}, {"contribution", "Translator"}}}},
                                             {"cached_image", QJsonObject{{"url", "https://images.example.org/11.jpg"}}}}},
                        {"edition", QJsonObject{{"isbn_13", "9780306406157"}, {"edition_format", "Paperback"}, {"reading_format_id", 1}}}},
            QJsonObject{{"id", 502}, {"book_id", 12}, {"status_id", 1}, {"rating", QJsonValue()}, {"owned", false},
                        {"user_book_reads", QJsonArray()}, {"book", QJsonObject{{"id", 12}, {"title", "Quiet Rivers"}, {"cached_contributors", QJsonArray()}}},
                        {"edition", QJsonValue()}},
            QJsonObject{{"id", 503}, {"book_id", 13}, {"status_id", 6}, {"book", QJsonObject{{"id", 13}, {"title", "Never Show Me This"}}}},
            QJsonObject{{"id", 504}, {"book_id", 14}, {"status_id", 5}, {"book", QJsonObject{{"id", 14}, {"title", "Left Off"}}}},
            QJsonObject{{"id", 505}, {"book_id", 15}, {"status_id", 4}, {"book", QJsonObject{{"id", 15}, {"title", "Set Aside"}}},
                        {"edition", QJsonObject{{"reading_format_id", 2}}}, {"owned", true}},
        };
        const QJsonObject hits{{"found", 2}, {"hits", QJsonArray{
            QJsonObject{{"document", QJsonObject{{"id", "11"}, {"title", "The Long Way Home"}, {"slug", "the-long-way-home"},
                                                 {"author_names", QJsonArray{"Ann Author"}}, {"description", "Two travellers. <b>One</b> road."},
                                                 {"genres", QJsonArray{"Fiction", "Travel"}}, {"isbns", QJsonArray{"9780306406157", "0306406152"}},
                                                 {"image", QJsonObject{{"url", "https://images.example.org/11.jpg"}}}, {"rating", 4.2},
                                                 {"ratings_count", 1234}, {"release_year", 1999},
                                                 {"featured_series", QJsonObject{{"series", QJsonObject{{"name", "The Road"}}}}},
                                                 {"featured_series_position", 2}}}},
            QJsonObject{{"document", QJsonObject{{"id", "90"}, {"title", "Cooking for One"}, {"author_names", QJsonArray{"Somebody Else"}}}}}}}};

        FakeSite hardcover;
        int status = 200;
        hardcover.respond = [&](const FakeSite::Request &request) -> FakeSite::Reply {
            if (status != 200)
                return {status, status == 403 ? "{\"error\": \"insufficient_scope\", \"scope\": \"write:library\"}" : "{\"error\": \"invalid_token\"}"};
            const QJsonObject asked = QJsonDocument::fromJson(request.body).object();
            const QString query = asked.value("query").toString();
            const QJsonObject variables = asked.value("variables").toObject();
            if (query.contains("me {")) return {200, data({{"me", QJsonArray{QJsonObject{{"id", 7}, {"username", "ann"}, {"name", "Ann Reader"}}}}})};
            if (query.contains("user_books(where") && query.contains("{ status_id }")) {
                QJsonArray statuses;
                for (const QJsonValue &row : library)
                    statuses << QJsonObject{{"status_id", row.toObject().value("status_id")}};
                return {200, data({{"user_books", statuses}})};
            }
            if (query.contains("user_books(where")) return {200, data({{"user_books", variables.value("offset").toInt() == 0 ? library : QJsonArray()}})};
            if (query.contains("search(")) return {200, data({{"search", QJsonObject{{"results", variables.value("words").toString().contains("Nobody") ? QJsonObject{{"hits", QJsonArray()}} : hits}}}})};
            if (query.contains("editions(")) return {200, data({{"editions", QJsonArray{QJsonObject{{"book", QJsonObject{{"id", 77}, {"title", "Brand New"}}}}}}})};
            if (query.contains("update_user_book(")) return {200, data({{"update_user_book", QJsonObject{{"id", variables.value("id")}, {"error", QJsonValue()}, {"user_book", QJsonObject{{"id", variables.value("id")}, {"user_book_reads", QJsonArray()}}}}}})};
            if (query.contains("insert_user_book(")) return {200, data({{"insert_user_book", QJsonObject{{"id", 600}, {"error", QJsonValue()}, {"user_book", QJsonObject{{"id", 600}, {"user_book_reads", QJsonArray{QJsonObject{{"id", 950}}}}}}}}})};
            if (query.contains("update_user_book_read(")) return {200, data({{"update_user_book_read", QJsonObject{{"id", variables.value("id")}, {"error", QJsonValue()}}}})};
            if (query.contains("insert_user_book_read(")) return {200, data({{"insert_user_book_read", QJsonObject{{"id", 951}, {"error", QJsonValue()}}}})};
            return {200, "{\"errors\": [{\"message\": \"field 'nonsense' not found in type: 'query_root'\"}]}"};
        };
        QNetworkAccessManager network;
        HardcoverSite site("", &network, nullptr, hardcover.url());
        site.setPace(0);
        site.setSecret("Bearer " + token + "\n");
        QSignalSpy checked(&site, &ReadingSite::checked), fetched(&site, &ReadingSite::fetched), failed(&site, &ReadingSite::failed),
            found(&site, &ReadingSite::found), wrote(&site, &ReadingSite::wrote);

        // A look says who the token is for and what they keep there; what
        // they told Hardcover to ignore is not counted.
        site.check();
        QTRY_COMPARE(checked.count(), 1);
        const SiteProfile profile = checked.first().first().value<SiteProfile>();
        QCOMPARE(profile.identity, QString("ann"));
        QCOMPARE(profile.name, QString("Ann Reader"));
        QCOMPARE(profile.books, 4);
        QCOMPARE(profile.shelves, 4);
        QCOMPARE(site.account(), QString("ann"));
        // Every request carries the token as Hardcover wants it, to its one address.
        QCOMPARE(hardcover.requests.first().path, QString("/v1/graphql"));
        QCOMPARE(hardcover.requests.first().method, QByteArray("POST"));
        QCOMPARE(hardcover.requests.first().header("authorization"), ("Bearer " + token).toUtf8());
        QVERIFY(hardcover.requests.first().header("user-agent").startsWith("Omabook/"));

        // The library, a page at a time until one comes back short.
        hardcover.requests.clear();
        site.fetchShelves();
        QTRY_COMPARE(fetched.count(), 1);
        QCOMPARE(hardcover.requests.size(), 1);
        const QList<SiteEntry> entries = fetched.first().first().value<QList<SiteEntry>>();
        QCOMPARE(entries.size(), 4);
        const SiteEntry &first = entries.first();
        QCOMPARE(first.siteId, QString("11"));
        QCOMPARE(first.title, QString("The Long Way Home"));
        // Authors, without the translator.
        QCOMPARE(first.authors, QStringList{"Ann Author"});
        QCOMPARE(first.isbn, QString("9780306406157"));
        QCOMPARE(first.shelf, QString("read"));
        // Half stars are rounded to whole ones.
        QCOMPARE(first.rating, 5);
        QCOMPARE(first.finished, QString("2024-03-09"));
        QCOMPARE(first.added, QString("2024-01-02"));
        QVERIFY(first.paper());
        QCOMPARE(first.coverUrl, QUrl("https://images.example.org/11.jpg"));
        QCOMPARE(first.averageRating, 4.2);
        QCOMPARE(entries.at(1).shelf, QString("to-read"));
        QVERIFY(entries.at(1).isbn.isEmpty() && !entries.at(1).paper());
        // "Did not finish" is no status here, and is not written over there.
        QCOMPARE(entries.at(2).title, QString("Left Off"));
        QVERIFY(entries.at(2).otherShelf && entries.at(2).shelf.isEmpty());
        QCOMPARE(entries.at(2).shelves, QStringList{"did-not-finish"});
        // A paused book is still being read; an owned audiobook is no paper book.
        QCOMPARE(entries.at(3).shelf, QString("reading"));
        QVERIFY(!entries.at(3).paper());

        // A search finds a book with what Hardcover's readers rate it.
        site.find("The Long Way Home", "Ann Author");
        QTRY_COMPARE(found.count(), 1);
        const QList<SiteBook> books = found.first().first().value<QList<SiteBook>>();
        QCOMPARE(books.size(), 2);
        QCOMPARE(books.first().siteId, QString("11"));
        QCOMPARE(books.first().averageRating, 4.2);
        QCOMPARE(books.first().ratings, 1234);
        QCOMPARE(books.first().url, QUrl("https://hardcover.app/books/the-long-way-home"));

        // Changes: a book in the library there is updated (and given a
        // finished date where it had no read to carry one), a read that is
        // there has its date changed, and a book that isn't there is found
        // by its ISBN and added.
        hardcover.requests.clear();
        auto change = [](qint64 book, const QString &theirs, const QString &title, const QString &shelf, int rating, const QString &finished) {
            ReadingSites::Outgoing out;
            out.bookId = book;
            out.siteId = theirs;
            out.title = title;
            out.authors = QStringList{"Ann Author"};
            out.shelf = shelf;
            out.rating = rating;
            out.finished = finished;
            out.status = shelf;
            return out;
        };
        ReadingSites::Outgoing fresh = change(3, {}, "Brand New", "reading", 0, {});
        fresh.isbn = "9780306406157";
        fresh.isNew = true;
        site.write({change(2, "12", "Quiet Rivers", "read", 5, "2024-06-01"), change(1, "11", "The Long Way Home", {}, 0, "2024-03"), fresh,
                    change(4, {}, "A Book Nobody Catalogued", "read", 0, {})});
        QTRY_COMPARE(wrote.count(), 1);
        const QList<SiteWritten> results = wrote.first().first().value<QList<SiteWritten>>();
        QCOMPARE(results.size(), 4);
        QVERIFY2(results.at(0).error.isEmpty() && results.at(1).error.isEmpty() && results.at(2).error.isEmpty(), qPrintable(results.at(0).error));
        QCOMPARE(results.at(2).siteId, QString("77"));
        QVERIFY(results.at(3).error.contains("doesn't know this book"));
        QStringList sent;
        for (const FakeSite::Request &request : hardcover.requests) {
            const QJsonObject asked = QJsonDocument::fromJson(request.body).object();
            const QString query = asked.value("query").toString();
            sent << query.section('{', 1, 1).section('(', 0, 0).trimmed() + " "
                        + QString::fromUtf8(QJsonDocument(asked.value("variables").toObject()).toJson(QJsonDocument::Compact));
            // Nothing is ever removed there.
            QVERIFY2(!query.contains("delete_"), qPrintable(query));
            QCOMPARE(request.header("authorization"), ("Bearer " + token).toUtf8());
        }
        QCOMPARE(sent, (QStringList{
            "update_user_book {\"id\":502,\"object\":{\"rating\":5,\"status_id\":3}}",
            "insert_user_book_read {\"dates\":{\"finished_at\":\"2024-06-01\",\"finished_at_precision\":1},\"id\":502}",
            "update_user_book_read {\"dates\":{\"finished_at\":\"2024-03-01\",\"finished_at_precision\":2},\"id\":900}",
            "editions {\"isbn\":\"9780306406157\"}",
            "insert_user_book {\"object\":{\"book_id\":77,\"status_id\":2}}",
            "search {\"most\":5,\"words\":\"A Book Nobody Catalogued Ann Author\"}",
        }));

        // The same search serves as a look-up, with series, genres and cover.
        HardcoverSource source(&network, nullptr, hardcover.url(), token);
        QCOMPARE(source.name(), QString("Hardcover"));
        QSignalSpy lookedUp(&source, &MetadataSource::found), lookUpFailed(&source, &MetadataSource::failed);
        BookMeta wanted;
        wanted.title = "The Long Way Home (The Road, #2)";
        wanted.authors = QStringList{"Ann Author"};
        source.search(wanted);
        QTRY_COMPARE(lookedUp.count(), 1);
        QList<MetaCandidate> candidates = lookedUp.first().first().value<QList<MetaCandidate>>();
        // What shares neither title nor author is no candidate.
        QCOMPARE(candidates.size(), 1);
        QCOMPARE(candidates.first().source, QString("Hardcover"));
        QCOMPARE(candidates.first().meta.series, QString("The Road"));
        QCOMPARE(candidates.first().meta.seriesIndex, 2.0);
        QCOMPARE(candidates.first().meta.tags, (QStringList{"Fiction", "Travel"}));
        QCOMPARE(candidates.first().meta.description, QString("Two travellers. One road."));
        QCOMPARE(candidates.first().meta.published, QString("1999"));
        QCOMPARE(candidates.first().why, QString("Hardcover readers rate it 4.20 out of 5 (1,234 ratings)."));
        // Asked by ISBN, the ISBN asked for is the book's.
        wanted.isbn = "0-306-40615-2";
        source.search(wanted);
        QTRY_COMPARE(lookedUp.count(), 2);
        QCOMPARE(lookedUp.last().first().value<QList<MetaCandidate>>().first().meta.isbn, QString("9780306406157"));

        // A token that ran out, one that may not write, a site that says
        // "slow down" and an answer of another shape each say what they are,
        // and none repeats the token.
        auto fails = [&](int with) {
            status = with;
            const int before = failed.count();
            site.fetchShelves();
            return QTest::qWaitFor([&] { return failed.count() > before; }) ? failed.last().first().toString() : QString();
        };
        QVERIFY(fails(401).contains("didn't take the token"));
        QCOMPARE(site.problem(), SiteProblem::Secret);
        QVERIFY(fails(403).contains("it lacks write:library"));
        QCOMPARE(site.problem(), SiteProblem::Secret);
        QVERIFY(fails(429).contains("sixty a minute"));
        QCOMPARE(site.problem(), SiteProblem::Refused);
        for (const QList<QVariant> &failure : failed)
            QVERIFY(!failure.first().toString().contains("TESTTOKEN"));
        status = 200;
        source.search(wanted);
        QTRY_COMPARE(lookedUp.count(), 3);
        QString error;
        QVERIFY(Hardcover::parseLibrary("{\"errors\": [{\"message\": \"field 'x' not found\"}]}", nullptr, &error).isEmpty());
        QVERIFY(error.contains("field 'x' not found"));
        QVERIFY(Hardcover::parseLibrary("<html>maintenance</html>", nullptr, &error).isEmpty());
        QVERIFY(error.contains("doesn't understand"));

        // Without a token nothing is asked at all.
        hardcover.requests.clear();
        HardcoverSite unsigned_("ann", &network, nullptr, hardcover.url());
        QSignalSpy unsignedFailed(&unsigned_, &ReadingSite::failed);
        unsigned_.fetchShelves();
        QTRY_COMPARE(unsignedFailed.count(), 1);
        QCOMPARE(unsigned_.problem(), SiteProblem::Secret);
        QVERIFY(!unsigned_.writes());
        QCOMPARE(hardcover.requests.size(), 0);
    }

    void aSyncSendsWhatChangedHereOnceItWasLookedOver() {
        QTemporaryDir dir;
        const QString root = dir.filePath("lib"), config = dir.filePath("config.json");
        Library library(root);
        QVERIFY(library.open());
        const qint64 alpha = library.addEntry(meta("Alpha", "Ann Author"), true).id;
        Settings settings(config);
        QNetworkAccessManager network;
        TestSecrets secrets;
        Script script;
        script.writes = true;
        script.profile.identity = "ann";
        script.profile.name = "Ann Reader";
        script.entries = {entry("1", "Alpha", "Ann Author", "read", 4), entry("2", "Beta", "Ann Author", "to-read")};
        qint64 now = 1'800'000'000;
        SiteDesk desk(&settings, &network, [root] { return root; });
        desk.setSecrets(&secrets);
        desk.setClock([&] { return now; });
        desk.setMaker([&](const QString &scheme, const QString &identity, const QString &secret, QObject *parent) -> ReadingSite * {
            auto *site = new ScriptedSite(scheme, identity, &script, parent);
            if (!secret.isEmpty())
                site->setSecret(secret);
            return site;
        });
        QSignalSpy done(&desk, &SiteDesk::done), reloaded(&desk, &SiteDesk::libraryChanged);
        auto row = [&] { return desk.rows().at(2).toMap(); };
        auto last = [&] { return done.last().at(1).toJsonObject(); };
        auto finished = [&](int count) { return QTest::qWaitFor([&] { return done.count() >= count; }); };
        const QString token = "hc_pat_SECRET_TOKEN_2";

        desk.connectSite("hardcover", "", token);
        QVERIFY(finished(1));
        QCOMPARE(row().value("state").toString(), QString("connected"));
        QVERIFY(row().value("canWrite").toBool());
        // Writing back is off until somebody turns it on.
        QVERIFY(!row().value("write").toBool());

        // A first sync brings the shelves in. With writing off, what was
        // changed here is kept here and nothing goes anywhere.
        desk.syncSite("hardcover");
        QVERIFY(finished(2));
        QVERIFY(last().value("ok").toBool());
        QCOMPARE(last().value("new").toInt(), 1);
        QCOMPARE(last().value("updated").toInt(), 1);
        QCOMPARE(reloaded.count(), 1);
        QCOMPARE(library.books().size(), 2);
        QCOMPARE(library.book(alpha)->rating, 4);
        QCOMPARE(row().value("syncedText").toString(), QString("Last brought in today"));
        QCOMPARE(row().value("status").toString(), QString("1 new, 1 updated, 0 unchanged, 0 kept as they are here."));
        QVERIFY(library.setReading(alpha, "read", 5, {}));
        desk.syncSite("hardcover");
        QVERIFY(finished(3));
        QCOMPARE(last().value("kept").toInt(), 1);
        QVERIFY(!last().contains("to_send"));
        QCOMPARE(script.written.size(), 0);

        // Turned on, the first time changes would go they are held back
        // and shown, not sent.
        desk.setWrite("hardcover", true);
        QVERIFY(row().value("write").toBool());
        desk.syncSite("hardcover");
        QVERIFY(finished(4));
        QCOMPARE(last().value("held").toInt(), 1);
        QCOMPARE(script.written.size(), 0);
        QCOMPARE(row().value("pendingCount").toInt(), 1);
        QCOMPARE(row().value("pending").toStringList(), QStringList{"Alpha: 5 stars"});
        QVERIFY(row().value("status").toString().contains("would go to Hardcover. Look them over, then send them."));
        QVERIFY(!settings.site("hardcover").value("sent").toBool());
        // Looked over and agreed to, they are sent, and are from then on
        // what the site says.
        desk.sendPending("hardcover");
        QVERIFY(finished(5));
        QCOMPARE(last().value("sent").toInt(), 1);
        QCOMPARE(script.written.size(), 1);
        QCOMPARE(script.written.first().title, QString("Alpha"));
        QCOMPARE(script.written.first().rating, 5);
        QCOMPARE(row().value("pendingCount").toInt(), 0);
        QVERIFY(settings.site("hardcover").value("sent").toBool());
        QCOMPARE(library.book(alpha)->identifiers.value("hardcover:synced").toString(), QString("read|5|"));
        script.entries[0].rating = 5;
        desk.syncSite("hardcover");
        QVERIFY(finished(6));
        QCOMPARE(script.written.size(), 1);
        QCOMPARE(last().value("unchanged").toInt(), 2);

        // After that first time a sync sends what changed without asking again.
        QVERIFY(library.setReading(alpha, "read", 3, "2024-07-07"));
        desk.syncSite("hardcover");
        QVERIFY(finished(7));
        QCOMPARE(last().value("sent").toInt(), 1);
        QCOMPARE(script.written.size(), 2);
        QCOMPARE(script.written.last().rating, 3);
        QCOMPARE(script.written.last().finished, QString("2024-07-07"));
        QVERIFY(row().value("status").toString().contains("sent to Hardcover."));
        script.entries[0].rating = 3;
        script.entries[0].finished = "2024-07-07";

        // A dry run says what a sync would do, here and there, and does neither.
        QVERIFY(library.setReading(alpha, "read", 2, "2024-07-07"));
        script.entries[1].shelf = "reading";
        const qint64 synced = settings.site("hardcover").value("synced").toInteger();
        now += 3600;
        SiteDesk::SyncOptions dry;
        dry.dryRun = true;
        desk.sync("hardcover", dry);
        QVERIFY(finished(8));
        QVERIFY(last().value("dry_run").toBool());
        QCOMPARE(last().value("updated").toInt(), 1);
        QCOMPARE(last().value("to_send").toArray().size(), 1);
        QCOMPARE(last().value("to_send").toArray().first().toObject().value("rating").toInt(), 2);
        QCOMPARE(script.written.size(), 2);
        QCOMPARE(library.books("title:Beta").first().shelf, QString("to-read"));
        QCOMPARE(settings.site("hardcover").value("synced").toInteger(), synced);
        QVERIFY(row().value("status").toString().startsWith("Nothing was changed."));

        // A change the site won't take is said, and is still to send next time.
        script.refuses = "Alpha";
        desk.syncSite("hardcover");
        QVERIFY(finished(9));
        QCOMPARE(last().value("sent").toInt(), 0);
        QCOMPARE(last().value("send_failed").toArray().size(), 1);
        QCOMPARE(last().value("send_failed").toArray().first().toObject().value("error").toString(), QString("the site said no"));
        QVERIFY(row().value("status").toString().contains("not taken by Hardcover."));
        script.refuses.clear();
        desk.syncSite("hardcover");
        QVERIFY(finished(10));
        QCOMPARE(last().value("sent").toInt(), 1);
        QCOMPARE(script.written.last().rating, 2);

        // A site that fails in the middle changes nothing and says why.
        script.problem = SiteProblem::Refused;
        script.message = "Hardcover is turning requests away for now.";
        desk.syncSite("hardcover");
        QVERIFY(finished(11));
        QVERIFY(!last().value("ok").toBool());
        QCOMPARE(last().value("problem").toString(), QString("refused"));
        QCOMPARE(row().value("state").toString(), QString("connected"));
        script.problem = SiteProblem::None;

        // Six weeks on, it is said, quietly, that it could be brought in again.
        QCOMPARE(desk.nudge(), QString());
        QVERIFY(!row().value("stale").toBool());
        now += 43 * 24 * 3600;
        QVERIFY(row().value("stale").toBool());
        QCOMPARE(row().value("syncedText").toString(), QString("Last brought in 6 weeks ago"));
        QCOMPARE(desk.nudge(), QString("Hardcover was last brought in 6 weeks ago. Bring it in again?"));
        QVERIFY(desk.list().at(2).toObject().value("stale").toBool());

        // Through all of it: no book went away, the token is where it
        // belongs, and nowhere else.
        QCOMPARE(library.books().size(), 2);
        QCOMPARE(secrets.get("hardcover"), token);
        QFile written(config);
        QVERIFY(written.open(QIODevice::ReadOnly));
        QVERIFY(!written.readAll().contains("SECRET_TOKEN"));
        for (const QList<QVariant> &answer : done)
            QVERIFY(!QJsonDocument(answer.at(1).toJsonObject()).toJson().contains("SECRET_TOKEN"));
        // Turned off again, nothing more is sent, and the first send after
        // turning it on once more is looked over again.
        desk.setWrite("hardcover", false);
        QVERIFY(!settings.site("hardcover").contains("sent"));
        QVERIFY(library.setReading(alpha, "read", 1, "2024-07-07"));
        const int sentSoFar = script.written.size();
        desk.syncSite("hardcover");
        QVERIFY(finished(12));
        QCOMPARE(script.written.size(), sentSoFar);
    }
};

QTEST_MAIN(ReadingSitesTests)
#include "readingsites_tests.moc"
