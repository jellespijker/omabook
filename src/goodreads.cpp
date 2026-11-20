#include "goodreads.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimeZone>
#include <QTimer>
#include <QXmlStreamReader>
#include <algorithm>

#include "htmlclean.h"

namespace {

constexpr auto notUnderstood = "Goodreads answered in a way Omabook doesn't understand; its pages may have changed.";

// Goodreads' three shelves every account has, and what Omabook calls them.
struct Shelf {
    const char *theirs;
    const char *ours;
};
constexpr Shelf shelves[] = {{"read", "read"}, {"currently-reading", "reading"}, {"to-read", "to-read"}};

QString ourShelf(const QString &theirs) {
    for (const Shelf &shelf : shelves)
        if (theirs == shelf.theirs)
            return shelf.ours;
    return {};
}

QString theirShelf(const QString &ours) {
    for (const Shelf &shelf : shelves)
        if (ours == shelf.ours)
            return shelf.theirs;
    return {};
}

// ="0306406152" and "0306406152" are both 0306406152.
QString unwrapped(QString field) {
    field = field.trimmed();
    if (field.startsWith('='))
        field.remove(0, 1);
    if (field.size() >= 2 && field.startsWith('"') && field.endsWith('"'))
        field = field.mid(1, field.size() - 2);
    return field.trimmed();
}

// "2024/03/09" as an ISO date; empty for anything that isn't a date.
QString isoDate(const QString &slashed) {
    const QDate date = QDate::fromString(slashed.trimmed(), "yyyy/MM/dd");
    return date.isValid() ? date.toString(Qt::ISODate) : QString();
}

QString slashed(const QString &iso) {
    const QDate date = QDate::fromString(iso.left(10), Qt::ISODate);
    return date.isValid() ? date.toString("yyyy/MM/dd") : QString();
}

// "Sat, 09 Mar 2024 00:00:00 -0800", as the feed writes its days.
QString feedDate(const QString &text) {
    const QDateTime when = QDateTime::fromString(text.simplified(), Qt::RFC2822Date);
    return when.isValid() ? when.date().toString(Qt::ISODate) : QString();
}

// The ten-digit form of a 978 ISBN, which Goodreads lists beside the other.
QString isbn10(const QString &isbn13) {
    if (isbn13.size() != 13 || !isbn13.startsWith("978"))
        return {};
    const QString body = isbn13.mid(3, 9);
    int sum = 0;
    for (int i = 0; i < 9; ++i)
        sum += body.at(i).digitValue() * (10 - i);
    const int check = (11 - sum % 11) % 11;
    return body + (check == 10 ? QString("X") : QString::number(check));
}

// "Ursula K. Le Guin" as "Guin, Ursula K. Le": the form Goodreads sorts by.
QString lastFirst(const QString &author) {
    const QString name = author.simplified();
    return name.contains(' ') ? name.section(' ', -1) + ", " + name.section(' ', 0, -2) : name;
}

// "English" as "en", the way the library writes a language.
QString languageCode(const QString &name) {
    if (name.isEmpty())
        return {};
    for (int language = QLocale::C + 1; language <= QLocale::LastLanguage; ++language)
        if (QLocale::languageToString(QLocale::Language(language)).compare(name, Qt::CaseInsensitive) == 0)
            return QLocale::languageToCode(QLocale::Language(language));
    return {};
}

// The picture at its full size: ".../5907._SY75_.jpg" names a thumbnail.
QUrl fullCover(QString address) {
    static const QRegularExpression size("\\._[A-Z]{2}\\d+_(?=\\.)");
    return QUrl(address.remove(size));
}

// Goodreads answers a book without a cover with a grey placeholder.
bool isCover(const QString &address) {
    return !address.isEmpty() && !address.contains("nophoto") && !address.contains("nocover");
}

QString rated(double average, qint64 ratings) {
    if (average <= 0)
        return {};
    return QString("Goodreads readers rate it %1 out of 5 (%2 ratings).")
        .arg(QString::number(average, 'f', 2), QLocale(QLocale::English).toString(ratings));
}

// The text between a marker inside a <script ...> tag and its </script>.
QByteArray scriptAfter(const QByteArray &html, const QByteArray &marker) {
    const qsizetype at = html.indexOf(marker);
    const qsizetype start = at < 0 ? -1 : html.indexOf('>', at);
    const qsizetype end = start < 0 ? -1 : html.indexOf("</script>", start);
    return end < 0 ? QByteArray() : html.mid(start + 1, end - start - 1);
}

QString day(const QJsonValue &milliseconds) {
    if (!milliseconds.isDouble())
        return {};
    return QDateTime::fromMSecsSinceEpoch(qint64(milliseconds.toDouble()), QTimeZone::UTC).date().toString(Qt::ISODate);
}

// The page's own data: a flat store of everything the page shows, each
// thing under a key and pointing at the others by "__ref".
std::optional<Goodreads::Found> fromPageData(const QByteArray &json) {
    const QJsonObject store = QJsonDocument::fromJson(json).object().value("props").toObject().value("pageProps").toObject()
                                  .value("apolloState").toObject();
    auto follow = [&](const QJsonValue &pointer) { return store.value(pointer.toObject().value("__ref").toString()).toObject(); };

    // The book the page is about is the one the page's query asked for; a
    // page holds a dozen other books (editions, what else readers liked).
    QJsonObject book;
    const QJsonObject root = store.value("ROOT_QUERY").toObject();
    for (auto it = root.begin(); it != root.end() && book.isEmpty(); ++it)
        if (it.key().startsWith("getBookByLegacyId"))
            book = follow(it.value());
    for (auto it = store.begin(); it != store.end() && book.isEmpty(); ++it)
        if (it.key().startsWith("Book:") && it.value().toObject().contains("details"))
            book = it.value().toObject();

    Goodreads::Found found;
    BookMeta &meta = found.candidate.meta;
    meta.title = book.value("title").toString().simplified();
    if (meta.title.isEmpty())
        return std::nullopt;
    if (book.value("legacyId").isDouble())
        found.id = QString::number(book.value("legacyId").toInteger());

    const QString first = follow(book.value("primaryContributorEdge").toObject().value("node")).value("name").toString().simplified();
    if (!first.isEmpty())
        meta.authors << first;
    // Illustrators, editors and translators are listed too; only authors count.
    for (const QJsonValue &edge : book.value("secondaryContributorEdges").toArray()) {
        const QString name = follow(edge.toObject().value("node")).value("name").toString().simplified();
        if (edge.toObject().value("role").toString() == "Author" && !name.isEmpty() && !meta.authors.contains(name))
            meta.authors << name;
    }

    const QJsonObject inSeries = book.value("bookSeries").toArray().first().toObject();
    meta.series = follow(inSeries.value("series")).value("title").toString().simplified();
    // "2", "2.5", or "1-3" for an omnibus, which counts from its start.
    static const QRegularExpression position("^\\s*(\\d+(?:\\.\\d+)?)");
    if (!meta.series.isEmpty())
        meta.seriesIndex = position.match(inSeries.value("userPosition").toString()).captured(1).toDouble();

    const QString plain = book.value("description({\"stripped\":true})").toString();
    meta.description = !plain.isEmpty() ? plain.trimmed() : htmlToText(book.value("description").toString()).trimmed();

    const QJsonObject details = book.value("details").toObject();
    meta.publisher = details.value("publisher").toString().simplified();
    meta.isbn = details.value("isbn13").toString().isEmpty() ? details.value("isbn").toString() : details.value("isbn13").toString();
    meta.language = languageCode(details.value("language").toObject().value("name").toString());

    for (const QJsonValue &genre : book.value("bookGenres").toArray()) {
        const QString name = genre.toObject().value("genre").toObject().value("name").toString().simplified();
        // "Audiobook" is how some readers took it in, not what the book is,
        // and "Science Fiction Fantasy" is two genres the page lists apart too.
        if (!name.isEmpty() && name != "Audiobook" && name != "Science Fiction Fantasy" && meta.tags.size() < 6)
            meta.tags << (name == "Non Fiction" ? QString("Nonfiction") : name);
    }

    // When the book first came out, which is what a reader means by its
    // date; this edition's own date only where the page has no other.
    const QJsonObject work = follow(book.value("work"));
    meta.published = day(work.value("details").toObject().value("publicationTime"));
    if (meta.published.isEmpty())
        meta.published = day(details.value("publicationTime"));
    const QJsonObject stats = work.value("stats").toObject();
    found.averageRating = stats.value("averageRating").toDouble();
    found.ratings = stats.value("ratingsCount").toInteger();

    if (const QString picture = book.value("imageUrl").toString(); isCover(picture))
        found.candidate.coverUrl = fullCover(picture);
    return found;
}

// What the page tells search engines: less, but it has been there longest.
std::optional<Goodreads::Found> fromLinkedData(const QByteArray &json) {
    const QJsonObject book = QJsonDocument::fromJson(json).object();
    Goodreads::Found found;
    BookMeta &meta = found.candidate.meta;
    meta.title = htmlToText(book.value("name").toString()).simplified();
    if (meta.title.isEmpty() || book.value("@type").toString() != "Book")
        return std::nullopt;
    const QJsonValue authors = book.value("author");
    for (const QJsonValue &author : authors.isArray() ? authors.toArray() : QJsonArray{authors})
        if (const QString name = author.toObject().value("name").toString().simplified(); !name.isEmpty() && meta.authors.size() < 1)
            meta.authors << name;
    meta.isbn = book.value("isbn").toString();
    meta.language = languageCode(book.value("inLanguage").toString());
    const QJsonObject rating = book.value("aggregateRating").toObject();
    found.averageRating = rating.value("ratingValue").toDouble();
    found.ratings = rating.value("ratingCount").toInteger();
    if (const QString picture = book.value("image").toString(); isCover(picture))
        found.candidate.coverUrl = fullCover(picture);
    return found;
}

QString httpProblem(QNetworkReply *reply) {
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 429 || status == 403 || status == 503)
        return "Goodreads is turning requests away for now; try again later.";
    if (reply->error() != QNetworkReply::NoError)
        return "Goodreads can't be reached: " + reply->errorString();
    return {};
}

} // namespace

