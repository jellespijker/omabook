// Importing a shared collection is subscribing to its published space (ADR
// 0001, *Spaces*; #4): the space is kept read-only as a layer beside the
// receiver's library, never merged into their own rows; its books are
// matched to theirs or become wishlist entries in a collection of their own;
// a newer snapshot brings only what is new, and unsubscribing drops it.

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>

#include "changelog.h"
#include "compaction.h"
#include "fixtures.h"
#include "graph.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "replication.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"
#include "spacesnapshot.h"
#include "subscribedspace.h"
#include "subscriptions.h"

namespace {

[[nodiscard]] QString sample() { return QFINDTESTDATA("data/space-1.0.omabook-space"); }

// Why an answer is none, for a check's message: only asked of one that is.
template <typename Answer> [[nodiscard]] QString whyNot(const Answer &answer) { return answer ? QString() : answer.error(); }

[[nodiscard]] SpaceSnapshot sampleSnapshot() {
    const auto snapshot = Subscriptions::fetch(sample());
    if (!snapshot)
        qFatal("%s", qPrintable(snapshot.error()));
    return *snapshot;
}

[[nodiscard]] qint64 count(const Library &library, const QString &sql, const QVariantList &values = {}) {
    QSqlQuery query = ChangeLog::query(library, sql, values);
    return query.next() ? query.value(0).toLongLong() : -1;
}

// What the database holds, table by table: what a dry run must leave as it is.
[[nodiscard]] QList<qint64> everything(const Library &library) {
    QList<qint64> counts;
    for (const char *table : {"ops", "snapshot_ops", "seen", "books", "collections", "collection_books", "spaces", "space_members",
                              "global_ids", "log_ids", "graph_nodes", "graph_facts", "graph_edges"})
        counts << count(library, QString("SELECT COUNT(*) FROM %1").arg(table));
    return counts;
}

[[nodiscard]] qint64 heldOf(const Library &library, const QString &space) {
    return count(library,
                 "SELECT (SELECT COUNT(*) FROM ops WHERE space = l.id) + (SELECT COUNT(*) FROM snapshot_ops WHERE space = l.id) "
                 "FROM log_ids AS l WHERE l.name = ?",
                 {space});
}

[[nodiscard]] const Subscriptions::Placed *bookTitled(const Subscriptions::Report &report, const QString &title) {
    const auto found = std::ranges::find_if(report.books, [&title](const Subscriptions::Placed &book) { return book.title == title; });
    return found != report.books.cend() ? &*found : nullptr;
}

[[nodiscard]] SpaceSnapshot::Owner ada() {
    static const auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return {.name = "Ada Fixture", .person = person->id()};
}

// Someone who shares a collection: "Shelf", with a series inside it by its
// rule, and a book of their own put in by hand.
struct Owner {
    Owner() : library(dir.filePath("Owner")) {
        QVERIFY(library.open());
        QVERIFY(Fixtures::epub(dir.filePath("Picked.epub"), "Picked by Hand"));
        dune = book("Dune", "Saga", 1, "9780441172719");
        messiah = book("Dune Messiah", "Saga", 2, "9780593098233");
        picked = library.add(dir.filePath("Picked.epub")).id;
        unseen = book("Unseen Elsewhere", {}, 0, "9780000000026");
        QVERIFY(dune > 0 && messiah > 0 && picked > 0 && unseen > 0);
        const auto made = library.addCollection("Shelf");
        QVERIFY(made.has_value());
        shelf = *made;
        QVERIFY(library.addCollection("Saga", "series:Saga", shelf).has_value());
        QVERIFY(library.putInCollection(shelf, picked, true));
        QVERIFY(library.putInCollection(shelf, unseen, true));
        Graph graph(library);
        const qint64 paul = graph.upsert({}, "character", "Paul Atreides", "The duke's son.", {}, GraphPlace{dune, 1, 0.2}, "Paul");
        const qint64 jessica = graph.upsert({}, "character", "Jessica", "His mother.", {}, GraphPlace{dune, 1, 0.3}, "Jessica");
        QVERIFY(paul > 0 && jessica > 0 && graph.link(paul, jessica, "son of", {}, GraphPlace{dune, 1, 0.3}) > 0);
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

    [[nodiscard]] SpaceSnapshot snapshot() const {
        const auto taken = PublishedSpace::snapshot(library, shelf, {.graph = true}, ada());
        if (!taken)
            qFatal("%s", qPrintable(taken.error()));
        return *taken;
    }

    QTemporaryDir dir;
    Library library;
    qint64 dune = 0, messiah = 0, picked = 0, unseen = 0, shelf = 0;
};

struct Receiver {
    Receiver() : library(dir.filePath("Receiver")) { QVERIFY(library.open()); }

