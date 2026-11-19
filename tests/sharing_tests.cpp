// Sharing a collection as a published space (ADR 0001, *Spaces* and
// *Compaction*; #1, #2): what the space holds and what it leaves out, each
// optional part by its own switch, and its snapshot as a `.omabook-space`
// file, which every later Omabook must still read (the samples in
// tests/data/space-*.omabook-space).

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <optional>

#include "changelog.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcontext.h"
#include "library.h"
#include "person.h"
#include "publishedspace.h"
#include "spacereducer.h"
#include "spaces.h"
#include "spacesnapshot.h"

namespace {

using Parts = PublishedSpace::Parts;

// A small library made on the spot: a collection "Sci-fi" inside one whose
// name tells something of its owner,
// with a series inside it, and books, reading and a graph in and outside it.
struct Shelf {
    Shelf() : library(dir.filePath("Library")) {
        QVERIFY(library.open());
        dune = book("Dune", "Dune", 1, "9780441172719");
        messiah = book("Dune Messiah", "Dune", 2, "9780593098233");
        outside = book("Outside the Shelf", {}, 0, "9780000000002");
        picked = book("Picked by Hand", {}, 0, "9780000000019");
        QVERIFY(library.setIdentifier(dune, "goodreads", "44767458"));
        QVERIFY(library.setIdentifier(dune, "openlibrary", "OL893415W"));

        const auto top = library.addCollection(QString(aboveName) + "/Sci-fi");
        QVERIFY(top.has_value());
        sciFi = *top;
        const auto saga = library.addCollection("Dune saga", "series:Dune", sciFi);
        QVERIFY(saga.has_value());
        series = *saga;
        QVERIFY(library.addCollection(QString(aboveName) + "/" + siblingName).has_value());
        QVERIFY(library.putInCollection(sciFi, picked, true));
        QVERIFY(library.setCollectionShelf(sciFi, {{"view", "grid"}, {"sort", "title"}}));

        QVERIFY(library.setReading(dune, "reading", 4, {}));
        QVERIFY(library.setReading(outside, "read", 2, "2026-10-01"));
        library.setProgress(dune, {.section = 3, .fraction = 0.5, .percent = 40, .updated = 1794730000});

        Graph graph(library);
        paul = graph.upsert({}, "character", "Paul Atreides", "The duke's son.", {}, GraphPlace{dune, 1, 0.2}, "Paul");
        jessica = graph.upsert({}, "character", "Jessica", "His mother.", {}, GraphPlace{dune, 1, 0.3}, "Jessica");
        stranger = graph.upsert({}, "character", "A Stranger", "Met elsewhere.", {}, GraphPlace{outside, 2, 0.1}, "Stranger");
        QVERIFY(paul > 0 && jessica > 0 && stranger > 0);
        QVERIFY(graph.link(paul, jessica, "son of", {}, GraphPlace{dune, 1, 0.3}) > 0);
        graph.logReading(dune, 10, 40, "2026-11-01");
        graph.logReading(outside, 0, 100, "2026-10-01");
        question = GraphContext::noteQuestion(library, dune, GraphPlace{dune, 2, 0.1}, "Who is Paul?", "The duke's son.");
        QVERIFY(question > 0);
    }

    qint64 book(const QString &title, const QString &series, double index, const QString &isbn) {
        const QString file = dir.filePath(title + ".epub");
        if (!Fixtures::epub(file, title))
            return 0;
        const qint64 id = library.add(file).id;
        BookMeta meta = library.book(id)->meta;
        meta.series = series;
        meta.seriesIndex = index;
        meta.isbn = isbn;
        meta.description = "Kept at home: " + title;
        meta.publisher = "Home Press";
        meta.tags = {"mine"};
        return library.setMeta(id, meta) ? id : 0;
    }

    [[nodiscard]] QList<SpaceOperation> published(const Parts &parts) const {
        const auto ops = PublishedSpace::operations(library, sciFi, parts, "published:test");
        if (!ops)
            qFatal("%s", qPrintable(ops.error()));
        return *ops;
    }

    [[nodiscard]] QString gid(qint64 bookId) const { return Spaces::bookId(library, bookId); }
    [[nodiscard]] QString node(qint64 nodeId) const { return Spaces::nodeId(library, nodeId); }

    static constexpr auto aboveName = "Therapy Notes of Ada";
    static constexpr auto siblingName = "Books Hidden From Bob";