QString Goodreads::userId(const QString &text) {
    static const QRegularExpression inAddress("(?:user/show/|review/list(?:_rss)?/|user_status/list/|user/updates_rss/)(\\d+)");
    static const QRegularExpression alone("^(\\d+)(?:-[^/\\s]*)?$");
    const QString trimmed = text.trimmed();
    if (const auto match = inAddress.match(trimmed); match.hasMatch())
        return match.captured(1);
    return alone.match(trimmed).captured(1);
}

QString Goodreads::identity(const QString &text) {
    if (const QString id = userId(text); !id.isEmpty())
        return id;
    // A profile can have a name of its own: goodreads.com/ann, or "ann".
    static const QRegularExpression named("^(?:(?:https?://)?(?:www\\.)?goodreads\\.com/)?([A-Za-z0-9_.]{2,50})/?(?:[?#].*)?$");
    static const QStringList pages{"book", "user", "review", "author", "list", "shelf", "search", "about", "blog", "group",
                                   "quotes", "genres", "series", "challenges", "giveaway", "news", "choiceawards", "api"};
    const QString name = named.match(text.trimmed()).captured(1);
    return pages.contains(name.toLower()) ? QString() : name;
}

QString Goodreads::feedKey(const QString &text) {
    static const QRegularExpression key("[?&]key=([A-Za-z0-9_-]+)");
    return key.match(text).captured(1);
}

