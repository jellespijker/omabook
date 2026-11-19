// Sharing by link (#7): a shared collection sent to a gist, secret unless
// asked otherwise, and to the same gist each time; imported from a gist or a
// repository's file, and followed there, a newer version offered with what
// it changes and taken in only when asked. Against the fake GitHub
// (fakegithub.h) and a fake gh (fakegh.cpp); nothing here reaches the network.

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "collectionshare.h"
#include "fakegithub.h"
#include "fixtures.h"
#include "following.h"
#include "githublink.h"
#include "graph.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "spacelinks.h"
#include "spacesnapshot.h"
#include "subscribedspace.h"
#include "subscriptions.h"

namespace {

template <typename Answer> [[nodiscard]] QString whyNot(const Answer &answer) { return answer ? QString() : answer.error(); }

[[nodiscard]] PluginSources::Service at(const QString &api) {
    PluginSources::Service service;
    service.api = api;
    service.raw = api + "/raw";
    service.hosts = QStringList{"127.0.0.1"};
    service.httpsOnly = false;
    return service;
}

// An address on this computer where nothing answers.
[[nodiscard]] QString nowhere() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost);
    const QString address = QString("http://127.0.0.1:%1").arg(server.serverPort());
    server.close();
    return address;
}

[[nodiscard]] SpaceSnapshot::Owner ada() {
    static const auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return {.name = "Ada Fixture", .person = person->id()};
}

// GitHub, as far as sharing by link asks it: the fake one, which the fake
// gh finds where OMABOOK_GITHUB_API says, as the command line's stand-in does.
struct Hub {
    Hub() { qputenv("OMABOOK_GITHUB_API", github.api().toUtf8()); }
    ~Hub() { qunsetenv("OMABOOK_GITHUB_API"); }
    Hub(const Hub &) = delete;
    Hub &operator=(const Hub &) = delete;
    Hub(Hub &&) = delete;
    Hub &operator=(Hub &&) = delete;

    [[nodiscard]] QString onlyGist() const { return github.gists.size() == 1 ? github.gists.keys().first() : QString(); }

    FakeGitHub github;
    GitHubLink link{FAKE_GH, at(github.api())};
};

// Someone who shares a collection, "Shelf": a series inside it by its rule,
// a book put in by hand, and what the graph knows of the first.
struct Owner {
    Owner() : library(dir.filePath("Owner")) {
        QVERIFY(library.open());
        dune = book("Dune", "Saga", 1, "9780441172719");
        messiah = book("Dune Messiah", "Saga", 2, "9780593098233");
        const qint64 picked = book("Picked by Hand", {}, 0, "9780000000026");
        QVERIFY(dune > 0 && messiah > 0 && picked > 0);
        const auto made = library.addCollection("Shelf");
        QVERIFY(made.has_value());
        shelf = *made;
        QVERIFY(library.addCollection("Saga", "series:Saga", shelf).has_value());
        QVERIFY(library.putInCollection(shelf, picked, true));
        Graph graph(library);
        QVERIFY(graph.upsert({}, "character", "Paul Atreides", "The duke's son.", {}, GraphPlace{dune, 1, 0.2}, "Paul") > 0);
    }

    qint64 book(const QString &title, const QString &series, double index, const QString &isbn) {
        const QString file = dir.filePath(title + ".epub");
        if (!Fixtures::epub(file, title))
            return 0;
        const qint64 id = library.add(file).id;
        BookMeta meta = library.book(id)->meta;
        meta.authors = {"Bo Writer"};
        meta.series = series;
        meta.seriesIndex = index;
        meta.isbn = isbn;
        return library.setMeta(id, meta) ? id : 0;
    }

    // `omabook collections share Shelf --gist`, as it does it.
    [[nodiscard]] std::expected<QJsonObject, QString> share(SpaceTransport &transport, bool open = false) {
        const auto published = PublishedSpace::publish(library, shelf, {.graph = true}, ada());
        if (!published)
            return std::unexpected(published.error());
        space = published->space;
        return CollectionShare::send(library, space, transport, open);
    }