    qint64 entry(const QString &title, const QStringList &authors, const QString &isbn = {}) {
        BookMeta meta;
        meta.title = title;
        meta.authors = authors;
        meta.isbn = isbn;
        return library.addEntry(meta, false).id;
    }

    QTemporaryDir dir;
    Library library;
};

[[nodiscard]] Subscriptions::Report subscribed(Library &library, const SpaceSnapshot &snapshot,
                                               const Subscriptions::Options &options = {}) {
    const auto report = Subscriptions::subscribe(library, snapshot, options);
    if (!report)
        qFatal("%s", qPrintable(report.error()));
    return *report;
}

} // namespace

class SubscribingTests : public QObject {
    Q_OBJECT

private slots:
    // The sample file into a library with nothing in it: the space kept as
    // a layer, its books wishlist entries in a collection of the space's
    // name, each in the collection inside it that holds it there.
    void theSampleIsSubscribedToInAFreshLibrary() {
        Receiver receiver;
        const SpaceSnapshot snapshot = sampleSnapshot();
        const Subscriptions::Report report = subscribed(receiver.library, snapshot);
        QCOMPARE(report.operations.added, snapshot.operations.size());
        QCOMPARE(report.operations.known, qsizetype(0));
        QVERIFY(SubscribedSpace::isSubscribed(receiver.library, snapshot.space));
        QCOMPARE(heldOf(receiver.library, snapshot.space), qint64(snapshot.operations.size()));
        QCOMPARE(SpaceReducer::reduce(SubscribedSpace::operations(receiver.library, snapshot.space)).books.size(), 3);

        QCOMPARE(report.collection, QString("Sci-fi"));
        QVERIFY(report.newCollection);
        const auto top = receiver.library.collectionNamed("Sci-fi");
        const auto saga = receiver.library.collectionNamed("Sci-fi/Dune saga");
        QVERIFY(top && saga);
        QCOMPARE(saga->rule, QString()); // the owner's rule chose among their books, not these
        QCOMPARE(SubscribedSpace::linkedCollection(receiver.library, snapshot.space), top->id);
        const QList<Book> books = receiver.library.books();
        QCOMPARE(books.size(), 3);
        QVERIFY(std::ranges::all_of(books, [](const Book &book) { return book.held() == "none"; }));
        QStringList inSaga;
        for (const Book &book : saga->of(books))
            inSaga << book.meta.title;
        inSaga.sort();
        QCOMPARE(inSaga, (QStringList{"Dune", "Dune Messiah"}));
        QCOMPARE(top->of(books).size(), 3);
        const Subscriptions::Placed *picked = bookTitled(report, "Picked by Hand");
        QVERIFY(picked && picked->how == "wishlist" && picked->collections == QStringList{"Sci-fi"});
        const auto dune = receiver.library.book(bookTitled(report, "Dune")->book);
        QVERIFY(dune && dune->meta.isbn == "9780441172719" && dune->identifiers.value("goodreads").toString() == "44767458");
        QVERIFY(report.things > 0 && report.facts > 0);
        QCOMPARE(Subscriptions::toJson(report).value("wishlist").toInteger(), qint64(3));
        QCOMPARE(Subscriptions::toJson(report).value("graph").toObject().value("shown").toBool(), true);
    }