bool Goodreads::parseProfile(const QByteArray &html, SiteProfile *profile) {
    const QString page = QString::fromUtf8(html);
    // <title>Ann Reader (ann) - Utrecht (312 books)</title>, and the name
    // alone where the page tells other sites what it is.
    static const QRegularExpression title("<title>\\s*([^<]*?)\\s*</title>", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression named("og:title[\"']\\s+content=\"([^\"]*)\"");
    static const QRegularExpression counted("\\(([\\d,.]+)\\s+books?\\)");
    const QString heading = htmlToText(title.match(page).captured(1)).simplified();
    QString name = htmlToText(named.match(page).captured(1)).simplified();
    const auto count = counted.match(heading);
    if (name.isEmpty() && count.hasMatch())
        name = heading.section(" (", 0, 0).simplified();
    if (name.isEmpty())
        return false;
    profile->name = name;
    if (count.hasMatch())
        profile->books = QString(count.captured(1)).remove(',').remove('.').toInt();
    return true;
}

ReadingSite::Export Goodreads::parseCsv(const QByteArray &file) {
    ReadingSite::Export result;
    const QList<QStringList> rows = ReadingSites::parseCsv(decodeText(file));
    QHash<QString, int> column;
    for (int i = 0; i < rows.value(0).size(); ++i)
        column.insert(rows.first().at(i).trimmed().toLower(), i);
    if (!column.contains("title") || !column.contains("exclusive shelf")) {
        result.error = "This is not a Goodreads export: it has no Title and Exclusive Shelf columns. Download yours from "
                       "goodreads.com under My Books, Import and export.";
        return result;
    }
    for (const QStringList &row : rows.mid(1)) {
        auto field = [&](const char *name) { return column.contains(name) ? row.value(column.value(name)).trimmed() : QString(); };
        SiteEntry entry;
        entry.siteId = field("book id");
        entry.title = field("title").simplified();
        if (const QString author = field("author").simplified(); !author.isEmpty())
            entry.authors << author;
        for (const QString &author : field("additional authors").split(',', Qt::SkipEmptyParts))
            if (!entry.authors.contains(author.simplified()))
                entry.authors << author.simplified();
        const QString isbn13 = unwrapped(field("isbn13"));
        entry.isbn = isbn13.isEmpty() ? unwrapped(field("isbn")) : isbn13;
        entry.rating = std::clamp(field("my rating").toInt(), 0, 5);
        entry.averageRating = field("average rating").toDouble();
        entry.publisher = field("publisher");
        entry.binding = field("binding");
        entry.published = !field("original publication year").isEmpty() ? field("original publication year") : field("year published");
        entry.finished = isoDate(field("date read"));
        entry.added = isoDate(field("date added"));
        entry.owned = field("owned copies").toInt();
        const QString exclusive = field("exclusive shelf");
        entry.shelf = ourShelf(exclusive);
        // A shelf of the user's own that stands in for the three is kept as a tag.
        if (entry.shelf.isEmpty() && !exclusive.isEmpty()) {
            entry.shelves << exclusive;
            entry.otherShelf = true;
        }
        for (const QString &shelf : field("bookshelves").split(',', Qt::SkipEmptyParts))
            if (ourShelf(shelf.trimmed()).isEmpty() && !entry.shelves.contains(shelf.trimmed()))
                entry.shelves << shelf.trimmed();
        if (!entry.title.isEmpty())
            result.entries << entry;
    }
    return result;
}

QByteArray Goodreads::writeCsv(const QList<Book> &books) {
    using ReadingSites::csvField;
    QStringList lines{"Book Id,Title,Author,Author l-f,Additional Authors,ISBN,ISBN13,My Rating,Average Rating,Publisher,"
                      "Binding,Number of Pages,Year Published,Original Publication Year,Date Read,Date Added,Bookshelves,"
                      "Bookshelves with positions,Exclusive Shelf,My Review,Spoiler,Private Notes,Read Count,Owned Copies"};
    for (const Book &book : books) {
        const QString shelf = theirShelf(book.status());
        // A book nothing is said of has nothing to tell Goodreads.
        if (shelf.isEmpty())
            continue;
        const QString isbn13 = ReadingSites::isbn13(book.meta.isbn);
        const QString year = book.meta.published.left(4);
        const QString held = book.held();
        const QStringList fields{
            book.identifiers.value("goodreads").toString(),
            csvField(book.meta.title),
            csvField(book.meta.authors.value(0)),
            csvField(lastFirst(book.meta.authors.value(0))),
            csvField(book.meta.authors.mid(1).join(", ")),
            "=\"" + isbn10(isbn13) + "\"",
            "=\"" + isbn13 + "\"",
            QString::number(book.rating),
            QString(),
            csvField(book.meta.publisher),
            held == "none" ? QString() : held == "digital" ? QString("ebook") : QString("Paperback"),
            QString(),
            year,
            year,
            shelf == "read" ? slashed(book.finished) : QString(),
            QDateTime::fromSecsSinceEpoch(book.added).date().toString("yyyy/MM/dd"),
            shelf == "read" ? QString() : shelf,
            QString(),
            shelf,
            QString(),
            QString(),
            QString(),
            shelf == "read" ? QString("1") : QString("0"),
            held == "none" ? QString("0") : QString("1"),
        };
        lines << fields.join(',');
    }
    return (lines.join('\n') + "\n").toUtf8();
}

QList<SiteEntry> Goodreads::parseFeed(const QByteArray &xml, const QString &shelf, bool *understood) {
    QList<SiteEntry> entries;
    bool isFeed = false;
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (reader.name() == QLatin1String("rss"))
            isFeed = true;
        if (reader.name() != QLatin1String("item") || !isFeed)
            continue;
        QHash<QString, QString> fields;
        while (reader.readNextStartElement()) {
            const QString name = reader.name().toString();
            // <book> only repeats the page count, nested.
            if (name == "book")
                reader.skipCurrentElement();
            else
                fields.insert(name, reader.readElementText(QXmlStreamReader::IncludeChildElements).trimmed());
        }
        SiteEntry entry;
        entry.siteId = fields.value("book_id");
        entry.title = fields.value("title").simplified();
        if (const QString author = fields.value("author_name").simplified(); !author.isEmpty())
            entry.authors << author;
        entry.isbn = fields.value("isbn");
        entry.shelf = shelf;
        entry.rating = std::clamp(fields.value("user_rating").toInt(), 0, 5);
        entry.finished = feedDate(fields.value("user_read_at"));
        entry.added = feedDate(fields.value("user_date_added"));
        entry.averageRating = fields.value("average_rating").toDouble();
        entry.published = fields.value("book_published");
        for (const QString &other : fields.value("user_shelves").split(',', Qt::SkipEmptyParts))
            if (ourShelf(other.trimmed()).isEmpty())
                entry.shelves << other.trimmed();
        const QString picture = !fields.value("book_large_image_url").isEmpty() ? fields.value("book_large_image_url")
                                                                                 : fields.value("book_image_url");
        // A book without a cover gets Goodreads' grey placeholder, which is no cover.
        if (isCover(picture))
            entry.coverUrl = fullCover(picture);
        if (!entry.title.isEmpty())
            entries << entry;
    }
    if (understood)
        *understood = isFeed && !reader.hasError();
    return entries;
}