    // A change those following it are offered: a book more, a title changed.
    void changeSomething() {
        const qint64 later = book("Later Still", {}, 0, "9780000000033");
        QVERIFY(later > 0 && library.putInCollection(shelf, later, true));
        BookMeta renamed = library.book(messiah)->meta;
        renamed.title = "Dune Messiah (Revised)";
        QVERIFY(library.setMeta(messiah, renamed));
    }

    QTemporaryDir dir;
    Library library;
    qint64 dune = 0, messiah = 0, shelf = 0;
    QString space;
};

struct Receiver {
    Receiver() : library(dir.filePath("Receiver")) { QVERIFY(library.open()); }

    [[nodiscard]] qsizetype held(const QString &space) const { return SubscribedSpace::operations(library, space).size(); }
    [[nodiscard]] bool hasBook(const QString &title) const {
        const QList<Book> books = library.books();
        return std::ranges::any_of(books, [&title](const Book &book) { return book.meta.title == title; });
    }

    QTemporaryDir dir;
    Library library;
};

[[nodiscard]] QStringList titles(const SpaceSnapshot &snapshot) {
    QStringList titles;
    for (const ReducedBook &book : SpaceReducer::reduce(snapshot.operations).books)
        if (!SpaceReducer::isRemoved(book.alive, book.removed))
            titles << book.fields.value("title").value;
    titles.sort();
    return titles;
}

struct Ran {
    int code = -1;
    QString output, errors;
};

// The real binary, run while this thread goes on answering as GitHub.
[[nodiscard]] Ran omabook(const QStringList &arguments, const QProcessEnvironment &environment, const QByteArray &input = {}) {
    QProcess process;
    process.setProcessEnvironment(environment);
    QSignalSpy finished(&process, &QProcess::finished);
    process.start(OMABOOK_BINARY, arguments);
    process.write(input);
    process.closeWriteChannel();
    Ran ran;
    if (!finished.wait(60000)) {
        process.kill();
        process.waitForFinished(2000);
        return ran;
    }
    ran.code = process.exitCode();
    ran.output = QString::fromUtf8(process.readAllStandardOutput());
    ran.errors = QString::fromUtf8(process.readAllStandardError());
    return ran;
}

// A person of their own (Person::userKeyFile), the fake gh first on PATH and
// GitHub where the fake one is.
[[nodiscard]] QProcessEnvironment environmentOf(const QString &dataHome, const Hub &hub) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_DATA_HOME", dataHome);
    environment.insert("OMABOOK_GITHUB_API", hub.github.api());
    environment.insert("PATH", QFileInfo(FAKE_GH).absolutePath() + ":" + environment.value("PATH"));
    return environment;
}

} // namespace

class LinkingTests : public QObject {
    Q_OBJECT

private slots:
    void aLinkIsAGistOrAFileOfARepository() {
        const QString id(32, QChar(u'e')); // a gist id is 20 to 40 hex digits
        for (const QString &text : {id, "https://gist.github.com/" + id, "https://gist.github.com/ada/" + id,
                                    "gist.github.com/ada/" + id + "/0a1b2c", "https://gist.githubusercontent.com/ada/" + id + "/raw/x"}) {
            const auto address = GitHubLink::parse(text);
            QVERIFY2(address && address->gist == id, qPrintable(text + ": " + whyNot(address)));
        }
        for (const QString &text :
             {QString("https://github.com/someone/shelf/blob/main/shared/Shelf.omabook-space"),
              QString("https://raw.githubusercontent.com/someone/shelf/main/shared/Shelf.omabook-space"),
              QString("https://raw.githubusercontent.com/someone/shelf/refs/heads/main/shared/Shelf.omabook-space")}) {
            const auto address = GitHubLink::parse(text);
            QVERIFY2(address && !address->isGist(), qPrintable(text + ": " + whyNot(address)));
            QCOMPARE(address->owner + "/" + address->repo + "@" + address->ref + ":" + address->path,
                     QString("someone/shelf@main:shared/Shelf.omabook-space"));
        }
        const auto repository = GitHubLink::parse("https://github.com/someone/shelf");
        QVERIFY(!repository && repository.error().contains("not a file"));
        QVERIFY(!GitHubLink::parse("https://example.com/Shelf.omabook-space"));
        QVERIFY(!GitHubLink::parse("Shelf.omabook-space"));
    }

