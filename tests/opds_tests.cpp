// The library as an OPDS catalog (#71): served read-only to whoever has its
// token, of the whole library or one collection, with only the books that
// have a file and never one a plugin keeps private; browsed by a stand-in
// app that reads the feeds as OPDS (OpdsCatalog), follows their links and
// downloads a book. And other catalogs read the same way.

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "deviceserver.h"
#include "fakedevice.h"
#include "fixtures.h"
#include "library.h"
#include "opdscatalog.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

const QHash<QByteArray, QByteArray> withToken{{"Authorization", "Basic " + QByteArray("kobo:token").toBase64()}};

// A library of an EPUB in a collection, a PDF, a book with no file and a
// notebook a plugin keeps private.
struct Shelf {
    QTemporaryDir dir;
    Library library{dir.filePath("Library")};
    qint64 alpha = 0, beta = 0, paper = 0, notebook = 0, study = 0, leisure = 0;

    Shelf() {
        if (!library.open())
            return;
        Fixtures::epub(dir.filePath("alpha.epub"), "Alpha");
        Fixtures::pdf(dir.filePath("beta.pdf"), "Beta", {"One page."});
        alpha = library.add(dir.filePath("alpha.epub")).id;
        beta = library.add(dir.filePath("beta.pdf")).id;
        BookMeta meta;
        meta.title = "Gamma";
        paper = library.addEntry(meta, true).id;
        notebook = Fixtures::privateNotebook(library, dir.path(), "My Notebook");
        study = library.addCollection("Study").value_or(0);
        leisure = library.addCollection("Leisure").value_or(0);
        library.putInCollection(study, alpha, true);
        library.putInCollection(study, notebook, true);
        library.putInCollection(leisure, beta, true);
    }
};

[[nodiscard]] DeviceServer::Setup catalog(const QString &collection = {}) {
    DeviceOptions options;
    options.reach = DeviceOptions::Reach::ThisComputer;
    options.port = 0;
    options.collection = collection;
    return {.options = options, .opdsToken = "token", .koreaderKey = {}};
}

// The feed at an address, as an app reads it.
[[nodiscard]] std::expected<OpdsCatalog::Feed, Failure> feedAt(const QUrl &url, const QHash<QByteArray, QByteArray> &headers = withToken) {
    const FakeDevice::Answer answer = FakeDevice::ask("GET", url, headers);
    if (answer.status != 200)
        return std::unexpected(Failure::said(QString::number(answer.status)));
    return OpdsCatalog::parse(answer.body, url);
}

[[nodiscard]] QStringList titles(const OpdsCatalog::Feed &feed) {
    QStringList found;
    for (const OpdsCatalog::Entry &entry : feed.entries)
        found << entry.title;
    return found;
}

[[nodiscard]] std::optional<OpdsCatalog::Entry> entryNamed(const OpdsCatalog::Feed &feed, const QString &title) {
    const auto found = std::ranges::find(feed.entries, title, &OpdsCatalog::Entry::title);
    if (found == feed.entries.end())
        return std::nullopt;
    return *found;
}

} // namespace

class OpdsTest : public QObject {
    Q_OBJECT

private slots:
    void theCatalogIsOnlyForWhoeverHasItsToken() {
        Shelf shelf;
        DeviceServer server(shelf.library.root());
        const auto address = server.start(catalog());
        QVERIFY2(address, qPrintable(whyNot(address)));
        const FakeDevice::Answer none = FakeDevice::ask("GET", *address + "/opds");
        QCOMPARE(none.status, 401);
        QCOMPARE(FakeDevice::ask("GET", *address + "/opds", {{"Authorization", "Basic " + QByteArray("kobo:guess").toBase64()}}).status,
                 401);
        QCOMPARE(FakeDevice::ask("GET", *address + "/opds/books/1/file/epub").status, 401);
        QCOMPARE(FakeDevice::ask("GET", *address + "/opds", withToken).status, 200);
        QCOMPARE(FakeDevice::ask("GET", *address + "/opds", {{"Authorization", "Bearer token"}}).status, 200);
        // In the path, for an app that sends neither.
        QCOMPARE(FakeDevice::ask("GET", *address + "/t/token/opds").status, 200);
        QCOMPARE(FakeDevice::ask("GET", *address + "/t/guess/opds").status, 401);
        // Read only.
        QCOMPARE(FakeDevice::ask("DELETE", *address + "/opds/books/1", withToken).status, 405);
        // KOReader's sync is off, so it isn't there.
        QCOMPARE(FakeDevice::ask("GET", *address + "/users/auth").status, 404);
    }