QList<Goodreads::Found> Goodreads::parseSuggestions(const QByteArray &json, bool *understood) {
    QList<Found> books;
    const QJsonDocument document = QJsonDocument::fromJson(json);
    if (understood)
        *understood = document.isArray();
    for (const QJsonValue &value : document.array()) {
        const QJsonObject object = value.toObject();
        Found found;
        // The number comes as text, or as a number; either is the number.
        found.id = object.value("bookId").isDouble() ? QString::number(object.value("bookId").toInteger())
                                                      : object.value("bookId").toString();
        BookMeta &meta = found.candidate.meta;
        meta.title = object.value("bookTitleBare").toString().simplified();
        if (meta.title.isEmpty())
            meta.title = object.value("title").toString().simplified();
        if (const QString author = object.value("author").toObject().value("name").toString().simplified(); !author.isEmpty())
            meta.authors << author;
        // The search box cuts descriptions short; half a description is worse than none.
        const QJsonObject description = object.value("description").toObject();
        if (!description.value("truncated").toBool(true))
            meta.description = htmlToText(description.value("html").toString()).trimmed();
        found.averageRating = object.value("avgRating").isString() ? object.value("avgRating").toString().toDouble()
                                                                    : object.value("avgRating").toDouble();
        found.ratings = object.value("ratingsCount").toInteger();
        if (const QString picture = object.value("imageUrl").toString(); isCover(picture))
            found.candidate.coverUrl = fullCover(picture);
        found.candidate.source = "Goodreads";
        found.candidate.why = rated(found.averageRating, found.ratings);
        // "NOT A BOOK" is how Goodreads files calendars, mugs and other
        // things that were given an ISBN.
        if (!found.id.isEmpty() && !meta.title.isEmpty() && meta.authors.value(0).compare("NOT A BOOK", Qt::CaseInsensitive) != 0)
            books << found;
    }
    return books;
}