    void booksAreMatchedByFileIdentifierThenTitleAndAuthors() {
        const Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        Receiver receiver;
        const qint64 dune = receiver.entry("Dune", {"Someone Else Entirely"}, "978-0-441-17271-9");
        const qint64 messiah = receiver.entry("Dune Messiah", {"Writer, Bo"});
        const qint64 picked = receiver.library.add(owner.dir.filePath("Picked.epub")).id;
        QVERIFY(dune > 0 && messiah > 0 && picked > 0);

        const Subscriptions::Report report = subscribed(receiver.library, owner.snapshot());
        QCOMPARE(bookTitled(report, "Dune")->book, dune);
        QCOMPARE(bookTitled(report, "Dune")->how, QString("an identifier"));
        QCOMPARE(bookTitled(report, "Dune Messiah")->book, messiah);
        QCOMPARE(bookTitled(report, "Dune Messiah")->how, QString("title and authors"));
        QCOMPARE(bookTitled(report, "Picked by Hand")->book, picked);
        QCOMPARE(bookTitled(report, "Picked by Hand")->how, QString("the same file"));
        QCOMPARE(bookTitled(report, "Unseen Elsewhere")->how, QString("wishlist"));
        QCOMPARE(receiver.library.books().size(), 4); // one wishlist entry, the rest the receiver's own
        // The receiver's own details stay theirs.
        QCOMPARE(receiver.library.book(dune)->meta.authors, QStringList{"Someone Else Entirely"});
        const auto saga = receiver.library.collectionNamed("Shelf/Saga");
        QVERIFY(saga && saga->included.contains(dune) && saga->included.contains(messiah));
        QVERIFY(receiver.library.collectionNamed("Shelf")->included.contains(picked));

        // Under another name, an existing collection is merged into, never
        // emptied: what the receiver had in it stays.
        Receiver other;
        const qint64 mine = other.entry("Mine Already", {"Me"});
        const auto reading = other.library.addCollection("Reading");
        QVERIFY(reading && other.library.putInCollection(*reading, mine, true));
        const Subscriptions::Report into = subscribed(other.library, owner.snapshot(), {.as = "Reading", .dryRun = false});
        QCOMPARE(into.collection, QString("Reading"));
        QVERIFY(!into.newCollection);
        const auto merged = other.library.collection(*reading);
        QVERIFY(merged->included.contains(mine));
        QCOMPARE(merged->of(other.library.books()).size(), 5);
    }

    // The same file twice changes nothing; a book the receiver took out of
    // the collection, or removed, isn't put back: only what a snapshot newly
    // brings is filed.
    void importingTheSameFileTwiceChangesNothing() {
        Receiver receiver;
        const SpaceSnapshot snapshot = sampleSnapshot();
        const Subscriptions::Report first = subscribed(receiver.library, snapshot);
        const auto top = receiver.library.collectionNamed("Sci-fi");
        const qint64 picked = bookTitled(first, "Picked by Hand")->book;
        QVERIFY(receiver.library.putInCollection(top->id, picked, false));
        const qint64 dune = bookTitled(first, "Dune")->book;
        QVERIFY(receiver.library.remove(dune).has_value());
        const QList<qint64> before = everything(receiver.library);

        const Subscriptions::Report again = subscribed(receiver.library, snapshot);
        QCOMPARE(again.operations.added, qsizetype(0));
        QCOMPARE(again.operations.known, snapshot.operations.size());
        QVERIFY(!again.newCollection);
        QCOMPARE(bookTitled(again, "Dune")->how, QString("removed here before"));
        QCOMPARE(everything(receiver.library), before);
        QVERIFY(!receiver.library.collection(top->id)->contains(*receiver.library.book(picked)));

        // After compaction too: what it took out of `ops` is still known.
        Compaction::compact(receiver.library);
        const QList<qint64> compacted = everything(receiver.library);
        QCOMPARE(subscribed(receiver.library, snapshot).operations.added, qsizetype(0));
        QCOMPARE(everything(receiver.library), compacted);
    }