    // Shared once: a new gist, with the snapshot in it, its link printed and
    // kept. Changed and shared again: the same gist, brought up to date.
    void sharingToAGistMakesOneAndThenUpdatesIt() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto first = owner.share(hub.link);
        QVERIFY2(first, qPrintable(whyNot(first)));
        const QString id = hub.onlyGist();
        QVERIFY(!id.isEmpty());
        QVERIFY(first->value("created").toBool());
        QCOMPARE(first->value("address").toString(), "https://gist.github.com/ada/" + id);
        QCOMPARE(SpaceLinks::of(owner.library, owner.space, SpaceLinks::Role::Sent, "github")->address, first->value("address").toString());
        const auto sent = SpaceSnapshotFile::read(hub.github.gists.value(id).files.value(GitHubLink::fileName));
        QVERIFY2(sent, qPrintable(whyNot(sent)));
        QCOMPARE(sent->space, owner.space);
        QCOMPARE(titles(*sent), (QStringList{"Dune", "Dune Messiah", "Picked by Hand"}));

        owner.changeSomething();
        const auto second = owner.share(hub.link);
        QVERIFY2(second, qPrintable(whyNot(second)));
        QCOMPARE(hub.onlyGist(), id);
        QVERIFY(!second->value("created").toBool());
        QCOMPARE(second->value("address").toString(), first->value("address").toString());
        const auto updated = SpaceSnapshotFile::read(hub.github.gists.value(id).files.value(GitHubLink::fileName));
        QVERIFY2(updated, qPrintable(whyNot(updated)));
        QCOMPARE(titles(*updated), (QStringList{"Dune", "Dune Messiah (Revised)", "Later Still", "Picked by Hand"}));