std::optional<Goodreads::Found> Goodreads::parseBookPage(const QByteArray &html, QString *problem) {
    std::optional<Found> found = fromPageData(scriptAfter(html, "id=\"__NEXT_DATA__\""));
    if (!found)
        found = fromLinkedData(scriptAfter(html, "application/ld+json"));
    if (!found) {
        if (problem)
            *problem = notUnderstood;
        return std::nullopt;
    }
    found->candidate.source = "Goodreads";
    found->candidate.why = rated(found->averageRating, found->ratings);
    return found;
}

GoodreadsSite::GoodreadsSite(const QString &user, QNetworkAccessManager *network, QObject *parent, const QString &base)
    : ReadingSite(parent), m_user(Goodreads::identity(user)), m_key(Goodreads::feedKey(user)), m_base(base), m_network(network) {}

QUrl GoodreadsSite::findMePage() const { return QUrl("https://www.goodreads.com/review/list"); }
QUrl GoodreadsSite::exportPage() { return QUrl("https://www.goodreads.com/review/import"); }

QUrl GoodreadsSite::shelfUrl(const QString &shelf, int page) const {
    QList<QPair<QString, QString>> items{{"shelf", shelf}, {"page", QString::number(page)}};
    if (!m_key.isEmpty())
        items.emplaceBack("key", m_key);
    return ReadingSites::address(m_base + "/review/list_rss/" + m_user, items);
}

