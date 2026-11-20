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
#include <QProcess>
#include <QSet>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <expected>
#include <optional>
#include <utility>

#include "changelog.h"
#include "collectionshare.h"
#include "fixtures.h"
#include "graph.h"
#include "graphcontext.h"
#include "library.h"
#include "person.h"
#include "publishedlog.h"
#include "publishedspace.h"
#include "spacereducer.h"
#include "spaces.h"
#include "spacesnapshot.h"
#include "subscribedspace.h"

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
    QVERIFY2(snapshot.has_value(), qPrintable(Fixtures::whyNot(snapshot)));
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

// Why a result failed, for a check's message: a check's message is made
// also when it passes, and a result that holds a value has no error to read.
template <typename T> [[nodiscard]] QByteArray why(const std::expected<T, QString> &result) {
    return result ? QByteArray() : result.error().toLocal8Bit();
}

[[nodiscard]] qint64 rows(const Library &library, const QString &sql) {
    QSqlQuery query = ChangeLog::query(library, sql);
    return query.next() ? query.value(0).toLongLong() : -1;
}

// Where an operation of the graph says a thing was met or learned, if it does.
[[nodiscard]] std::optional<std::pair<int, double>> placeOf(const SpaceOperation &op) {
    if (op.entity != "fact" && op.entity != "edge" && !op.field.startsWith("seen:"))
        return std::nullopt;
    const QJsonObject at = QJsonDocument::fromJson(op.value.toUtf8()).object();
    return std::pair{at.value("section").toInt(), at.value("fraction").toDouble()};
}

[[nodiscard]] bool gone(const ReducedBook &book) { return SpaceReducer::isRemoved(book.alive, book.removed); }

// `omabook <arguments>`, the real binary, run to the end.
struct Ran {
    int code = -1;
    QString output, errors;
};

[[nodiscard]] Ran omabook(const QStringList &arguments, const QString &dataHome = {}) {
    QProcess process;
    // A data folder of its own is a person of its own (Person::userKeyFile).
    if (!dataHome.isEmpty()) {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("XDG_DATA_HOME", dataHome);
        process.setProcessEnvironment(environment);
    }
    process.start(OMABOOK_BINARY, arguments);
    process.closeWriteChannel();
    Ran ran;
    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished(2000);
        return ran;
    }
    ran.code = process.exitCode();
    ran.output = QString::fromUtf8(process.readAllStandardOutput());
    ran.errors = QString::fromUtf8(process.readAllStandardError());
    return ran;
}