        // One deleted on GitHub meanwhile is replaced, and the person told.
        hub.github.gists.clear();
        const auto again = owner.share(hub.link);
        QVERIFY2(again, qPrintable(whyNot(again)));
        QVERIFY(again->value("created").toBool() && again->value("note").toString().contains("is gone"));
        QVERIFY(!hub.onlyGist().isEmpty() && hub.onlyGist() != id);
    }

    void aGistIsSecretUnlessAskedToBePublic() {
        Hub hub;
        Owner secret;
        QVERIFY(!QTest::currentTestFailed());
        const auto shared = secret.share(hub.link);
        QVERIFY2(shared, qPrintable(whyNot(shared)));
        QVERIFY(!shared->value("public").toBool() && !hub.github.gists.value(hub.onlyGist()).open);
        // A gist keeps what it was made as, and says so.
        const auto asked = secret.share(hub.link, true);
        QVERIFY2(asked, qPrintable(whyNot(asked)));
        QVERIFY(!hub.github.gists.value(hub.onlyGist()).open && asked->value("note").toString().contains("still secret"));

        Hub other;
        Owner open;
        QVERIFY(!QTest::currentTestFailed());
        const auto made = open.share(other.link, true);
        QVERIFY2(made, qPrintable(whyNot(made)));
        QVERIFY(made->value("public").toBool() && other.github.gists.value(other.onlyGist()).open);
    }

    // Imported by a gist's link (or its id), the subscription is followed
    // from it; a dry run remembers nothing. A large file comes whole.
    void aCollectionIsImportedByItsGistsLink() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto shared = owner.share(hub.link);
        QVERIFY2(shared, qPrintable(whyNot(shared)));
        const QString link = shared->value("address").toString();

        Receiver dry;
        const auto planned = Following::subscribe(dry.library, hub.onlyGist(), hub.link, {.as = {}, .dryRun = true});
        QVERIFY2(planned && planned->dryRun, qPrintable(whyNot(planned)));
        QVERIFY(SpaceLinks::all(dry.library, SpaceLinks::Role::Followed).isEmpty());

        Receiver receiver;
        const auto report = Following::subscribe(receiver.library, link, hub.link, {});
        QVERIFY2(report, qPrintable(whyNot(report)));
        QCOMPARE(report->space.space, owner.space);
        QVERIFY(SubscribedSpace::isSubscribed(receiver.library, owner.space));
        QVERIFY(receiver.hasBook("Picked by Hand"));
        const auto followed = SpaceLinks::of(receiver.library, owner.space, SpaceLinks::Role::Followed);
        QVERIFY(followed && followed->address == link && followed->transport == "github");

        hub.github.gists[hub.onlyGist()].truncated = true;
        Receiver whole;
        const auto large = Following::subscribe(whole.library, link, hub.link, {});
        QVERIFY2(large, qPrintable(whyNot(large)));
        QVERIFY(hub.github.count("/gistraw/") == 1 && whole.hasBook("Picked by Hand"));

        // Unsubscribing forgets where it was followed from.
        QVERIFY(Subscriptions::unsubscribe(receiver.library, "Shelf"));
        QVERIFY(SpaceLinks::all(receiver.library, SpaceLinks::Role::Followed).isEmpty());
    }

    void aCollectionIsImportedByARepositoryFilesLink() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto snapshot = PublishedSpace::snapshot(owner.library, owner.shelf, {}, ada());
        QVERIFY2(snapshot, qPrintable(whyNot(snapshot)));
        hub.github.files["someone/shelf"].insert("shared/Shelf.omabook-space", SpaceSnapshotFile::write(*snapshot));

        Receiver receiver;
        const auto report =
            Following::subscribe(receiver.library, "https://github.com/someone/shelf/blob/main/shared/Shelf.omabook-space", hub.link, {});
        QVERIFY2(report, qPrintable(whyNot(report)));
        QVERIFY(receiver.hasBook("Dune Messiah"));
        QCOMPARE(hub.github.count("/repos/someone/shelf/contents/shared/Shelf.omabook-space?ref=main"), 1);
        Receiver raw;
        const auto rawReport = Following::subscribe(
            raw.library, "https://raw.githubusercontent.com/someone/shelf/main/shared/Shelf.omabook-space", hub.link, {});
        QVERIFY2(rawReport, qPrintable(whyNot(rawReport)));
        QVERIFY(SpaceLinks::of(raw.library, snapshot->space, SpaceLinks::Role::Followed).has_value());

        Receiver missing;
        const auto none =
            Following::subscribe(missing.library, "https://github.com/someone/shelf/blob/main/Other.omabook-space", hub.link, {});
        QVERIFY(!none && none.error().contains("There is no Other.omabook-space in someone/shelf"));
    }

    // A newer version is offered, with what it changes, and nothing taken in;
    // asked to, only the operations after those already held are. Then
    // there is nothing new.
    void followingOffersANewerVersionAndTakesItOnlyWhenAsked() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto shared = owner.share(hub.link);
        QVERIFY2(shared, qPrintable(whyNot(shared)));
        const auto first = SpaceSnapshotFile::read(hub.github.gists.value(hub.onlyGist()).files.value(GitHubLink::fileName));
        Receiver receiver;
        QVERIFY(Following::subscribe(receiver.library, shared->value("address").toString(), hub.link, {}));
        const qsizetype held = receiver.held(owner.space);

        owner.changeSomething();
        QVERIFY(owner.share(hub.link));
        const auto second = SpaceSnapshotFile::read(hub.github.gists.value(hub.onlyGist()).files.value(GitHubLink::fileName));
        QVERIFY(first && second);
        // Its operations kept their numbers: what is new is what comes after.
        QHash<QString, qint64> had;
        for (const SpaceOperation &op : first->operations)
            had[op.device] = std::max(had.value(op.device), op.seq);
        const qsizetype after =
            std::ranges::count_if(second->operations, [&had](const SpaceOperation &op) { return op.seq > had.value(op.device); });
        QVERIFY(after > 0 && after < second->operations.size());

        const auto offered = Following::check(receiver.library, {}, {&hub.link}, false);
        QVERIFY2(offered && offered->size() == 1, qPrintable(whyNot(offered)));
        const Following::Offer &offer = offered->first();
        QVERIFY2(offer.problem.isEmpty(), qPrintable(offer.problem));
        QCOMPARE(offer.operations, after);
        QCOMPARE(offer.changes.books, (SpaceDiff::Count{.added = 1, .changed = 1, .removed = 0}));
        QVERIFY(!offer.taken.has_value());
        QCOMPARE(receiver.held(owner.space), held);
        QVERIFY(!receiver.hasBook("Later Still"));
        const QString said = Following::describe(*offered).join('\n');
        QVERIFY2(said.contains("a newer version") && said.contains("books: 1 new, 1 changed") && said.contains("--yes"), qPrintable(said));
        QCOMPARE(Following::toJson(*offered).value("followed").toArray().first().toObject().value("taken").toBool(), false);

        const auto taken = Following::check(receiver.library, "Shelf", {&hub.link}, true);
        QVERIFY2(taken && taken->first().taken.has_value(), qPrintable(whyNot(taken)));
        QCOMPARE(receiver.held(owner.space), held + after);
        QVERIFY(receiver.hasBook("Later Still"));

        const auto again = Following::check(receiver.library, {}, {&hub.link}, true);
        QVERIFY2(again && again->first().operations == 0 && !again->first().taken, qPrintable(whyNot(again)));
        QVERIFY(Following::describe(*again).join('\n').contains("nothing new"));
    }

    void nothingNewIsSaidSo() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto shared = owner.share(hub.link);
        QVERIFY2(shared, qPrintable(whyNot(shared)));
        Receiver receiver;
        QVERIFY(Following::subscribe(receiver.library, shared->value("address").toString(), hub.link, {}));
        const auto offered = Following::check(receiver.library, "Shelf", {&hub.link}, false);
        QVERIFY2(offered && offered->first().operations == 0 && offered->first().changes.any() == 0, qPrintable(whyNot(offered)));
        QVERIFY(Following::describe(*offered).first().endsWith("nothing new."));
        QCOMPARE(Following::toJson(*offered).value("followed").toArray().first().toObject().value("new").toBool(), false);
    }

    // A source that can't be reached, or is gone, is said, and nothing changes.
    void aSourceThatCantBeReachedIsSaidClearly() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto shared = owner.share(hub.link);
        QVERIFY2(shared, qPrintable(whyNot(shared)));
        const QString link = shared->value("address").toString();
        Receiver receiver;
        QVERIFY(Following::subscribe(receiver.library, link, hub.link, {}));
        const qsizetype held = receiver.held(owner.space);

        GitHubLink away(FAKE_GH, at(nowhere()));
        const auto offered = Following::check(receiver.library, {}, {&away}, true);
        QVERIFY2(offered && offered->first().problem.contains("GitHub couldn't be reached"), qPrintable(whyNot(offered)));
        QVERIFY(Following::describe(*offered).first().contains("not checked"));
        QCOMPARE(receiver.held(owner.space), held);
        Receiver other;
        const auto imported = Following::subscribe(other.library, link, away, {});
        QVERIFY(!imported && imported.error().contains("GitHub couldn't be reached"));

        hub.github.gists.clear();
        const auto gone = Following::check(receiver.library, {}, {&hub.link}, true);
        QVERIFY2(gone && gone->first().problem.contains("There is no gist"), qPrintable(whyNot(gone)));
        QCOMPARE(receiver.held(owner.space), held);
    }

    // A file imported is followed from nowhere.
    void aCollectionImportedFromAFileHasNoLinkToFollow() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        Receiver receiver;
        const auto none = Following::check(receiver.library, {}, {&hub.link}, false);
        QVERIFY(!none && none.error().contains("No shared collection here is followed"));
        const auto snapshot = PublishedSpace::snapshot(owner.library, owner.shelf, {}, ada());
        QVERIFY(snapshot && Subscriptions::subscribe(receiver.library, *snapshot, {}));
        const auto fromFile = Following::check(receiver.library, "Shelf", {&hub.link}, false);
        QVERIFY(!fromFile && fromFile.error().contains("imported from a file"));
    }

    // Without gh, or with one nobody signed in to, or no GitHub to reach,
    // nothing is shared and it says what to do.
    void withoutAGhThatCanSendNothingIsShared() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        GitHubLink none("/nonexistent/omabook-tests/gh", at(hub.github.api()));
        const auto missing = owner.share(none);
        QVERIFY(!missing && missing.error().contains("gh") && missing.error().contains("cli.github.com"));

        qputenv("FAKE_GH_SIGNED_OUT", "1");
        const auto signedOut = owner.share(hub.link);
        qunsetenv("FAKE_GH_SIGNED_OUT");
        QVERIFY(!signedOut && signedOut.error().contains("gh auth login"));

        qputenv("OMABOOK_GITHUB_API", nowhere().toUtf8());
        const auto offline = owner.share(hub.link);
        qputenv("OMABOOK_GITHUB_API", hub.github.api().toUtf8());
        QVERIFY2(!offline && offline.error().contains("GitHub couldn't be reached"), qPrintable(whyNot(offline)));

        QVERIFY(hub.github.gists.isEmpty());
        QVERIFY(!SpaceLinks::of(owner.library, owner.space, SpaceLinks::Role::Sent).has_value());
    }

    // The commands and the MCP tool, through the real binary: shared to a
    // gist, imported by its link, and followed.
    void theCommandsShareByGistImportAndFollow() {
        Hub hub;
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const QString root = owner.library.root();
        const QString receiver = owner.dir.filePath("Receiver");
        const QProcessEnvironment ada = environmentOf(owner.dir.filePath("ada-data"), hub);
        const QProcessEnvironment bo = environmentOf(owner.dir.filePath("bo-data"), hub);

        const Ran wrong = omabook({"collections", "share", "Shelf", "--as", "Ada", "--public", "--library", root}, ada);
        QVERIFY2(wrong.code == 1 && wrong.errors.contains("goes with --gist"), qPrintable(wrong.errors));
        const Ran shared = omabook({"collections", "share", "Shelf", "--as", "Ada", "--gist", "--library", root}, ada);
        QVERIFY2(shared.code == 0 && shared.output.contains("Sent to a secret gist"), qPrintable(shared.output + shared.errors));
        const QString link = "https://gist.github.com/ada/" + hub.onlyGist();
        QVERIFY2(shared.output.contains(link), qPrintable(shared.output));

        const Ran imported = omabook({"collections", "import", link, "--library", receiver}, bo);
        QVERIFY2(imported.code == 0 && imported.output.contains("Subscribed to Shelf") && imported.output.contains("followed from"),
                 qPrintable(imported.output + imported.errors));
        const Ran nothing = omabook({"collections", "import", "--follow", "--library", receiver}, bo);
        QVERIFY2(nothing.code == 0 && nothing.output.contains("nothing new"), qPrintable(nothing.output + nothing.errors));

        owner.changeSomething();
        const Ran again = omabook({"collections", "share", "Shelf", "--gist", "--library", root}, ada);
        QVERIFY2(again.code == 0 && again.output.contains("Brought the secret gist up to date"), qPrintable(again.output + again.errors));
        QCOMPARE(hub.github.gists.size(), 1);

        const Ran offered = omabook({"collections", "import", "--follow", "--json", "--library", receiver}, bo);
        QVERIFY2(offered.code == 0, qPrintable(offered.errors));
        const QJsonObject offer = QJsonDocument::fromJson(offered.output.toUtf8()).object().value("followed").toArray().first().toObject();
        QVERIFY2(offer.value("new").toBool() && !offer.value("taken").toBool(), qPrintable(offered.output));
        QCOMPARE(offer.value("changes").toObject().value("books").toObject().value("added").toInt(), 1);

        const QByteArray call =
            QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                      {"id", 1},
                                      {"method", "tools/call"},
                                      {"params", QJsonObject{{"name", "collection_import"}, {"arguments", QJsonObject{{"follow", true}}}}}})
                .toJson(QJsonDocument::Compact)
            + "\n";
        const Ran tool = omabook({"mcp", "--library", receiver}, bo, call);
        const QJsonObject result = QJsonDocument::fromJson(tool.output.section('\n', 0, 0).toUtf8()).object().value("result").toObject();
        QVERIFY2(!result.value("isError").toBool(), qPrintable(tool.output + tool.errors));
        QVERIFY2(result.value("structuredContent").toObject().value("followed").toArray().first().toObject().value("new").toBool(),
                 qPrintable(tool.output));

        const Ran taken = omabook({"collections", "import", "--follow", "Shelf", "--yes", "--library", receiver}, bo);
        QVERIFY2(taken.code == 0 && taken.output.contains("taken in"), qPrintable(taken.output + taken.errors));
        Library library(receiver);
        QVERIFY(library.open());
        const QList<Book> books = library.books();
        QVERIFY(std::ranges::any_of(books, [](const Book &book) { return book.meta.title == "Later Still"; }));
    }
};

QTEST_MAIN(LinkingTests)
#include "linking_tests.moc"