QUrl GoodreadsSite::profileUrl() const {
    static const QRegularExpression number("^\\d+$");
    return QUrl(m_base + (number.match(m_user).hasMatch() ? "/user/show/" : "/") + m_user);
}

QUrl GoodreadsSite::findUrl(const QString &title, const QString &author) const {
    return ReadingSites::address(m_base + "/book/auto_complete", {{"format", "json"}, {"q", (title + " " + author).simplified()}});
}

bool GoodreadsSite::named() {
    if (!m_user.isEmpty())
        return true;
    // Answered from the event loop, as every other answer is.
    QMetaObject::invokeMethod(this, [this] {
        fail(SiteProblem::NoIdentity, "Goodreads doesn't know whose shelves to read: put your user id or profile address in the settings "
                                      "(goodreads_user), or import the file Goodreads lets you download.");
    }, Qt::QueuedConnection);
    return false;
}

// The profile page says who somebody is and how many books they keep, to
// anyone. Whether their shelves can be read too is another question, which
// one page of the feed answers.
void GoodreadsSite::check() {
    if (!named())
        return;
    clearProblem();
    QNetworkReply *reply = m_network->get(ReadingSites::request(profileUrl()));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString landed = reply->url().toString();
        SiteProfile profile;
        if (status == 404) {
            fail(SiteProblem::NoUser, QString("Goodreads knows nobody called %1. Open your profile there and copy its address.").arg(m_user));
        } else if (const QString problem = httpProblem(reply); !problem.isEmpty()) {
            fail(reply->error() != QNetworkReply::NoError && status == 0 ? SiteProblem::Unreachable : SiteProblem::Refused, problem);
        } else if (landed.contains("sign_in")) {
            fail(SiteProblem::Private, "This Goodreads profile is only shown to people who are signed in. Bring your shelves in "
                                       "with the file Goodreads lets you download instead.");
        } else if (!Goodreads::parseProfile(reply->readAll(), &profile)) {
            fail(SiteProblem::Changed, notUnderstood);
        } else {
            // A profile reached by its name is known by its number from here on.
            if (const QString id = Goodreads::userId(landed); !id.isEmpty())
                m_user = id;
            profile.identity = m_user;
            checkShelves(profile);
        }
    });
}

void GoodreadsSite::checkShelves(SiteProfile profile) {
    QNetworkReply *reply = m_network->get(ReadingSites::request(shelfUrl("read", 1)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, profile]() mutable {
        reply->deleteLater();
        bool understood = false;
        Goodreads::parseFeed(reply->readAll(), "read", &understood);
        profile.shelvesReadable = understood && reply->error() == QNetworkReply::NoError;
        profile.shelves = 3;
        emit checked(profile);
    });
}