    QTemporaryDir dir;
    Library library;
    qint64 dune = 0, messiah = 0, outside = 0, picked = 0;
    qint64 sciFi = 0, series = 0;
    qint64 paul = 0, jessica = 0, stranger = 0, question = 0;
};

[[nodiscard]] QSet<QString> fieldsOf(const QList<SpaceOperation> &ops, const QString &entity, const QString &id = {}) {
    QSet<QString> fields;
    for (const SpaceOperation &op : ops)
        if (op.entity == entity && (id.isEmpty() || op.id == id))
            fields.insert(op.field);
    return fields;
}

[[nodiscard]] QSet<QString> idsOf(const QList<SpaceOperation> &ops, const QString &entity) {
    QSet<QString> ids;
    for (const SpaceOperation &op : ops)
        if (op.entity == entity)
            ids.insert(op.id);
    return ids;
}

[[nodiscard]] QString collectionNamed(const ReducedState &state, const QString &name) {
    for (auto collection = state.collections.cbegin(); collection != state.collections.cend(); ++collection)
        if (collection->fields.value("name").value == name)
            return collection.key();
    return {};
}

[[nodiscard]] SpaceSnapshot::Owner owner();

void expectNothingOfHome(const Shelf &shelf) {
    const auto snapshot =
        PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.graph = true, .questions = true, .ratings = true, .reading = true}, owner());
    QVERIFY2(snapshot.has_value(), qPrintable(snapshot.error()));
    const QByteArray file = SpaceSnapshotFile::write(*snapshot);
    const QStringList home{Shelf::aboveName,
                           Shelf::siblingName,
                           shelf.dir.path(),
                           QDir::homePath(),
                           ".epub",
                           "Library/",
                           Spaces::deviceId(shelf.library),
                           Spaces::personalSpace(shelf.library),
                           "c:" + QString(Shelf::aboveName)};
    for (const QString &word : home)
        QVERIFY2(!file.contains(word.toUtf8()), qPrintable(word));
    for (const SpaceOperation &op : snapshot->operations) {
        QVERIFY2(op.device.startsWith("device-") && op.author == snapshot->owner.person, qPrintable(op.device));
        QVERIFY2(op.entity != "collection" || !op.id.contains('/'), qPrintable(op.id));
    }
}

[[nodiscard]] SpaceSnapshot::Owner owner() {
    const auto person = Person::generate();
    if (!person)
        qFatal("%s", qPrintable(person.error()));
    return {.name = "Ada Fixture", .person = person->id()};
}

// The sample file's text with its version and an extra field or two, as a
// later minor (or major) version might write it.
[[nodiscard]] QByteArray rewritten(const QByteArray &file, const QString &version, bool addFields) {
    QJsonObject object = QJsonDocument::fromJson(file).object();
    object.insert("version", version);
    if (addFields) {
        object.insert("signature", "not known to this Omabook");
        QJsonArray ops = object.value("operations").toArray();
        QJsonArray first = ops.at(0).toArray();
        first.append("a later field of an operation");
        ops.replace(0, first);
        object.insert("operations", ops);
    }
    return QJsonDocument(object).toJson();
}

} // namespace

class SharingTests : public QObject {
    Q_OBJECT

private slots:
    // The collection's tree and its books' identity, and nothing else of
    // the owner's: no reading, no stars, no graph, no file.
    void aPublishedSpaceHoldsTheCollectionAndItsBooksAndNothingPersonal() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const QList<SpaceOperation> ops = shelf.published({});
        QVERIFY(!ops.isEmpty());
        QVERIFY(std::ranges::all_of(ops, [](const SpaceOperation &op) { return op.space == "published:test"; }));

        const ReducedState state = SpaceReducer::reduce(ops);
        QCOMPARE(state.collections.size(), 2);
        const QString topId = collectionNamed(state, "Sci-fi");
        const QString sagaId = collectionNamed(state, "Dune saga");
        const ReducedCollection top = state.collections.value(topId);
        QCOMPARE(top.fields.value("parent").value, QString()); // at the top of the space, not inside what holds it at home
        QCOMPARE(QJsonDocument::fromJson(top.fields.value("shelf").value.toUtf8()).object().value("view").toString(), QString("grid"));
        QCOMPARE(top.books.value(shelf.gid(shelf.picked)).value, QString("in"));
        const ReducedCollection saga = state.collections.value(sagaId);
        QCOMPARE(saga.fields.value("rule").value, QString("series:Dune"));
        QCOMPARE(saga.fields.value("parent").value, topId);