    // A later snapshot of the same space, numbered anew, brings only the
    // operations the subscriber lacks, into the same collection.
    void aNewerSnapshotBringsOnlyWhatIsNew() {
        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        Receiver receiver;
        const SpaceSnapshot first = owner.snapshot();
        std::ignore = subscribed(receiver.library, first, {.as = "From Ada", .dryRun = false}); // the second import is the point
        const qint64 later = owner.book("Later Still", {}, 0, "9780000000033");
        QVERIFY(later > 0 && owner.library.putInCollection(owner.shelf, later, true));
        BookMeta renamed = owner.library.book(owner.messiah)->meta;
        renamed.title = "Dune Messiah (Revised)";
        QVERIFY(owner.library.setMeta(owner.messiah, renamed));
        const SpaceSnapshot second = owner.snapshot();
        QCOMPARE(second.space, first.space);

        QSet<QString> had;
        const auto same = [](const SpaceOperation &op) {
            return QStringList{op.device, QString::number(op.hlc), op.entity, op.id, op.field, op.value}.join('|');
        };
        for (const SpaceOperation &op : first.operations)
            had.insert(same(op));
        const qsizetype fresh = std::ranges::count_if(second.operations, [&](const SpaceOperation &op) { return !had.contains(same(op)); });
        QVERIFY(fresh > 0 && fresh < second.operations.size());

        const Subscriptions::Report report = subscribed(receiver.library, second);
        QCOMPARE(report.operations.added, fresh);
        QCOMPARE(report.collection, QString("From Ada")); // the collection linked to the space, without --as again
        QCOMPARE(heldOf(receiver.library, second.space), qint64(first.operations.size() + fresh));
        QStringList layer, owners;
        for (const ReducedBook &book : SpaceReducer::reduce(SubscribedSpace::operations(receiver.library, second.space)).books)
            layer << book.fields.value("title").value;
        for (const ReducedBook &book : SpaceReducer::reduce(second.operations).books)
            owners << book.fields.value("title").value;
        layer.sort();
        owners.sort();
        QCOMPARE(layer, owners);
        QCOMPARE(bookTitled(report, "Later Still")->how, QString("wishlist"));
        QVERIFY(receiver.library.collectionNamed("From Ada")->of(receiver.library.books()).size() == 5);
    }

    void unsubscribingDropsTheLayerAndKeepsTheCollection() {
        Receiver receiver;
        const SpaceSnapshot snapshot = sampleSnapshot();
        std::ignore = subscribed(receiver.library, snapshot); // unsubscribing is the point
        Compaction::compact(receiver.library);                // so the snapshot holds some of the space too
        const qint64 personal = count(receiver.library, "SELECT COUNT(*) FROM live_ops");

        const auto dropped = Subscriptions::unsubscribe(receiver.library, "Sci-fi");
        QVERIFY2(dropped.has_value(), qPrintable(whyNot(dropped)));
        QCOMPARE(dropped->collection, QString("Sci-fi"));
        QVERIFY(dropped->operations > 0);
        QVERIFY(!SubscribedSpace::isSubscribed(receiver.library, snapshot.space));
        QCOMPARE(heldOf(receiver.library, snapshot.space), qint64(0));
        QCOMPARE(count(receiver.library, "SELECT COUNT(*) FROM spaces WHERE id = ?", {snapshot.space}), qint64(0));
        QCOMPARE(count(receiver.library, "SELECT COUNT(*) FROM live_ops"), personal);
        // The collection and its books are the receiver's own, and stay.
        QCOMPARE(receiver.library.collectionNamed("Sci-fi")->of(receiver.library.books()).size(), 3);
        QVERIFY(SubscribedSpace::list(receiver.library).isEmpty());
        QVERIFY(!Subscriptions::unsubscribe(receiver.library, "Sci-fi").has_value());

        // Subscribing again is subscribing anew, into the same collection.
        const Subscriptions::Report again = subscribed(receiver.library, snapshot);
        QCOMPARE(again.operations.added, snapshot.operations.size());
        QVERIFY(!again.newCollection);
        QCOMPARE(receiver.library.books().size(), 3);
    }