void GoodreadsSite::fetchShelves() {
    if (m_fetching || !named())
        return;
    clearProblem();
    m_fetching = true;
    m_shelf = 0;
    m_page = 1;
    m_firstOfLastPage.clear();
    m_entries.clear();
    fetchPage();
}

void GoodreadsSite::fetchPage() {
    const Shelf shelf = shelves[m_shelf];
    QNetworkReply *reply = m_network->get(ReadingSites::request(shelfUrl(shelf.theirs, m_page)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, shelf] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool understood = false;
        const QList<SiteEntry> page = Goodreads::parseFeed(reply->readAll(), shelf.ours, &understood);
        // Goodreads shows shelves to few who aren't signed in: it answers
        // "not found", or a page to sign in on.
        if (status == 404 || (reply->error() == QNetworkReply::NoError && !understood)) {
            m_fetching = false;
            fail(SiteProblem::Private,
                 QString("Goodreads shows no shelves for user %1 to someone who isn't signed in: the profile is private, "
                         "the id is wrong, or Goodreads no longer offers the feed. Download your library from goodreads.com "
                         "(My Books, Import and export) and import that file instead.").arg(m_user));
            return;
        }
        if (const QString problem = httpProblem(reply); !problem.isEmpty()) {
            m_fetching = false;
            fail(status == 0 ? SiteProblem::Unreachable : SiteProblem::Refused, problem);
            return;
        }
        // The feed has no last page: past the end it is empty, or repeats.
        const QString first = page.value(0).siteId;
        const bool more = !page.isEmpty() && first != m_firstOfLastPage && m_page < 100;
        if (more) {
            m_entries += page;
            m_firstOfLastPage = first;
            ++m_page;
        } else {
            m_firstOfLastPage.clear();
            m_page = 1;
            ++m_shelf;
        }
        if (m_shelf >= int(std::size(shelves))) {
            m_fetching = false;
            emit fetched(m_entries);
            return;
        }
        fetchPage();
    });
}

void GoodreadsSite::find(const QString &title, const QString &author) {
    QNetworkReply *reply = m_network->get(ReadingSites::request(findUrl(title, author)));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (const QString problem = httpProblem(reply); !problem.isEmpty()) {
            emit findFailed(problem);
            return;
        }
        bool understood = false;
        const QList<Goodreads::Found> answers = Goodreads::parseSuggestions(reply->readAll(), &understood);
        if (!understood) {
            emit findFailed(notUnderstood);
            return;
        }
        QList<SiteBook> books;
        for (const Goodreads::Found &answer : answers) {
            SiteBook book;
            book.site = name();
            book.scheme = scheme();
            book.siteId = answer.id;
            book.title = answer.candidate.meta.title;
            book.authors = answer.candidate.meta.authors;
            book.averageRating = answer.averageRating;
            book.ratings = answer.ratings;
            book.coverUrl = answer.candidate.coverUrl;
            book.url = QUrl("https://www.goodreads.com/book/show/" + answer.id);
            books << book;
        }
        emit found(books);
    });
}

QUrl GoodreadsSource::isbnUrl(const QString &isbn) const {
    return ReadingSites::address(m_base + "/book/auto_complete", {{"format", "json"}, {"q", ReadingSites::isbn13(isbn)}});
}

QUrl GoodreadsSource::searchUrl(const BookMeta &book, bool withAuthor) const {
    // The search box does best with the title as it is on the cover: no
    // series in brackets, and no subtitle.
    QString title = ReadingSites::splitSeries(book.title).title;
    if (const QString main = title.section(": ", 0, 0).simplified(); main.count(' ') >= 1 || main.size() >= 8)
        title = main;
    return ReadingSites::address(m_base + "/book/auto_complete",
                                 {{"format", "json"}, {"q", (title + " " + (withAuthor ? book.authors.value(0) : QString())).simplified()}});
}

// The page's address with ".xml" on it is the same page; the bare address
// is the one Goodreads' firewall answers with nothing.
QUrl GoodreadsSource::bookUrl(const QString &id) const { return QUrl(m_base + "/book/show/" + id + ".xml"); }