        QCOMPARE(QSet<QString>(state.books.keyBegin(), state.books.keyEnd()),
                 (QSet<QString>{shelf.gid(shelf.dune), shelf.gid(shelf.messiah), shelf.gid(shelf.picked)}));
        const ReducedBook dune = state.books.value(shelf.gid(shelf.dune));
        QCOMPARE(dune.fields.value("title").value, QString("Dune"));
        QCOMPARE(dune.fields.value("series").value, QString("Dune"));
        QCOMPARE(dune.fields.value("isbn").value, QString("9780441172719"));
        QCOMPARE(QJsonDocument::fromJson(dune.fields.value("identifiers").value.toUtf8()).object().value("goodreads").toString(),
                 QString("44767458"));
        QVERIFY(!dune.fields.value("authors").value.isEmpty());
        const QSet<QString> allowed{"title", "authors", "series", "series_index", "isbn", "identifiers"};
        QVERIFY2(fieldsOf(ops, "book").subtract(allowed).isEmpty(), qPrintable(QStringList(fieldsOf(ops, "book").values()).join(", ")));
        QVERIFY(fieldsOf(ops, "collection").subtract({"name", "parent", "rule", "shelf", "book:" + shelf.gid(shelf.picked)}).isEmpty());
        for (const char *entity : {"node", "fact", "edge", "reading_log"})
            QVERIFY2(idsOf(ops, entity).isEmpty(), entity);
        // Not a word of where a book lies or what is in it.
        for (const SpaceOperation &op : ops)
            QVERIFY2(!op.value.contains("Home Press") && !op.value.contains("Kept at home") && !op.value.contains(".epub")
                         && !op.value.contains(shelf.dir.path()),
                     qPrintable(op.field + ": " + op.value));

        // Nothing in the file of what holds the collection at home or sits
        // beside it, of where the library and its files are, or of the
        // owner's computers.
        expectNothingOfHome(shelf);

