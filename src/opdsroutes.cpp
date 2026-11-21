#include "opdsroutes.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <ranges>

#include "opdsfeed.h"
#include "privatebooks.h"
#include "sendbook.h"

using namespace Qt::StringLiterals;

namespace {

constexpr qsizetype pageSize = 50;

// What an app shows of the catalog, in the language Omabook is in (bin/i18n reads what tr() is handed).
constexpr auto tr = [](const char *text) { return QCoreApplication::translate("OpdsRoutes", text); };

[[nodiscard]] HttpResponse xml(const QByteArray &body, const char *type = OpdsFeed::acquisitionType) {
    return {200, QByteArray(type) + "; charset=utf-8", body, {}, {}};
}

[[nodiscard]] HttpResponse notFound() { return {404, "text/plain; charset=utf-8", "Not found", {}, {}}; }

[[nodiscard]] HttpResponse askForToken() {
    return {401, "text/plain; charset=utf-8", "The token is needed", {}, {{"WWW-Authenticate", R"(Basic realm="Omabook")"}}};
}

// The token a request carries in its headers: basic authentication's password, or a bearer token.
[[nodiscard]] QString tokenIn(const HttpRequest &request) {
    const QByteArray authorization = request.header("authorization");
    if (authorization.startsWith("Bearer "))
        return QString::fromUtf8(authorization.mid(7).trimmed());
    if (!authorization.startsWith("Basic "))
        return {};
    const QByteArray pair = QByteArray::fromBase64(authorization.mid(6).trimmed());
    return QString::fromUtf8(pair.mid(pair.indexOf(':') + 1));
}

[[nodiscard]] QDateTime newest(const QList<Book> &books) {
    qint64 latest = 0;
    for (const Book &book : books)
        latest = std::max({latest, book.added, book.detailsAt});
    return QDateTime::fromSecsSinceEpoch(latest);
}

void byTitle(QList<Book> &books) {
    std::ranges::sort(books, [](const Book &a, const Book &b) { return QString::localeAwareCompare(a.meta.title, b.meta.title) < 0; });
}

} // namespace

OpdsRoutes::OpdsRoutes(const Library &library, QString token, QString collection)
    : m_library(&library), m_token(std::move(token)), m_collection(std::move(collection)) {}

std::optional<HttpResponse> OpdsRoutes::answer(const HttpRequest &request) const {
    const QString &path = request.path;
    if (path == u"/opds" || path.startsWith(u"/opds/")) {
        if (m_token.isEmpty() || tokenIn(request) != m_token)
            return askForToken();
        return route(request, u"/opds"_s, path.mid(5));
    }
    if (!path.startsWith(u"/t/"))
        return std::nullopt;
    const QString given = path.section('/', 2, 2);
    const QString base = u"/t/"_s + given + u"/opds"_s;
    if (m_token.isEmpty() || given != m_token || !(path == base || path.startsWith(base + '/')))
        return askForToken();
    return route(request, base, path.mid(base.size()));
}

std::optional<Collection> OpdsRoutes::servedCollection() const {
    if (m_collection.isEmpty())
        return std::nullopt;
    return m_library->collectionNamed(m_collection);
}

QList<Book> OpdsRoutes::served() const {
    const auto collection = servedCollection();
    if (!m_collection.isEmpty() && !collection)
        return {}; // the collection chosen is gone: nothing, rather than everything
    const QList<Book> all = m_library->books();
    const QList<Book> books = collection ? collection->of(all) : all;
    const QSet<qint64> withheld = PrivateBooks::withheld(*m_library, books);
    QList<Book> shown;
    std::ranges::copy_if(books, std::back_inserter(shown),
                         [&withheld](const Book &book) { return !book.formats.isEmpty() && !withheld.contains(book.id); });
    return shown;
}

QList<Collection> OpdsRoutes::sections() const {
    const auto collection = servedCollection();
    if (!m_collection.isEmpty() && !collection)
        return {};
    const qint64 parent = collection ? collection->id : 0;
    QList<Collection> within;
    std::ranges::copy_if(m_library->collections(), std::back_inserter(within),
                         [parent](const Collection &each) { return each.parent == parent && !each.removed; });
    return within;
}

HttpResponse OpdsRoutes::route(const HttpRequest &request, const QString &base, const QString &rest) const {
    if (request.method != "GET" && request.method != "HEAD")
        return {405, "text/plain; charset=utf-8", "Read only", {}, {}};
    if (rest.isEmpty() || rest == u"/")
        return root(base);
    if (rest == u"/search.xml")
        return xml(OpdsFeed::openSearch(base), "application/opensearchdescription+xml");
    if (rest == u"/books") {
        QList<Book> books = served();
        byTitle(books);
        return page(base, rest, u"urn:omabook:books"_s, tr("All books"), books, request);
    }
    if (rest == u"/new") {
        QList<Book> books = served();
        std::ranges::sort(books, std::ranges::greater{}, &Book::added);
        return page(base, rest, u"urn:omabook:new"_s, tr("Recently added"), books.first(std::min<qsizetype>(books.size(), 100)), request);
    }
    if (rest == u"/reading") {
        QList<Book> books;
        std::ranges::copy_if(served(), std::back_inserter(books), [](const Book &book) { return book.status() == u"reading"; });
        std::ranges::sort(books, std::ranges::greater{}, &Book::lastRead);
        return page(base, rest, u"urn:omabook:reading"_s, tr("Reading now"), books, request);
    }
    if (rest == u"/search")
        return search(request, base, rest);
    if (rest.startsWith(u"/collections/"))
        return collectionPage(request, base, rest);
    if (rest.startsWith(u"/books/"))
        return bookFile(rest);
    return notFound();
}