void GoodreadsSource::search(const BookMeta &book) {
    if (ReadingSites::isbn13(book.isbn).isEmpty()) {
        searchByTitle(book, !book.authors.isEmpty());
        return;
    }
    // The search box knows a book by its ISBN, and says which edition it is.
    QNetworkReply *reply = m_network->get(ReadingSites::request(isbnUrl(book.isbn)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, book] {
        reply->deleteLater();
        const QList<Goodreads::Found> answers = reply->error() == QNetworkReply::NoError
                                                    ? Goodreads::parseSuggestions(reply->readAll()) : QList<Goodreads::Found>();
        if (!answers.isEmpty())
            fetchPage(answers.first().id, {answers.first().candidate}, 0);
        else if (!book.title.trimmed().isEmpty())
            // Goodreads has no edition with this ISBN; the title may find the book.
            searchByTitle(book, !book.authors.isEmpty());
        else if (const QString problem = httpProblem(reply); !problem.isEmpty())
            emit failed(Failure::said(problem));
        else
            emit found({});
    });
}

void GoodreadsSource::searchByTitle(const BookMeta &book, bool withAuthor) {
    if (book.title.trimmed().isEmpty()) {
        QMetaObject::invokeMethod(this, [this] { emit found({}); }, Qt::QueuedConnection);
        return;
    }
    QNetworkReply *reply = m_network->get(ReadingSites::request(searchUrl(book, withAuthor)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, book, withAuthor] {
        reply->deleteLater();
        if (const QString problem = httpProblem(reply); !problem.isEmpty()) {
            emit failed(Failure::said(problem));
            return;
        }
        bool understood = false;
        QList<Goodreads::Found> answers = Goodreads::parseSuggestions(reply->readAll(), &understood);
        if (!understood) {
            emit failed(Failure::said(notUnderstood));
            return;
        }
        // The search box puts study guides and books about the book beside
        // the book itself; the one by this author under this title leads,
        // and what shares neither is not this book at all.
        auto fits = [&book](const Goodreads::Found &answer) {
            const BookMeta &meta = answer.candidate.meta;
            return (!book.authors.isEmpty() && ReadingSites::sameAuthor(book.authors.first(), meta.authors) ? 2 : 0)
                   + (ReadingSites::sameTitle(book.title, meta.title) ? 1 : 0);
        };
        answers.removeIf([&](const Goodreads::Found &answer) { return fits(answer) == 0; });
        std::ranges::stable_sort(answers, [&](const Goodreads::Found &a, const Goodreads::Found &b) {
            return fits(a) != fits(b) ? fits(a) > fits(b) : a.ratings > b.ratings;
        });
        QList<MetaCandidate> candidates;
        for (const Goodreads::Found &answer : answers.mid(0, 4))
            candidates << answer.candidate;
        if (candidates.isEmpty()) {
            // An author spelled another way can hide a book the title finds.
            if (withAuthor)
                searchByTitle(book, false);
            else
                emit found({});
            return;
        }
        // The search box knows little of a book; the first one's own page
        // has the rest. One more request, not one per result.
        fetchPage(answers.first().id, candidates, 0);
    });
}

void GoodreadsSource::fetchPage(const QString &id, const QList<MetaCandidate> &others, int attempt) {
    QNetworkReply *reply = m_network->get(ReadingSites::request(bookUrl(id)));
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, others, attempt] {
        reply->deleteLater();
        QString problem = httpProblem(reply);
        std::optional<Goodreads::Found> page;
        if (problem.isEmpty())
            page = Goodreads::parseBookPage(reply->readAll(), &problem);
        if (page) {
            emit found(QList<MetaCandidate>{page->candidate} + others.mid(1));
        } else if (attempt < 2 && reply->error() == QNetworkReply::NoError) {
            // Goodreads now and then answers with an empty shell of a page;
            // asked again a moment later it sends the real one.
            QTimer::singleShot(m_retryAfter, this, [this, id, others, attempt] { fetchPage(id, others, attempt + 1); });
        } else if (!others.isEmpty()) {
            // What the search box said is still true.
            emit found(others);
        } else {
            emit failed(Failure::said(problem));
        }
    });
}