    // The space's graph, and its books, never become the receiver's rows:
    // not when it is taken in, nor when the rows are made again from the
    // log, nor on the receiver's other computers.
    void theReceiversOwnGraphIsUntouched() {
        Receiver receiver;
        const qint64 dune = receiver.entry("Dune", {"Ann Author"}, "9780441172719");
        Graph graph(receiver.library);
        const qint64 paul = graph.upsert({}, "character", "Paul Atreides", "Mine, as I read it.", {}, GraphPlace{dune, 1, 0.1}, "Paul");
        QVERIFY(paul > 0);
        const auto rows = [&receiver] {
            return QList<qint64>{
                count(receiver.library, "SELECT COUNT(*) FROM graph_nodes"), count(receiver.library, "SELECT COUNT(*) FROM graph_facts"),
                count(receiver.library, "SELECT COUNT(*) FROM graph_edges"), count(receiver.library, "SELECT COUNT(*) FROM graph_seen")};
        };
        const QList<qint64> before = rows();
        const qint64 ownOps = count(receiver.library, "SELECT COUNT(*) FROM live_ops WHERE entity IN ('node', 'fact', 'edge')");

        const SpaceSnapshot snapshot = sampleSnapshot();
        QVERIFY(subscribed(receiver.library, snapshot).things > 0);
        QCOMPARE(rows(), before);
        QCOMPARE(count(receiver.library, "SELECT COUNT(*) FROM live_ops WHERE entity IN ('node', 'fact', 'edge')"), ownOps);
        QVERIFY(SpaceReplay::replay(receiver.library));
        QCOMPARE(rows(), before);
        QCOMPARE(Graph(receiver.library).node(paul)->summary, QString("Mine, as I read it."));
        QCOMPARE(count(receiver.library, "SELECT COUNT(*) FROM graph_nodes WHERE name = 'Jessica'"), qint64(0));
        QCOMPARE(receiver.library.books().size(), 3);
        QVERIFY(std::ranges::none_of(Replication::operations(receiver.library),
                                     [&snapshot](const SpaceOperation &op) { return op.space == snapshot.space; }));
    }

    // A space that says a book the receiver also has (the very same file, so
    // the same id) is called otherwise, or was removed, changes nothing of
    // the receiver's own book: not when it is taken in, nor when the rows
    // are made again from the log, nor when the library is opened again.
    void aSharedSpaceNeverChangesTheReceiversOwnBooks() {
        const Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        Receiver receiver;
        const qint64 mine = receiver.library.add(owner.dir.filePath("Picked.epub")).id;
        QVERIFY(mine > 0);
        const QString gid = Spaces::bookId(receiver.library, mine);
        const BookMeta before = receiver.library.book(mine)->meta;
        SpaceSnapshot snapshot = owner.snapshot();
        QVERIFY(std::ranges::any_of(snapshot.operations, [&gid](const SpaceOperation &op) { return op.id == gid; }));
        const qint64 later = snapshot.clock + 1000;
        for (const auto &[field, value] : {std::pair{"title", "Called Otherwise There"}, {"removed", "gone there"}})
            snapshot.operations << SpaceOperation{.space = snapshot.space,
                                                  .device = "device-001",
                                                  .seq = 10000 + snapshot.operations.size(),
                                                  .hlc = later,
                                                  .author = snapshot.owner.person,
                                                  .entity = "book",
                                                  .id = gid,
                                                  .field = field,
                                                  .value = value};
        snapshot.clock = later;

        const Subscriptions::Report report = subscribed(receiver.library, snapshot);
        QCOMPARE(bookTitled(report, "Picked by Hand"), nullptr); // removed in the space, so not of it
        const auto kept = [&receiver, mine, &before] {
            const auto book = receiver.library.book(mine);
            return book && book->meta.title == before.title
                   && !receiver.library.wasRemoved(receiver.library.formatFiles(mine).value(0).hash);
        };
        QVERIFY(kept());
        QVERIFY(SpaceReplay::replay(receiver.library));
        QVERIFY(kept());
        ChangeLog::reconcile(receiver.library);
        Library again(receiver.library.root());
        QVERIFY(again.open());
        QCOMPARE(again.book(mine)->meta.title, before.title);
        QVERIFY(std::ranges::none_of(SpaceReplay::operations(again),
                                     [&snapshot](const SpaceOperation &op) { return op.space == snapshot.space; }));
    }