HttpResponse OpdsRoutes::search(const HttpRequest &request, const QString &base, const QString &rest) const {
    const QString words = request.query.queryItemValue(u"q"_s, QUrl::FullyDecoded);
    QSet<qint64> found;
    for (const Book &book : m_library->books(words))
        found.insert(book.id);
    QList<Book> books;
    std::ranges::copy_if(served(), std::back_inserter(books), [&found](const Book &book) { return found.contains(book.id); });
    byTitle(books);
    return page(base, rest, u"urn:omabook:search"_s, words, books, request);
}

HttpResponse OpdsRoutes::collectionPage(const HttpRequest &request, const QString &base, const QString &rest) const {
    const qint64 id = rest.section('/', 2, 2).toLongLong();
    const auto collection = m_library->collection(id);
    const auto within = servedCollection();
    // Only the collection served and those inside it.
    if (!collection || collection->removed || (within && collection->id != within->id && !collection->path.startsWith(within->path + '/')))
        return notFound();
    QList<Book> books = collection->of(served());
    byTitle(books);
    return page(base, rest, u"urn:omabook:collection:%1"_s.arg(id), collection->name, books, request);
}

HttpResponse OpdsRoutes::root(const QString &base) const {
    const auto collection = servedCollection();
    QList<OpdsFeed::Section> sections{
        {u"urn:omabook:books"_s, tr("All books"), tr("Every book, by title"), u"/books"_s, true},
        {u"urn:omabook:reading"_s, tr("Reading now"), tr("The books being read, the latest first"), u"/reading"_s, true},
        {u"urn:omabook:new"_s, tr("Recently added"), tr("The books added last"), u"/new"_s, true},
    };
    for (const Collection &each : this->sections())
        sections.append({u"urn:omabook:collection:%1"_s.arg(each.id), each.name, each.path, u"/collections/%1"_s.arg(each.id), true});
    const QString title = collection ? u"Omabook: "_s + collection->name : u"Omabook"_s;
    return xml(OpdsFeed::navigation(base, u"urn:omabook:root"_s, title, sections, newest(served())), OpdsFeed::navigationType);
}

HttpResponse OpdsRoutes::page(const QString &base, const QString &self, const QString &id, const QString &title, QList<Book> books,
                              const HttpRequest &request) const {
    const qsizetype number = std::max(0, request.query.queryItemValue(u"page"_s).toInt());
    const auto at = [&request, &self](qsizetype which) {
        QUrlQuery query;
        if (request.query.hasQueryItem(u"q"_s))
            query.addQueryItem(u"q"_s, request.query.queryItemValue(u"q"_s));
        if (which > 0)
            query.addQueryItem(u"page"_s, QString::number(which));
        return query.isEmpty() ? self : self + '?' + query.toString(QUrl::FullyEncoded);
    };
    OpdsFeed::Page feed{.id = id, .title = title, .self = at(number), .books = {}, .previous = {}, .next = {}, .updated = newest(books)};
    feed.books = books.mid(number * pageSize, pageSize);
    if (number > 0)
        feed.previous = at(number - 1);
    if ((number + 1) * pageSize < books.size())
        feed.next = at(number + 1);
    return xml(OpdsFeed::acquisition(base, feed));
}

HttpResponse OpdsRoutes::bookFile(const QString &rest) const {
    // /books/<id>/cover, or /books/<id>/file/<format>
    const qint64 id = rest.section('/', 2, 2).toLongLong();
    const QString what = rest.section('/', 3, 3);
    const QList<Book> books = served();
    const auto book = std::ranges::find(books, id, &Book::id);
    if (book == books.end())
        return notFound();
    if (what == u"cover" && book->hasCover) {
        const QString cover = m_library->coverPath(id);
        const QString suffix = QFileInfo(cover).suffix().toLower();
        return {200, suffix == u"png" ? "image/png" : "image/jpeg", {}, cover, {}};
    }
    const QString format = rest.section('/', 4, 4).toLower();
    if (what != u"file" || !book->formats.contains(format))
        return notFound();
    const QString name = SendBook::fileName(*book, format);
    return {200,
            OpdsFeed::mimeType(format).toLatin1(),
            {},
            m_library->formatPath(id, format),
            {{"Content-Disposition", "attachment; filename*=UTF-8''" + QUrl::toPercentEncoding(name)}}};
}