    void anAppBrowsesTheCatalogAndDownloadsABook() {
        Shelf shelf;
        QVERIFY(shelf.alpha > 0 && shelf.beta > 0 && shelf.notebook > 0 && shelf.study > 0);
        DeviceServer server(shelf.library.root());
        const auto address = server.start(catalog());
        QVERIFY2(address, qPrintable(whyNot(address)));
        const auto root = feedAt(*address + "/opds");
        QVERIFY2(root, qPrintable(whyNot(root)));
        QCOMPARE(root->title, QString("Omabook"));
        QVERIFY2(titles(*root).contains("All books") && titles(*root).contains("Study") && titles(*root).contains("Leisure"),
                 qPrintable(titles(*root).join(", ")));
        QVERIFY(root->link("search"));

        const auto all = feedAt(entryNamed(*root, "All books")->feed()->href);
        QVERIFY2(all, qPrintable(whyNot(all)));
        // Only books with a file, and never the private notebook.
        QCOMPARE(titles(*all), QStringList({"Alpha", "Beta"}));
        const auto alpha = entryNamed(*all, "Alpha");
        QCOMPARE(alpha->acquisitions().size(), 1);
        QCOMPARE(alpha->acquisitions().first().type, QString("application/epub+zip"));
        QCOMPARE(entryNamed(*all, "Beta")->acquisitions().first().type, QString("application/pdf"));
        // The download is the library's file.
        const FakeDevice::Answer file = FakeDevice::ask("GET", alpha->acquisitions().first().href, withToken);
        QCOMPARE(file.status, 200);
        QCOMPARE(file.type, QByteArray("application/epub+zip"));
        QVERIFY2(file.disposition.contains("Alpha"), file.disposition.constData());
        QFile kept(shelf.library.formatPath(shelf.alpha, "epub"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(file.body, kept.readAll());
        // Not even asked for by its number.
        QCOMPARE(FakeDevice::ask("GET", u"%1/opds/books/%2/file/omanotebook"_s.arg(*address).arg(shelf.notebook), withToken).status, 404);
        QCOMPARE(FakeDevice::ask("GET", u"%1/opds/books/%2/file/epub"_s.arg(*address).arg(shelf.paper), withToken).status, 404);

        // A collection holds what it holds, the notebook still left out.
        const auto study = feedAt(entryNamed(*root, "Study")->feed()->href);
        QVERIFY2(study, qPrintable(whyNot(study)));
        QCOMPARE(titles(*study), QStringList({"Alpha"}));
    }

    void oneCollectionIsServedAlone() {
        Shelf shelf;
        DeviceServer server(shelf.library.root());
        const auto address = server.start(catalog("Study"));
        QVERIFY2(address, qPrintable(whyNot(address)));
        const auto root = feedAt(*address + "/t/token/opds");
        QVERIFY2(root, qPrintable(whyNot(root)));
        QCOMPARE(root->title, QString("Omabook: Study"));
        QVERIFY(!titles(*root).contains("Leisure"));
        // The links keep the token in the path.
        const auto all = feedAt(entryNamed(*root, "All books")->feed()->href, {});
        QVERIFY2(all, qPrintable(whyNot(all)));
        QCOMPARE(titles(*all), QStringList({"Alpha"}));
        QCOMPARE(FakeDevice::ask("GET", u"%1/opds/collections/%2"_s.arg(*address).arg(shelf.leisure), withToken).status, 404);
        QCOMPARE(FakeDevice::ask("GET", u"%1/opds/books/%2/file/pdf"_s.arg(*address).arg(shelf.beta), withToken).status, 404);
    }

    void anAppSearchesAndTurnsThePages() {
        Shelf shelf;
        for (int i = 0; i < 55; ++i) {
            const QString path = shelf.dir.filePath(u"note-%1.txt"_s.arg(i, 2, 10, QChar('0')));
            QVERIFY(Fixtures::write(path, "Note " + QByteArray::number(i) + " of many.\n"));
            QVERIFY(shelf.library.add(path).id > 0);
        }
        DeviceServer server(shelf.library.root());
        const auto address = server.start(catalog());
        QVERIFY2(address, qPrintable(whyNot(address)));
        const FakeDevice::Answer search = FakeDevice::ask("GET", *address + "/opds/search.xml", withToken);
        QVERIFY2(search.body.contains("/opds/search?q={searchTerms}"), search.body.constData());
        const auto found = feedAt(*address + "/opds/search?q=alpha");
        QVERIFY2(found, qPrintable(whyNot(found)));
        QCOMPARE(titles(*found), QStringList({"Alpha"}));

        const auto first = feedAt(*address + "/opds/books");
        QVERIFY2(first, qPrintable(whyNot(first)));
        QCOMPARE(first->entries.size(), 50);
        QVERIFY(!first->link("previous"));
        const auto next = feedAt(first->link("next")->href);
        QVERIFY2(next, qPrintable(whyNot(next)));
        QCOMPARE(next->entries.size(), 7);
        QVERIFY(next->link("previous") && !next->link("next"));
    }

    void otherCatalogsAreReadTheSameWay() {
        // A catalog as Standard Ebooks writes one, its links relative to it.
        const QByteArray xml =
            "<?xml version='1.0' encoding='utf-8'?>\n"
            "<feed xmlns='http://www.w3.org/2005/Atom' xmlns:opds='http://opds-spec.org/2010/catalog'>\n"
            "  <id>https://example.org/feeds/opds</id>\n"
            "  <title>Example Ebooks</title>\n"
            "  <link rel='next' href='/feeds/opds/all?page=2' type='application/atom+xml;profile=opds-catalog;kind=acquisition'/>\n"
            "  <entry>\n"
            "    <title>Subjects</title>\n"
            "    <id>subjects</id>\n"
            "    <link rel='subsection' href='subjects' type='application/atom+xml;profile=opds-catalog;kind=navigation'/>\n"
            "  </entry>\n"
            "  <entry>\n"
            "    <title>The Time Machine</title>\n"
            "    <id>urn:example:1</id>\n"
            "    <author><name>H. G. Wells</name></author>\n"
            "    <summary>A traveller in time.</summary>\n"
            "    <link rel='http://opds-spec.org/image' href='/images/1.jpg' type='image/jpeg'/>\n"
            "    <link rel='http://opds-spec.org/acquisition/open-access' href='/ebooks/1.epub' type='application/epub+zip'/>\n"
            "  </entry>\n"
            "</feed>\n";
        const auto feed = OpdsCatalog::parse(xml, QUrl("https://example.org/feeds/opds"));
        QVERIFY2(feed, qPrintable(whyNot(feed)));
        QCOMPARE(feed->title, QString("Example Ebooks"));
        QCOMPARE(feed->link("next")->href, QUrl("https://example.org/feeds/opds/all?page=2"));
        QCOMPARE(feed->entries.size(), 2);
        QCOMPARE(feed->entries.at(0).feed()->href, QUrl("https://example.org/feeds/subjects"));
        const OpdsCatalog::Entry &book = feed->entries.at(1);
        QVERIFY(!book.feed());
        QCOMPARE(book.authors, QStringList({"H. G. Wells"}));
        QCOMPARE(book.summary, QString("A traveller in time."));
        QCOMPARE(book.acquisitions().size(), 1);
        QCOMPARE(book.acquisitions().first().href, QUrl("https://example.org/ebooks/1.epub"));
        const auto page = OpdsCatalog::parse("<html><body>Not here</body></html>", QUrl("https://example.org/"));
        QVERIFY(!page);
        QCOMPARE(page.error().code(), Failure::Code::NotAnOpdsFeed);
    }
};

// A GUI application: the PDF fixture is drawn with a painter.
QTEST_MAIN(OpdsTest)
#include "opds_tests.moc"