        // Numbered anew per device in this space, each number once.
        QHash<QString, QList<qint64>> numbers;
        for (const SpaceOperation &op : ops)
            numbers[op.device] << op.seq;
        for (QList<qint64> &seqs : numbers) {
            std::ranges::sort(seqs);
            for (qsizetype at = 0; at < seqs.size(); ++at)
                QCOMPARE(seqs.at(at), at + 1);
        }
    }

    void eachPartGoesOnlyWhenItsSwitchIsOn() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const QString dune = shelf.gid(shelf.dune);

        const QList<SpaceOperation> stars = shelf.published({.ratings = true});
        QCOMPARE(SpaceReducer::reduce(stars).books.value(dune).fields.value("rating").value, QString("4"));
        QVERIFY(!fieldsOf(stars, "book").contains("progress") && !fieldsOf(stars, "book").contains("shelf"));
        QVERIFY(idsOf(stars, "reading_log").isEmpty() && idsOf(stars, "node").isEmpty());

        const QList<SpaceOperation> reading = shelf.published({.reading = true});
        const ReducedState read = SpaceReducer::reduce(reading);
        QCOMPARE(QJsonDocument::fromJson(read.books.value(dune).fields.value("progress").value.toUtf8()).object().value("section").toInt(),
                 3);
        QCOMPARE(read.books.value(dune).fields.value("shelf").value, QString("reading"));
        QVERIFY(!fieldsOf(reading, "book").contains("rating"));
        // The days Dune was read on, not the one outside the collection.
        QVERIFY(!read.days.isEmpty());
        QVERIFY(std::ranges::all_of(read.days, [&](const ReducedDay &day) { return day.bookGid == dune; }));
        QVERIFY(idsOf(reading, "node").isEmpty());
    }

    void theGraphAndTheQuestionsGoEachByItsOwnSwitch() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const QString dune = shelf.gid(shelf.dune);
        const QList<SpaceOperation> graph = shelf.published({.graph = true});
        const QSet<QString> things = idsOf(graph, "node");
        QVERIFY(things.contains(shelf.node(shelf.paul)) && things.contains(shelf.node(shelf.jessica)));
        QVERIFY(!things.contains(shelf.node(shelf.stranger)));
        QVERIFY(!things.contains(shelf.node(shelf.question)));
        QVERIFY(!idsOf(graph, "fact").isEmpty() && !idsOf(graph, "edge").isEmpty());
        for (const SpaceOperation &op : graph)
            QVERIFY2(!op.field.startsWith("seen:") || op.field == "seen:" + dune, qPrintable(op.field));
        QVERIFY(!fieldsOf(graph, "book").contains("rating") && idsOf(graph, "reading_log").isEmpty());

        const QList<SpaceOperation> questions = shelf.published({.questions = true});
        const QSet<QString> asked = idsOf(questions, "node");
        QVERIFY(asked.contains(shelf.node(shelf.question)));
        QVERIFY(!asked.contains(shelf.node(shelf.paul)));
        QVERIFY(!idsOf(questions, "edge").isEmpty()); // what it was asked of

        // All of them together is each of them.
        const QList<SpaceOperation> all = shelf.published({.graph = true, .questions = true, .ratings = true, .reading = true});
        QVERIFY(idsOf(all, "node").contains(shelf.node(shelf.question)) && idsOf(all, "node").contains(shelf.node(shelf.paul)));
        QVERIFY(fieldsOf(all, "book").contains("rating") && fieldsOf(all, "book").contains("progress"));
        // Never what a book's own thing of the graph keeps of the reading, a
        // tag, or the row number here of the book a question was asked of.
        for (const SpaceOperation &op : all) {
            QVERIFY2(op.entity != "node" || op.field != "kind" || op.value != "tag", qPrintable(op.id));
            QVERIFY2(op.entity != "node" || op.field != "data" || (!op.value.contains("rating") && !op.value.contains("\"book\":")),
                     qPrintable(op.value));
        }
        QCOMPARE(Parts::fromNames(Parts{.graph = true, .reading = true}.names()), (Parts{.graph = true, .reading = true}));
        QCOMPARE(Parts::fromNames({"ratings", "a part from a later Omabook"}), Parts{.ratings = true});
    }

    // The snapshot as a file: what it says on top, an operation a line, and
    // read back it is what was written. The space is the owner's, recorded
    // as published.
    void aSnapshotFileReadsBackAsItWasWritten() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const SpaceSnapshot::Owner ada = owner();
        const auto snapshot = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.ratings = true}, ada);
        QVERIFY2(snapshot.has_value(), qPrintable(snapshot.error()));
        QCOMPARE(snapshot->space, PublishedSpace::idFor(ada.person, Spaces::collectionId(shelf.library, shelf.sciFi)));
        QVERIFY(snapshot->space.startsWith("published:"));
        QCOMPARE(snapshot->kind, QString("published"));
        QCOMPARE(snapshot->name, QString("Sci-fi"));
        QCOMPARE(snapshot->owner, ada);
        QCOMPARE(snapshot->parts, QStringList{"ratings"});
        QCOMPARE(snapshot->writtenBy, QString(OMABOOK_VERSION));
        QVERIFY(std::ranges::all_of(snapshot->operations, [&](const SpaceOperation &op) { return op.hlc <= snapshot->clock; }));

        QSqlQuery space = ChangeLog::query(shelf.library,
                                           "SELECT s.kind, s.owner, m.role, m.name FROM spaces AS s JOIN space_members AS m "
                                           "ON m.space = s.id WHERE s.id = ?",
                                           {snapshot->space});
        QVERIFY(space.next());
        QCOMPARE(space.value(0).toString(), QString("published"));
        QCOMPARE(space.value(1).toString(), ada.person);
        QCOMPARE(space.value(2).toString(), QString("writer"));
        QCOMPARE(space.value(3).toString(), ada.name);
        space.finish();

        const QByteArray file = SpaceSnapshotFile::write(*snapshot);
        QVERIFY2(file.startsWith("{\n  \"format\": \"omabook-space\",\n  \"version\": \"1.0\",\n"), file.left(80).constData());
        QCOMPARE(file.count("\n    ["), snapshot->operations.size());
        const auto back = SpaceSnapshotFile::read(file);
        QVERIFY2(back.has_value(), qPrintable(back.error()));
        QCOMPARE(*back, *snapshot);

        const QString path = shelf.dir.filePath(QString("Sci-fi") + SpaceSnapshotFile::suffix);
        QVERIFY(SpaceSnapshotFile::save(*snapshot, path).has_value());
        const auto loaded = SpaceSnapshotFile::load(path);
        QVERIFY(loaded.has_value() && *loaded == *snapshot);
        QVERIFY(!SpaceSnapshotFile::load(shelf.dir.filePath("none.omabook-space")).has_value());
        QVERIFY(!PublishedSpace::snapshot(shelf.library, 999, {}, ada).has_value());
        QVERIFY(!PublishedSpace::snapshot(shelf.library, shelf.sciFi, {}, {.name = "Nobody", .person = {}}).has_value());
    }

    // A later major version is refused, saying so; a later minor one is
    // read, what it adds left aside; what isn't a space file is refused.
    void aNewerMajorVersionIsRefusedAndUnknownFieldsAreIgnored() {
        QFile sample(QFINDTESTDATA("data/space-1.0.omabook-space"));
        QVERIFY(sample.open(QIODevice::ReadOnly));
        const QByteArray file = sample.readAll();
        const auto known = SpaceSnapshotFile::read(file);
        QVERIFY2(known.has_value(), qPrintable(known.error()));

        const auto newer = SpaceSnapshotFile::read(rewritten(file, "2.0", false));
        QVERIFY(!newer.has_value());
        QVERIFY2(newer.error().contains("newer Omabook") && newer.error().contains("2.0") && newer.error().contains("Update Omabook"),
                 qPrintable(newer.error()));
        const auto later = SpaceSnapshotFile::read(rewritten(file, "1.7", true));
        QVERIFY2(later.has_value(), qPrintable(later.error()));
        QCOMPARE(later->operations, known->operations);
        QCOMPARE(later->owner, known->owner);

        for (const QByteArray &broken :
             {QByteArray("not json"), QByteArray("{\"format\": \"something else\", \"version\": \"1.0\"}"), rewritten(file, "", false),
              rewritten(file, "0.9", false), file.left(file.indexOf("\n    [")) + "\n    [\"device\", 1]\n  ]\n}\n"}) {
            const auto refused = SpaceSnapshotFile::read(broken);
            QVERIFY2(!refused.has_value(), broken.left(60).constData());
            QVERIFY(!refused.error().isEmpty());
        }
    }

    // Every sample of the format that a release wrote still reads, as it
    // was written (tests/data/space-<format version>.omabook-space; a release
    // that changes the format adds one, made from fixtures with the test
    // below).
    void everySharedSpaceSampleStillReads() {
        const QDir data(QFileInfo(QFINDTESTDATA("data/space-1.0.omabook-space")).absolutePath());
        const QStringList samples = data.entryList({QString("space-*") + SpaceSnapshotFile::suffix}, QDir::Files, QDir::Name);
        QVERIFY(samples.contains("space-1.0.omabook-space"));
        for (const QString &name : samples) {
            const auto snapshot = SpaceSnapshotFile::load(data.filePath(name));
            QVERIFY2(snapshot.has_value(), qPrintable(name + ": " + snapshot.error()));
            QCOMPARE(snapshot->kind, QString("published"));
            QVERIFY2(snapshot->space.startsWith("published:"), qPrintable(name));
            QVERIFY2(!snapshot->owner.name.isEmpty() && Person::publicKeyFrom(snapshot->owner.person).has_value(), qPrintable(name));
            QVERIFY2(snapshot->clock > 0 && !snapshot->writtenBy.isEmpty(), qPrintable(name));
            QVERIFY(std::ranges::all_of(
                snapshot->operations, [&](const SpaceOperation &op) { return op.space == snapshot->space && op.hlc <= snapshot->clock; }));
            const ReducedState state = SpaceReducer::reduce(snapshot->operations);
            const auto collection = std::ranges::find_if(
                state.collections, [](const ReducedCollection &collection) { return collection.fields.value("name").value == "Sci-fi"; });
            QVERIFY2(collection != state.collections.cend(), qPrintable(name));
            QVERIFY2(collection->fields.value("parent").value.isEmpty(), qPrintable(name));
            QStringList titles;
            for (const ReducedBook &book : state.books)
                titles << book.fields.value("title").value;
            titles.sort();
            QCOMPARE(titles, (QStringList{"Dune", "Dune Messiah", "Picked by Hand"}));
            QCOMPARE(Parts::fromNames(snapshot->parts), (Parts{.graph = true, .questions = true, .ratings = true, .reading = true}));
            QVERIFY2(!state.nodes.isEmpty() && !state.days.isEmpty(), qPrintable(name));
        }
    }

    // Writes the sample of this format from the fixtures above, when asked
    // (OMABOOK_WRITE_SPACE_SAMPLE names the file), for a release that changes
    // the format; never from anyone's own library.
    void theSampleIsMadeFromFixtures() {
        const QString target = qEnvironmentVariable("OMABOOK_WRITE_SPACE_SAMPLE");
        if (target.isEmpty())
            QSKIP("Set OMABOOK_WRITE_SPACE_SAMPLE to the file to write the sample to.");
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const auto snapshot = PublishedSpace::snapshot(shelf.library, shelf.sciFi,
                                                       {.graph = true, .questions = true, .ratings = true, .reading = true}, owner());
        QVERIFY2(snapshot.has_value(), qPrintable(snapshot.error()));
        QVERIFY(SpaceSnapshotFile::save(*snapshot, target).has_value());
    }
};

QTEST_MAIN(SharingTests)
#include "sharing_tests.moc"