    void aDryRunWritesNothing() {
        Receiver receiver;
        std::ignore = receiver.entry("Dune Messiah", {"Bo Writer"}); // matched, so the dry run has something to find
        const QList<qint64> before = everything(receiver.library);
        const SpaceSnapshot snapshot = sampleSnapshot();
        const Subscriptions::Report report = subscribed(receiver.library, snapshot, {.as = {}, .dryRun = true});
        QCOMPARE(everything(receiver.library), before);
        QVERIFY(report.dryRun && report.newCollection);
        QCOMPARE(report.operations.added, snapshot.operations.size());
        QCOMPARE(bookTitled(report, "Dune Messiah")->how, QString("title and authors"));
        QCOMPARE(bookTitled(report, "Dune")->how, QString("wishlist"));
        QVERIFY(!SubscribedSpace::isSubscribed(receiver.library, snapshot.space));
        QVERIFY(Subscriptions::describe(report).last().contains("Nothing was written"));
    }

    // A newer format, a link (#7), an operation not the owner's, and a space
    // the library publishes itself are refused, and nothing is written.
    void whatCantBeSubscribedToIsRefused() {
        QFile file(sample());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject newer = QJsonDocument::fromJson(file.readAll()).object();
        newer.insert("version", "2.0");
        QTemporaryDir dir;
        const QString path = dir.filePath("newer.omabook-space");
        QVERIFY(Fixtures::write(path, QJsonDocument(newer).toJson()));
        const auto refused = Subscriptions::fetch(path);
        QVERIFY(!refused.has_value() && refused.error().contains("newer Omabook"));
        QVERIFY(!Subscriptions::fetch("https://gist.github.com/someone/abc").has_value());
        QVERIFY(!Subscriptions::fetch(dir.filePath("none.omabook-space")).has_value());

        Receiver receiver;
        const QList<qint64> before = everything(receiver.library);
        SpaceSnapshot tampered = sampleSnapshot();
        tampered.operations[3].author = "someone-else";
        const auto notTheOwners = Subscriptions::subscribe(receiver.library, tampered, {});
        QVERIFY(!notTheOwners.has_value() && notTheOwners.error().contains("not its owner's"));
        QCOMPARE(everything(receiver.library), before);

        Owner owner;
        QVERIFY(!QTest::currentTestFailed());
        const auto ownSpace = Subscriptions::subscribe(owner.library, owner.snapshot(), {});
        QVERIFY(!ownSpace.has_value() && ownSpace.error().contains("publishes itself"));
    }

    // The command line and what an agent calls: a dry run, the import and
    // the unsubscribing, as JSON and as text.
    void theCommandImportsAndUnsubscribes() {
        QTemporaryDir dir;
        const QString library = dir.filePath("Library");
        const auto run = [&library](QStringList arguments) {
            QProcess process;
            process.start(OMABOOK_BINARY, arguments << "--library" << library);
            process.closeWriteChannel();
            process.waitForFinished(60000);
            return std::pair{process.exitCode(), QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
        };
        const auto [dryCode, dry] = run({"collections", "import", sample(), "--dry-run", "--json"});
        QCOMPARE(dryCode, 0);
        const QJsonObject planned = QJsonDocument::fromJson(dry.toUtf8()).object();
        QVERIFY2(planned.value("dry_run").toBool() && planned.value("wishlist").toInteger() == 3, qPrintable(dry));
        const auto [code, text] = run({"collections", "import", sample(), "--as", "From a Friend"});
        QVERIFY2(code == 0 && text.contains("Subscribed to Sci-fi") && text.contains("From a Friend"), qPrintable(text));
        const auto [listCode, listed] = run({"collections"});
        QVERIFY2(listCode == 0 && listed.contains("From a Friend") && listed.contains("Dune saga"), qPrintable(listed));
        const auto [goneCode, gone] = run({"collections", "unsubscribe", "From a Friend"});
        QVERIFY2(goneCode == 0 && gone.contains("Unsubscribed from Sci-fi"), qPrintable(gone));
        const auto [againCode, again] = run({"collections", "unsubscribe", "Sci-fi"});
        QVERIFY2(againCode == 1, qPrintable(again));
    }
};

QTEST_MAIN(SubscribingTests)
#include "subscribing_tests.moc"