// One tool of `omabook mcp` called, as an agent calls it: its result.
[[nodiscard]] QJsonObject tool(const QString &library, const QString &name, const QJsonObject &arguments) {
    QProcess server;
    server.start(OMABOOK_BINARY, {"mcp", "--library", library});
    const QJsonObject request{
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", QJsonObject{{"name", name}, {"arguments", arguments}}}};
    server.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
    while (!server.canReadLine())
        if (!server.waitForReadyRead(60000))
            break;
    const QJsonObject answer = QJsonDocument::fromJson(server.readLine()).object();
    server.closeWriteChannel();
    if (!server.waitForFinished(5000))
        server.kill();
    return answer.value("result").toObject();
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
        QVERIFY2(snapshot.has_value(), qPrintable(Fixtures::whyNot(snapshot)));
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
        QVERIFY2(back.has_value(), qPrintable(Fixtures::whyNot(back)));
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
        QVERIFY2(known.has_value(), qPrintable(Fixtures::whyNot(known)));

        const auto newer = SpaceSnapshotFile::read(rewritten(file, "2.0", false));
        QVERIFY(!newer.has_value());
        QVERIFY2(newer.error().contains("newer Omabook") && newer.error().contains("2.0") && newer.error().contains("Update Omabook"),
                 qPrintable(Fixtures::whyNot(newer)));
        const auto later = SpaceSnapshotFile::read(rewritten(file, "1.7", true));
        QVERIFY2(later.has_value(), qPrintable(Fixtures::whyNot(later)));
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
            QVERIFY2(snapshot.has_value(), qPrintable(name + ": " + Fixtures::whyNot(snapshot)));
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

    // What the owner hasn't read yet doesn't go: of the graph, nothing met,
    // learned or related past their place in a book, nothing of a book not
    // begun, nor a thing's summary of now. What they meet later follows.
    void nothingPastTheOwnersPlaceIsPublished() {
        Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        Graph graph(shelf.library);
        const qint64 leto = graph.upsert({}, "character", "Leto II", "The duke's grandson.", {}, GraphPlace{shelf.dune, 5, 0.1}, "Leto");
        const qint64 hayt = graph.upsert({}, "character", "Hayt", "A ghola.", {}, GraphPlace{shelf.messiah, 1, 0.1}, "Hayt");
        QCOMPARE(graph.upsert({}, "character", "Paul Atreides", "He becomes emperor.", {}, GraphPlace{shelf.dune, 6, 0.2}, "emperor"),
                 shelf.paul);
        QVERIFY(graph.link(shelf.paul, leto, "father of", {}, GraphPlace{shelf.dune, 5, 0.1}) > 0);

        const QList<SpaceOperation> ops = shelf.published({.graph = true, .questions = true});
        const QSet<QString> things = idsOf(ops, "node");
        QVERIFY(things.contains(shelf.node(shelf.paul)) && things.contains(shelf.node(shelf.jessica)));
        QVERIFY(!things.contains(shelf.node(leto)));
        QVERIFY(!things.contains(shelf.node(hayt))); // Dune Messiah isn't begun
        for (const SpaceOperation &op : ops) {
            QVERIFY2(!op.value.contains("emperor") && !op.value.contains("father of") && !op.value.contains("Leto"),
                     qPrintable(op.field + ": " + op.value));
            if (const auto at = placeOf(op))
                QVERIFY2(*at <= (std::pair{3, 0.5}), qPrintable(op.field + ": " + op.value)); // where Dune is read to
        }
        const ReducedState before = SpaceReducer::reduce(ops);
        QVERIFY(before.nodes.value(shelf.node(shelf.paul)).fields.value("summary").value.isEmpty());

        // The owner reads on, and the space follows: a newer snapshot has
        // what they have met since, and still nothing of the book not begun.
        const SpaceSnapshot::Owner ada = owner();
        const auto first = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.graph = true}, ada);
        QVERIFY2(first.has_value(), why(first).constData());
        QVERIFY(!idsOf(first->operations, "node").contains(shelf.node(leto)));
        shelf.library.setProgress(shelf.dune, {.section = 7, .fraction = 0.1, .percent = 90, .updated = 1794740000});
        const auto later = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.graph = true}, ada);
        QVERIFY2(later.has_value(), why(later).constData());
        const ReducedState after = SpaceReducer::reduce(later->operations);
        QVERIFY(after.nodes.contains(shelf.node(leto)));
        QVERIFY(!after.nodes.contains(shelf.node(hayt)));
        QVERIFY(std::ranges::any_of(after.facts, [](const ReducedFact &fact) { return fact.value == "He becomes emperor."; }));
        QVERIFY(std::ranges::any_of(after.edges, [](const ReducedEdge &edge) { return edge.kind == "father of"; }));
    }

    // The published space follows the collection: renamed, a book in and
    // one out, details and stars changed, a newer snapshot says so. What an
    // earlier one held keeps its numbers in the space's log, so one who
    // holds it needs only the operations after it to be where the newer is.
    void aLaterChangeShowsUpInANewerSnapshot() {
        Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const SpaceSnapshot::Owner ada = owner();
        const auto first = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.ratings = true}, ada);
        QVERIFY2(first.has_value(), why(first).constData());

        QVERIFY(shelf.library.renameCollection(shelf.sciFi, "Science fiction"));
        QVERIFY(shelf.library.putInCollection(shelf.sciFi, shelf.outside, true));
        QVERIFY(shelf.library.putInCollection(shelf.sciFi, shelf.picked, false));
        BookMeta meta = shelf.library.book(shelf.messiah)->meta;
        meta.title = "Dune Messiah, Revised";
        QVERIFY(shelf.library.setMeta(shelf.messiah, meta));
        QVERIFY(shelf.library.setReading(shelf.dune, "reading", 5, {}));

        const auto second = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.ratings = true}, ada);
        QVERIFY2(second.has_value(), why(second).constData());
        QCOMPARE(second->space, first->space);
        QCOMPARE(second->name, QString("Science fiction"));
        QVERIFY(second->clock >= first->clock);
        const ReducedState state = SpaceReducer::reduce(second->operations);
        QVERIFY(!collectionNamed(state, "Science fiction").isEmpty());
        QVERIFY(collectionNamed(state, "Sci-fi").isEmpty());
        QCOMPARE(state.books.value(shelf.gid(shelf.outside)).fields.value("title").value, QString("Outside the Shelf"));
        QVERIFY(!gone(state.books.value(shelf.gid(shelf.outside))));
        QVERIFY(gone(state.books.value(shelf.gid(shelf.picked))));
        QCOMPARE(state.books.value(shelf.gid(shelf.messiah)).fields.value("title").value, QString("Dune Messiah, Revised"));
        QCOMPARE(state.books.value(shelf.gid(shelf.dune)).fields.value("rating").value, QString("5"));

        // The numbers stay: every operation of the first is in the log as it
        // was, and of the second those numbered as far as the first went are
        // the first's.
        const QList<SpaceOperation> log = PublishedLog::operations(shelf.library, first->space);
        for (const SpaceOperation &op : first->operations)
            QVERIFY2(log.contains(op), qPrintable(op.field + ": " + op.value));
        QSet<QString> devices;
        qint64 last = 0;
        for (const SpaceOperation &op : first->operations) {
            devices << op.device;
            last = std::max(last, op.seq);
        }
        QCOMPARE(devices.size(), 1);
        QVERIFY(devices.begin()->startsWith("device-") && !devices.begin()->contains(Spaces::deviceId(shelf.library)));
        for (const SpaceOperation &op : second->operations)
            QVERIFY2(op.seq > last || first->operations.contains(op), qPrintable(op.field + ": " + op.value));

        // The first and what came after it say what the second says.
        QList<SpaceOperation> caughtUp = first->operations;
        for (const SpaceOperation &op : log)
            if (op.seq > last)
                caughtUp << op;
        const ReducedState followed = SpaceReducer::reduce(caughtUp);
        QCOMPARE(QSet<QString>(followed.books.keyBegin(), followed.books.keyEnd()),
                 QSet<QString>(state.books.keyBegin(), state.books.keyEnd()));
        for (auto book = state.books.cbegin(); book != state.books.cend(); ++book) {
            QCOMPARE(gone(followed.books.value(book.key())), gone(*book));
            if (gone(*book))
                continue; // the newer snapshot says only that it went
            for (const char *field : {"title", "rating"})
                QCOMPARE(followed.books.value(book.key()).fields.value(field).value, book->fields.value(field).value);
        }
        QCOMPARE(collectionNamed(followed, "Science fiction"), collectionNamed(state, "Science fiction"));

        // Nothing changed, nothing new; a part switched off is said to be gone.
        const auto again = PublishedSpace::publish(shelf.library, shelf.sciFi, {.ratings = true}, ada);
        QVERIFY2(again.has_value(), why(again).constData());
        QCOMPARE(again->appended, 0);
        QCOMPARE(again->operations, log.size());
        const auto without = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {}, ada);
        QVERIFY2(without.has_value(), why(without).constData());
        QCOMPARE(SpaceReducer::reduce(without->operations).books.value(shelf.gid(shelf.dune)).fields.value("rating").value, QString());
    }

    // Switching the personal parts off takes all of them back: a snapshot
    // written after holds none of it (not a byte of the file), and one who
    // held the earlier snapshot and takes the operations after it is left
    // without it too.
    void switchingAPartOffTakesItBackFromLaterSnapshotsAndFollowers() {
        Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const SpaceSnapshot::Owner ada = owner();
        const auto first =
            PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.graph = true, .questions = true, .ratings = true, .reading = true}, ada);
        QVERIFY2(first.has_value(), why(first).constData());
        const QByteArray before = SpaceSnapshotFile::write(*first);
        const QList<QByteArray> personal{
            "\"rating\",\"4\"", "\"progress\",\"{", "\"shelf\",\"reading\"", "\"range\"", "2026-11-01", "\"revealed\"", "\"link\"", "seen:",
            "Paul Atreides",    "Who is Paul",      "The duke's son"};
        for (const QByteArray &word : personal)
            QVERIFY2(before.contains(word), word.constData()); // there to be taken back

        const auto second = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {}, ada);
        QVERIFY2(second.has_value(), why(second).constData());
        QCOMPARE(second->space, first->space);
        const QByteArray after = SpaceSnapshotFile::write(*second);
        for (const QByteArray &word : personal)
            QVERIFY2(!after.contains(word), word.constData());
        const ReducedState now = SpaceReducer::reduce(second->operations);
        QCOMPARE(now.books.size(), 3);
        QCOMPARE(now.books.value(shelf.gid(shelf.dune)).fields.value("title").value, QString("Dune"));

        // One who follows: the first snapshot, then what the log took after it.
        qint64 last = 0;
        for (const SpaceOperation &op : first->operations)
            last = std::max(last, op.seq);
        QList<SpaceOperation> followed = first->operations;
        for (const SpaceOperation &op : PublishedLog::operations(shelf.library, first->space))
            if (op.seq > last)
                followed << op;
        const ReducedState state = SpaceReducer::reduce(followed);
        const auto isGone = [](const auto &thing) { return SpaceReducer::isRemoved(thing.alive, thing.removed); };
        QVERIFY(!state.nodes.isEmpty() && std::ranges::all_of(state.nodes, isGone));
        QVERIFY(!state.facts.isEmpty() && std::ranges::all_of(state.facts, isGone));
        QVERIFY(!state.edges.isEmpty() && std::ranges::all_of(state.edges, isGone));
        QVERIFY(!state.days.isEmpty() && std::ranges::none_of(state.days, [](const ReducedDay &day) { return day.valid; }));
        for (const ReducedBook &book : state.books)
            for (const char *field : {"rating", "progress", "shelf", "finished"})
                QVERIFY2(book.fields.value(field).value.isEmpty() || book.fields.value(field).value == "0", field);

        // Switched on again, it comes back.
        const auto again = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.reading = true}, ada);
        QVERIFY2(again.has_value(), why(again).constData());
        const ReducedState back = SpaceReducer::reduce(again->operations);
        QVERIFY(std::ranges::any_of(back.days, [](const ReducedDay &day) { return day.valid; }));
        QCOMPARE(back.books.value(shelf.gid(shelf.dune)).fields.value("shelf").value, QString("reading"));
    }

    // A collection deleted at home is taken back from its space: what follows
    // the space is its removal of all it held, and a snapshot of it holds
    // nothing more.
    void aCollectionDeletedAtHomeIsTakenBackFromItsSpace() {
        Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const auto first = PublishedSpace::snapshot(shelf.library, shelf.sciFi, {.graph = true, .ratings = true}, owner());
        QVERIFY2(first.has_value(), why(first).constData());
        QVERIFY(shelf.library.removeCollection(shelf.sciFi));

        const auto followed = PublishedSpace::follow(shelf.library, first->space);
        QVERIFY2(followed.has_value(), why(followed).constData());
        QVERIFY(followed->appended > 0);
        const auto later = PublishedSpace::snapshotOf(shelf.library, first->space);
        QVERIFY2(later.has_value(), why(later).constData());
        QCOMPARE(later->owner, first->owner);
        QVERIFY(!later->operations.isEmpty());
        for (const SpaceOperation &op : later->operations)
            QVERIFY2(op.field == "removed" && op.value == "1", qPrintable(op.entity + " " + op.field + ": " + op.value));
        const QByteArray file = SpaceSnapshotFile::write(*later);
        for (const char *word : {"Dune", "Paul Atreides", "9780441172719", "\"rating\""})
            QVERIFY2(!file.contains(word), word);

        qint64 last = 0;
        for (const SpaceOperation &op : first->operations)
            last = std::max(last, op.seq);
        QList<SpaceOperation> all = first->operations;
        for (const SpaceOperation &op : PublishedLog::operations(shelf.library, first->space))
            if (op.seq > last)
                all << op;
        const ReducedState state = SpaceReducer::reduce(all);
        const auto isGone = [](const auto &thing) { return SpaceReducer::isRemoved(thing.alive, thing.removed); };
        QVERIFY(!state.books.isEmpty() && std::ranges::all_of(state.books, isGone));
        QVERIFY(!state.collections.isEmpty() && std::ranges::all_of(state.collections, isGone));
        QVERIFY(std::ranges::all_of(state.nodes, isGone) && std::ranges::all_of(state.edges, isGone));
        const auto quiet = PublishedSpace::follow(shelf.library, first->space);
        QVERIFY(quiet.has_value() && quiet->appended == 0);
    }

    // Before anything is written: what goes in, counted by kind, and what is
    // left out, the files always and each part that isn't switched on.
    void theSummaryCountsWhatGoesInAndWhatIsLeftOut() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const Collection sciFi = *shelf.library.collection(shelf.sciFi);
        const auto shown = CollectionShare::preview(shelf.library, sciFi, {.graph = true});
        QVERIFY2(shown.has_value(), why(shown).constData());
        const QJsonObject in = shown->value("in").toObject();
        QCOMPARE(in.value("collections").toInt(), 2);
        QCOMPARE(in.value("books").toInt(), 3);
        QVERIFY(in.value("things").toInt() >= 2); // Paul and Jessica, and who wrote the books
        QCOMPARE(in.value("facts").toInt(), 2);
        QVERIFY(in.value("relations").toInt() >= 1);
        for (const char *none : {"questions", "ratings", "reading_places", "reading_days"})
            QVERIFY2(in.value(none).toInt() == 0, none);

        QHash<QString, QJsonObject> out;
        for (const QJsonValue &value : shown->value("left_out").toArray())
            out.insert(value.toObject().value("part").toString(), value.toObject());
        QCOMPARE(QSet<QString>(out.keyBegin(), out.keyEnd()), (QSet<QString>{"files", "questions", "ratings", "progress"}));
        QCOMPARE(out.value("files").value("count").toInt(), 3);
        QCOMPARE(out.value("files").value("switch").toString(), QString("--with files"));
        QCOMPARE(shown->value("files").toInt(), 3);
        QCOMPARE(out.value("questions").value("count").toInt(), 1);
        QCOMPARE(out.value("ratings").value("count").toInt(), 1);
        QCOMPARE(out.value("ratings").value("switch").toString(), QString("--with ratings"));
        QCOMPARE(out.value("progress").value("places").toInt(), 1);
        const qsizetype days = SpaceReducer::reduce(shelf.published({.reading = true})).days.size();
        QVERIFY(days > 0);
        QCOMPARE(out.value("progress").value("days").toInt(), days);

        // With every part but the files: they go in, counted as they were left out, and only the files stay out.
        const auto all =
            CollectionShare::preview(shelf.library, sciFi, {.graph = true, .questions = true, .ratings = true, .reading = true});
        QVERIFY2(all.has_value(), why(all).constData());
        const QJsonObject every = all->value("in").toObject();
        QCOMPARE(every.value("questions").toInt(), 1);
        QCOMPARE(every.value("ratings").toInt(), 1);
        QCOMPARE(every.value("reading_places").toInt(), 1);
        QCOMPARE(every.value("reading_days").toInt(), days);
        QCOMPARE(all->value("left_out").toArray().size(), 1);
        // Counting published nothing.
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM published_ops"), 0);
    }

    // The command: --dry-run shows what would go and writes nothing; a
    // share publishes, writes the snapshot, and remembers the name.
    void aDryRunWritesNothing() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const QString root = shelf.library.root();
        const QString path = QString(Shelf::aboveName) + "/Sci-fi";
        const QString file = shelf.dir.filePath(QString("Sci-fi") + SpaceSnapshotFile::suffix);
        const Ran dry =
            omabook({"collections", "share", path, "--with-graph", "--as", "Ada", "--output", file, "--dry-run", "--library", root});
        QVERIFY2(dry.code == 0, qPrintable(dry.errors));
        for (const char *said : {"Goes in", "3 books", "Left out", "--with files", "--with ratings", "--dry-run"})
            QVERIFY2(dry.output.contains(said), qPrintable(said + QString("\n") + dry.output));
        QVERIFY(!QFile::exists(file));
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM spaces WHERE kind = 'published'"), 0);
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM published"), 0);
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM published_ops"), 0);

        // The first time, the name to go by is asked for.
        const Ran nameless = omabook({"collections", "share", path, "--json", "--library", root});
        QVERIFY(nameless.code != 0 && nameless.errors.contains("--as"));
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM published_ops"), 0);

        const Ran shared =
            omabook({"collections", "share", path, "--with-graph", "--as", "Ada", "--output", file, "--json", "--library", root});
        QVERIFY2(shared.code == 0, qPrintable(shared.errors));
        const QJsonObject done = QJsonDocument::fromJson(shared.output.toUtf8()).object().value("published").toObject();
        QVERIFY(done.value("space").toString().startsWith("published:"));
        QVERIFY(done.value("new").toInteger() > 0);
        QCOMPARE(done.value("operations").toInteger(), done.value("new").toInteger());
        const auto snapshot = SpaceSnapshotFile::load(file);
        QVERIFY2(snapshot.has_value(), why(snapshot).constData());
        QCOMPARE(snapshot->space, done.value("space").toString());
        QCOMPARE(snapshot->owner.name, QString("Ada"));
        QCOMPARE(snapshot->parts, QStringList{"graph"});
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM spaces WHERE kind = 'published'"), 1);

        // Again, unchanged: nothing new, and the name is the one given before.
        const Ran again = omabook({"collections", "share", path, "--with-graph", "--library", root});
        QVERIFY2(again.code == 0, qPrintable(again.errors));
        QVERIFY2(again.output.contains("by Ada") && again.output.contains("0 of them new"), qPrintable(again.output));
    }

    // From one library to another and on: shared with its graph, stars and
    // reading, imported; then changed and its parts switched off, shared
    // again, and the newer snapshot imported: the receiver's layer has the
    // change and none of what was switched off.
    void aShareIsImportedAndItsChangesAndRetractionsFollow() {
        Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const QTemporaryDir there;
        const QString owner = there.filePath("owner-data"), reader = there.filePath("reader-data");
        const QString receiver = there.filePath("Library");
        const QString root = shelf.library.root();
        const QString first = there.filePath(QString("first") + SpaceSnapshotFile::suffix);
        const QString second = there.filePath(QString("second") + SpaceSnapshotFile::suffix);

        const Ran shared = omabook({"collections", "share", QString(Shelf::aboveName) + "/Sci-fi", "--with-graph", "--with",
                                    "ratings,progress", "--as", "Ada", "--output", first, "--library", root},
                                   owner);
        QVERIFY2(shared.code == 0, qPrintable(shared.errors));
        const Ran imported = omabook({"collections", "import", first, "--library", receiver}, reader);
        QVERIFY2(imported.code == 0, qPrintable(imported.errors + imported.output));
        const auto snapshot = SpaceSnapshotFile::load(first);
        QVERIFY2(snapshot.has_value(), why(snapshot).constData());
        const QString space = snapshot->space;
        {
            Library library(receiver);
            QVERIFY(library.open());
            QVERIFY(SubscribedSpace::isSubscribed(library, space));
            const ReducedState held = SpaceReducer::reduce(SubscribedSpace::operations(library, space));
            QCOMPARE(held.books.value(shelf.gid(shelf.dune)).fields.value("rating").value, QString("4"));
            QVERIFY(std::ranges::any_of(held.days, [](const ReducedDay &day) { return day.valid; }));
            QVERIFY(std::ranges::any_of(held.nodes,
                                        [](const ReducedNode &node) { return !SpaceReducer::isRemoved(node.alive, node.removed); }));
        }

        QVERIFY(shelf.library.renameCollection(shelf.sciFi, "Science fiction"));
        QVERIFY(shelf.library.putInCollection(shelf.sciFi, shelf.picked, false));
        BookMeta meta = shelf.library.book(shelf.messiah)->meta;
        meta.title = "Dune Messiah, Revised";
        QVERIFY(shelf.library.setMeta(shelf.messiah, meta));
        const Ran again =
            omabook({"collections", "share", QString(Shelf::aboveName) + "/Science fiction", "--output", second, "--library", root}, owner);
        QVERIFY2(again.code == 0, qPrintable(again.errors));
        const Ran newer = omabook({"collections", "import", second, "--library", receiver}, reader);
        QVERIFY2(newer.code == 0, qPrintable(newer.errors + newer.output));

        Library library(receiver);
        QVERIFY(library.open());
        const ReducedState held = SpaceReducer::reduce(SubscribedSpace::operations(library, space));
        const auto isGone = [](const auto &thing) { return SpaceReducer::isRemoved(thing.alive, thing.removed); };
        QVERIFY(!collectionNamed(held, "Science fiction").isEmpty());
        QCOMPARE(held.books.value(shelf.gid(shelf.messiah)).fields.value("title").value, QString("Dune Messiah, Revised"));
        QVERIFY(isGone(held.books.value(shelf.gid(shelf.picked))));
        QVERIFY(!isGone(held.books.value(shelf.gid(shelf.dune))));
        for (const ReducedBook &book : held.books)
            for (const char *field : {"rating", "progress", "shelf", "finished"})
                QVERIFY2(book.fields.value(field).value.isEmpty() || book.fields.value(field).value == "0", field);
        QVERIFY(std::ranges::none_of(held.days, [](const ReducedDay &day) { return day.valid; }));
        QVERIFY(std::ranges::all_of(held.nodes, isGone) && std::ranges::all_of(held.facts, isGone)
                && std::ranges::all_of(held.edges, isGone));
    }

    void theMcpToolSharesACollection() {
        const Shelf shelf;
        QVERIFY(!QTest::currentTestFailed());
        const QString root = shelf.library.root();
        const QString path = QString(Shelf::aboveName) + "/Sci-fi";
        const QJsonObject dry =
            tool(root, "collection_share", {{"name", path}, {"graph", true}, {"with", QJsonArray{"ratings"}}, {"dry_run", true}});
        QVERIFY2(!dry.value("isError").toBool(), qPrintable(QJsonDocument(dry).toJson()));
        const QJsonObject shown = dry.value("structuredContent").toObject();
        QCOMPARE(shown.value("in").toObject().value("books").toInt(), 3);
        QCOMPARE(shown.value("in").toObject().value("ratings").toInt(), 1);
        QCOMPARE(shown.value("parts").toArray(), (QJsonArray{"graph", "ratings"}));
        QVERIFY(shown.value("dry_run").toBool() && !shown.contains("published"));
        QCOMPARE(rows(shelf.library, "SELECT COUNT(*) FROM published_ops"), 0);

        const QString file = shelf.dir.filePath(QString("Shared") + SpaceSnapshotFile::suffix);
        const QJsonObject shared = tool(root, "collection_share", {{"name", path}, {"as", "Ada"}, {"output", file}});
        QVERIFY2(!shared.value("isError").toBool(), qPrintable(QJsonDocument(shared).toJson()));
        QVERIFY(shared.value("structuredContent").toObject().value("published").toObject().value("new").toInteger() > 0);
        const auto snapshot = SpaceSnapshotFile::load(file);
        QVERIFY2(snapshot.has_value(), why(snapshot).constData());
        QCOMPARE(snapshot->parts, QStringList());
        QVERIFY(idsOf(snapshot->operations, "node").isEmpty());
        QVERIFY(!tool(root, "collection_share", {{"name", "Nowhere"}, {"dry_run", true}}).value("content").toArray().isEmpty());
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
        QVERIFY2(snapshot.has_value(), qPrintable(Fixtures::whyNot(snapshot)));
        QVERIFY(SpaceSnapshotFile::save(*snapshot, target).has_value());
    }
};

QTEST_MAIN(SharingTests)
#include "sharing_tests.moc"
