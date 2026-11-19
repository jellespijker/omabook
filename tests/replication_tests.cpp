// Replicating the change log between computers, and syncing with an Omabook
// from before it (ADR 0001, Migration 3, 4 and 7).

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <algorithm>
#include <tuple>

#include "changelog.h"
#include "compaction.h"
#include "fixtures.h"
#include "graph.h"
#include "hlc.h"
#include "library.h"
#include "remote.h"
#include "replication.h"
#include "samples.h"
#include "spacereducer.h"
#include "spacereplay.h"
#include "spaces.h"
#include "sync.h"

namespace {

// A stand-in for ssh in front of Fixtures::standInSsh that writes down every
// command it is given. As an `older` Omabook it refuses the options one from
// before the log doesn't know, as QCommandLineParser does (exit code 1).
QString recordingSsh(const QTemporaryDir &dir, const QString &name, bool older) {
    const QByteArray standIn = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY).toUtf8();
    const QByteArray log = dir.filePath(name + ".log").toUtf8();
    const QByteArray refuse = older ? "case \"$*\" in\n"
                                      "  *\"'--log'\"*) echo \"Unknown option 'log'.\" >&2; exit 1 ;;\n"
                                      "  *\"'--replicate'\"*) echo \"Unknown option 'replicate'.\" >&2; exit 1 ;;\n"
                                      "esac\n"
                                    : "";
    return Fixtures::script(dir.filePath(name), "echo \"$*\" >> '" + log + "'\n" + refuse + "exec '" + standIn + "' \"$@\"\n");
}

QString commandsOf(const QTemporaryDir &dir, const QString &name) {
    QFile file(dir.filePath(name + ".log"));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

int countOf(const QList<SpaceOperation> &ops, const QString &field) {
    return static_cast<int>(std::ranges::count_if(ops, [&field](const SpaceOperation &op) { return op.field == field; }));
}

// The same reading on two copies of a library, with every time given, so a
// sync of the one pair and of the other have the same to do.
void readOn(Library &library, const Progress &place, qint64 detailsHlc, const QString &publisher) {
    library.setProgress(1, place);
    BookMeta meta = library.book(3)->meta;
    meta.publisher = publisher;
    QVERIFY(library.setMeta(3, meta, nullptr, detailsHlc));
}

// The size of a library's database file once its write-ahead log is folded
// in, and with `vacuum` once the pages compaction freed are given back.
qint64 fileSize(const QString &file, bool vacuum) {
    const QString connection = "size-" + QUuid::createUuid().toString(QUuid::Id128);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(file);
        if (database.open()) {
            QSqlQuery query(database);
            if (vacuum)
                query.exec("VACUUM");
            query.exec("PRAGMA wal_checkpoint(TRUNCATE)");
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return QFileInfo(file).size();
}

// A snapshot without what is left out of a comparison.
QJsonObject without(QJsonObject snapshot, const QString &section) {
    snapshot.remove(section);
    return snapshot;
}

// The same without the knowledge graph's nodes a library makes of its own
// books (Graph::bookNode: the book, its authors, series and tags), which no
// sync carries.
QJsonObject withoutMadeNodes(QJsonObject snapshot) {
    static const QStringList made{"book", "author", "series", "tag"};
    QJsonArray nodes;
    for (const auto &node : snapshot.value("nodes").toArray())
        if (!made.contains(node.toObject().value("kind").toString()))
            nodes << node;
    snapshot.insert("nodes", nodes);
    return snapshot;
}

// The same with each removal by what was removed, not when.
QJsonObject withoutRemovalTimes(QJsonObject snapshot) {
    QJsonArray removals;
    for (const auto &removal : snapshot.value("removals").toArray())
        removals << removal.toObject().value("key");
    snapshot.insert("removals", removals);
    return snapshot;
}

// The first place two snapshots differ, as section[index]: actual where expected, or nothing.
QString firstDifference(const QJsonObject &actual, const QJsonObject &expected) {
    const auto text = [](const QJsonValue &value) {
        return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
    };
    for (const QString &key : expected.keys() + actual.keys()) {
        const QJsonArray mine = actual.value(key).toArray();
        const QJsonArray theirs = expected.value(key).toArray();
        for (qsizetype i = 0; i < std::max(mine.size(), theirs.size()); ++i) {
            const QJsonValue one = i < mine.size() ? mine.at(i) : QJsonValue();
            const QJsonValue other = i < theirs.size() ? theirs.at(i) : QJsonValue();
            if (one != other)
                return QString("%1[%2]: expected %4, got %3").arg(key).arg(i).arg(text(one), text(other));
        }
    }
    return {};
}

// Two copies of a library sample synced the old way (by the manifest, as with
// an older Omabook) and two through the log, to compare after each step.
struct SamplePairs {
    SamplePairs(const QString &sample, const QString &name)
        : dump(name), oldHere(dir.filePath("OldHere")), oldThere(dir.filePath("OldThere")), logHere(dir.filePath("LogHere")),
          logThere(dir.filePath("LogThere")), older("laptop:" + dir.filePath("OldThere"), {recordingSsh(dir, "ssh-older", true)}),
          current("laptop:" + dir.filePath("LogThere"), {recordingSsh(dir, "ssh-log", false)}) {
        int format = 0;
        for (const char *copy : {"OldHere", "OldThere", "LogHere", "LogThere"}) {
            QVERIFY(QDir(dir.path()).mkpath(copy));
            Samples::loadSqlDump(sample, dir.filePath(QString(copy) + "/omabook.db"), format);
        }
        QVERIFY(oldHere.open() && oldThere.open() && logHere.open() && logThere.open());
        days = Samples::librarySnapshot(logHere).value("days");
    }

    void expectAlike(const char *step) const {
        const QString at = dump + ", step " + step + ", ";
        const QString here = firstDifference(withoutRemovalTimes(withoutMadeNodes(without(Samples::librarySnapshot(logHere), "days"))),
                                             withoutRemovalTimes(withoutMadeNodes(without(Samples::librarySnapshot(oldHere), "days"))));
        const QString there = firstDifference(withoutRemovalTimes(without(Samples::librarySnapshot(logThere), "days")),
                                              withoutRemovalTimes(without(Samples::librarySnapshot(oldThere), "days")));
        QVERIFY2(here.isEmpty(), qPrintable(at + "here: " + here));
        if (collectionTimesDiffer)
            expectAlikeButCollectionTimes(at);
        else
            QVERIFY2(there.isEmpty(), qPrintable(at + "there: " + there));
        QCOMPARE(Samples::librarySnapshot(logHere).value("days"), days);
        QCOMPARE(Samples::librarySnapshot(logThere).value("days"), days);
    }

    // From the removal step on: the old way moves a collection's time when a
    // book in it is removed (and hands it over as changed); the log, only when
    // the collection itself changes.
    void expectAlikeButCollectionTimes(const QString &at) const {
        const auto times = [](QJsonObject snapshot) {
            QJsonArray collections;
            for (const auto &collection : snapshot.value("collections").toArray()) {
                QJsonObject object = collection.toObject();
                object.remove("updated");
                collections << object;
            }
            snapshot.insert("collections", collections);
            return withoutRemovalTimes(without(snapshot, "days"));
        };
        const QString there = firstDifference(times(Samples::librarySnapshot(logThere)), times(Samples::librarySnapshot(oldThere)));
        QVERIFY2(there.isEmpty(), qPrintable(at + "there: " + there));
    }

    QTemporaryDir dir;
    QString dump;
    bool collectionTimesDiffer = false;
    Library oldHere, oldThere, logHere, logThere;
    Remote older, current;
    QJsonValue days;
};

void readFurtherAndChangeDetailsBothSides(SamplePairs &p) {
    // 1. Read further here, and the details changed both sides.
    for (Library *library : {&p.oldHere, &p.logHere})
        readOn(*library, {.section = 2, .fraction = 0.25, .percent = 70, .updated = 1794700000}, Hlc::pack(1794700100, 0), "Here Press");
    for (Library *library : {&p.oldThere, &p.logThere})
        readOn(*library, {.section = 1, .fraction = 0.9, .percent = 65, .updated = 1794699000}, Hlc::pack(1794700200, 0), "There Press");
    const SyncSummary oldFirst = syncLibrary(p.oldHere, p.older);
    const SyncSummary logFirst = syncLibrary(p.logHere, p.current);
    QVERIFY2(oldFirst.problems.isEmpty() && logFirst.problems.isEmpty(),
             qPrintable(p.dump + ": " + (oldFirst.problems + logFirst.problems).join('\n')));
    QCOMPARE(logFirst.placesSent, oldFirst.placesSent);
    QCOMPARE(logFirst.placesTaken, oldFirst.placesTaken);
    QCOMPARE(logFirst.detailsTaken + logFirst.detailsSent, oldFirst.detailsTaken + oldFirst.detailsSent);
    p.expectAlike("1");
    QCOMPARE(p.logThere.progress(1).updated, qint64(1794700000));
    QCOMPARE(Samples::librarySnapshot(p.oldThere).value("days").toArray().size(), p.days.toArray().size() + 1);
}

void readFurtherThere(SamplePairs &p) {
    // 2. Then read further there.
    for (Library *library : {&p.oldThere, &p.logThere})
        library->setProgress(1, {.section = 3, .fraction = 0.1, .percent = 80, .updated = 1794710000});
    const SyncSummary oldSecond = syncLibrary(p.oldHere, p.older);
    const SyncSummary logSecond = syncLibrary(p.logHere, p.current);
    QCOMPARE(logSecond.placesTaken, oldSecond.placesTaken);
    QCOMPARE(logSecond.placesSent, oldSecond.placesSent);
    p.expectAlike("2");
    QCOMPARE(p.logHere.progress(1).section, 3);
}

void changeDetailsWithNoTime(SamplePairs &p) {
    // 3. Details changed both sides with no time to say which are
    //    newer, as before times were kept: left as they are, and
    //    counted, until the other side's are taken.
    for (Library *library : {&p.oldHere, &p.logHere, &p.oldThere, &p.logThere}) {
        BookMeta meta = library->book(2)->meta;
        meta.publisher = library == &p.oldHere || library == &p.logHere ? "Here Books" : "There Books";
        QVERIFY(library->setMeta(2, meta, nullptr, 0));
    }
    const SyncSummary oldUndecided = syncLibrary(p.oldHere, p.older);
    const SyncSummary logUndecided = syncLibrary(p.logHere, p.current);
    QCOMPARE(oldUndecided.detailsUndecided, 1);
    QCOMPARE(logUndecided.detailsUndecided, oldUndecided.detailsUndecided);
    QCOMPARE(p.logHere.book(2)->meta.publisher, QString("Here Books"));
    QCOMPARE(p.logThere.book(2)->meta.publisher, QString("There Books"));
    p.expectAlike("3");
    const SyncSummary oldSettled = syncLibrary(p.oldHere, p.older, {}, SyncOptions{.details = SyncOptions::Details::Theirs});
    const SyncSummary logSettled = syncLibrary(p.logHere, p.current, {}, SyncOptions{.details = SyncOptions::Details::Theirs});
    QCOMPARE(logSettled.detailsTaken, oldSettled.detailsTaken);
    QCOMPARE(p.logHere.book(2)->meta.publisher, QString("There Books"));
    p.expectAlike("3, settled");
    QCOMPARE(syncLibrary(p.logHere, p.current).detailsUndecided, 0);
}

void makeAndRemoveCollections(SamplePairs &p) {
    // 4. A collection made on each side, then one removed, each at a
    //    time of its own so the four copies do the same.
    const auto store = [](Library &library, const QString &path, qint64 parent, qint64 at, bool removed = false) {
        Collection collection;
        collection.parent = parent;
        collection.name = path.section('/', -1);
        collection.rule = removed ? QString() : "rating:4";
        collection.updated = at;
        collection.id = library.collectionNamed(path) ? library.collectionNamed(path)->id : 0;
        collection.removed = removed;
        if (path == "Picks/Here" && !removed)
            collection.included = {1};
        return library.storeCollection(collection).value_or(0);
    };
    for (Library *library : {&p.oldHere, &p.logHere})
        QVERIFY(store(*library, "Picks/Here", store(*library, "Picks", 0, 1794720000), 1794720010) > 0);
    for (Library *library : {&p.oldThere, &p.logThere})
        QVERIFY(store(*library, "To Lend", 0, 1794720020) > 0);
    const SyncSummary oldCollected = syncLibrary(p.oldHere, p.older);
    const SyncSummary logCollected = syncLibrary(p.logHere, p.current);
    QCOMPARE(logCollected.collectionsTaken, oldCollected.collectionsTaken);
    QVERIFY(p.logThere.collectionNamed("Picks/Here") && p.logHere.collectionNamed("To Lend"));
    QVERIFY(p.logThere.collectionNamed("Picks/Here")->included.contains(1));
    p.expectAlike("4");
    for (Library *library : {&p.oldThere, &p.logThere}) {
        const std::optional<Collection> picked = library->collectionNamed("Picks/Here");
        QVERIFY(picked && store(*library, "Picks/Here", picked->parent, 1794720030, true) > 0);
    }
    QVERIFY(syncLibrary(p.oldHere, p.older).problems.isEmpty());
    QVERIFY(syncLibrary(p.logHere, p.current).problems.isEmpty());
    QVERIFY(p.logHere.collections(true).size() == p.oldHere.collections(true).size());
    QVERIFY(!p.logHere.collectionNamed("Picks/Here"));
    p.expectAlike("4, removed");
}

void removeOnEachSide(SamplePairs &p) {
    for (Library *library : {&p.oldHere, &p.logHere})
        QVERIFY(library->remove(3));
    for (Library *library : {&p.oldThere, &p.logThere})
        QVERIFY(library->remove(2));
    const SyncSummary oldRemoved = syncLibrary(p.oldHere, p.older);
    const SyncSummary logRemoved = syncLibrary(p.logHere, p.current);
    QVERIFY2(logRemoved.problems.isEmpty(), qPrintable(logRemoved.problems.join('\n')));
    QCOMPARE(logRemoved.removedHere, oldRemoved.removedHere);
    QCOMPARE(logRemoved.removedThere, oldRemoved.removedThere);
    QVERIFY(!p.logHere.book(2) && !p.logThere.book(3));
    p.collectionTimesDiffer = true;
    p.expectAlike("5");
    // Through the log, a removal keeps the time it was made on the computer that made it.
    QCOMPARE(Samples::librarySnapshot(p.logThere).value("removals"), Samples::librarySnapshot(p.logHere).value("removals"));
    const SyncSummary settled = syncLibrary(p.logHere, p.current);
    QCOMPARE(settled.removedHere + settled.removedThere + settled.received + settled.sent, 0);
}

void learnOnEachSide(SamplePairs &p) {
    for (Library *library : {&p.oldHere, &p.logHere})
        QVERIFY(Graph(*library).upsert({}, "character", "Mara", "Flies the boat.", {}, GraphPlace{1, 2, 0.5}, "Mara") > 0);
    for (Library *library : {&p.oldThere, &p.logThere}) {
        Graph graph(*library);
        const qint64 ann = graph.named({}, "character", "Ann");
        QVERIFY(ann > 0 && graph.learn(ann, "alias", "Annie", GraphMoment{GraphPlace{1, 1, 0.2}, "Annie"}) > 0);
        QVERIFY(graph.upsert({}, "place", "The Lighthouse", {}, {}, GraphPlace{1, 3, 0.1}, "lighthouse") > 0);
    }
    const SyncSummary oldLearned = syncLibrary(p.oldHere, p.older);
    const SyncSummary logLearned = syncLibrary(p.logHere, p.current);
    QVERIFY2(logLearned.problems.isEmpty(), qPrintable(logLearned.problems.join('\n')));
    QVERIFY(logLearned.graphTaken > 0 && logLearned.graphSent > 0);
    QVERIFY(Graph(p.logThere).named({}, "character", "Mara") > 0);
    QVERIFY(Graph(p.logHere).node(Graph(p.logHere).named({}, "character", "Ann"))->aliases().contains("Annie"));
    p.expectAlike("6");
    const SyncSummary settled = syncLibrary(p.logHere, p.current);
    QCOMPARE(settled.graphTaken + settled.graphSent, 0);
    QCOMPARE(syncLibrary(p.oldHere, p.older).graphTaken, 0);
}

} // namespace

class ReplicationTests : public QObject {
    Q_OBJECT

private slots:
    void sameAsLinksFoldBooksInAnyOrder() {
        const qint64 early = Hlc::pack(1794860000, 0);
        const qint64 late = Hlc::pack(1794860050, 0);
        const QList<SpaceOperation> ops{
            {"personal:a", "dev-a", 1, early, "dev-a", "book", "b:ccc", "progress", R"({"section":1,"fraction":0.1,"percent":10})"},
            {"personal:b", "dev-b", 1, late, "dev-b", "book", "b:bbb", "progress", R"({"section":4,"fraction":0.4,"percent":40})"},
            {"personal:a", "dev-a", 2, early, "dev-a", "book", "b:ccc", "same_as", "b:bbb"},
            {"personal:b", "dev-b", 2, early, "dev-b", "book", "b:bbb", "same_as", "b:aaa"},
        };
        QList<SpaceOperation> reversed = ops;
        std::ranges::reverse(reversed);
        for (const QList<SpaceOperation> &order : {ops, reversed}) {
            const QHash<QString, QString> canonical = SpaceReducer::sameAs(order);
            QCOMPARE(canonical.value("b:ccc"), QString("b:aaa"));
            QCOMPARE(canonical.value("b:bbb"), QString("b:aaa"));
            const ReducedState state = SpaceReducer::reduce(order);
            QCOMPARE(state.books.size(), 1);
            QCOMPARE(SpaceReducer::fieldHlc(state.books.value("b:aaa"), "progress"), late);
        }
    }

    void twoUpdatedComputersPairAndCarryPlacesThroughTheLog() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "On Both"));
        QVERIFY(Fixtures::write(dir.filePath("two.txt"), "Two formats.\n"));
        QVERIFY(Fixtures::write(dir.filePath("two.md"), "# Two formats\n"));
        const QString ssh = recordingSsh(dir, "ssh-log", false);
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const QString hereSpace = Spaces::personalSpace(here);
        const QString thereSpace = Spaces::personalSpace(there);
        QVERIFY(hereSpace != thereSpace);

        const qint64 sharedHere = here.add(dir.filePath("shared.epub")).id;
        const qint64 sharedThere = there.add(dir.filePath("shared.epub")).id;
        // The same book with its formats taken in in another order: the
        // content ids the two give it don't line up, today's rules match it.
        const qint64 twoHere = here.add(dir.filePath("two.txt")).id;
        QVERIFY(here.addFormat(twoHere, dir.filePath("two.md")));
        const qint64 twoThere = there.add(dir.filePath("two.md")).id;
        QVERIFY(there.addFormat(twoThere, dir.filePath("two.txt")));
        QVERIFY(Spaces::bookId(here, twoHere) != Spaces::bookId(there, twoThere));

        there.setProgress(sharedThere, {.section = 2, .fraction = 0.5, .percent = 50, .updated = 1794860200});
        here.setProgress(sharedHere, {.section = 1, .fraction = 0.1, .percent = 10, .updated = 1794860100});
        here.setProgress(twoHere, {.section = 0, .fraction = 0.8, .percent = 80, .updated = 1794860300});

        // The manifest for a log peer says so, and leaves the places out.
        const QJsonObject manifest = libraryManifest(there, ManifestFor::LogPeer);
        QCOMPARE(manifest.value("log").toInt(), 1);
        QCOMPARE(manifest.value("space").toString(), thereSpace);
        for (const auto &book : manifest.value("books").toArray()) {
            QVERIFY(!book.toObject().contains("progress"));
            QVERIFY(book.toObject().value("gid").toString().startsWith("b:"));
        }

        const Remote remote("laptop:" + dir.filePath("There"), {ssh});
        const SyncSummary first = syncLibrary(here, remote);
        QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
        QVERIFY2(first.problems.isEmpty(), qPrintable(first.problems.join('\n')));
        QCOMPARE(first.placesTaken, 1);
        QCOMPARE(first.placesSent, 1);
        QCOMPARE(here.progress(sharedHere).section, 2);
        QCOMPARE(here.progress(sharedHere).updated, qint64(1794860200));
        QCOMPARE(there.progress(twoThere).fraction, 0.8);
        QCOMPARE(there.progress(twoThere).updated, qint64(1794860300));

        // Through the log, never by the manifest's commands.
        const QString commands = commandsOf(dir, "ssh-log");
        QVERIFY2(commands.contains("'manifest' '--log'"), qPrintable(commands));
        QVERIFY2(commands.contains("'sync' '--replicate'"), qPrintable(commands));
        QVERIFY2(!commands.contains("'progress'"), qPrintable(commands));

        // Paired into the space with the smaller id, and the book that didn't
        // line up is one by a same-as operation, on both sides alike.
        QCOMPARE(Spaces::personalSpace(here), std::min(hereSpace, thereSpace));
        QCOMPARE(Spaces::personalSpace(there), std::min(hereSpace, thereSpace));
        QCOMPARE(countOf(Replication::operations(here), "same_as"), 1);
        QCOMPARE(Replication::operations(here), Replication::operations(there));
        QCOMPARE(Replication::vector(here), Replication::vector(there));

        // Nothing left to do, and nothing sent again.
        const SyncSummary settled = syncLibrary(here, remote);
        QVERIFY2(settled.problems.isEmpty(), qPrintable(settled.problems.join('\n')));
        QCOMPARE(settled.placesTaken + settled.placesSent, 0);
        QCOMPARE(countOf(Replication::operations(here), "same_as"), 1);
        QCOMPARE(Replication::since(here, Replication::vector(there)).size(), 0);

        // Reading on there, of the book known by another id: the place comes here.
        there.setProgress(twoThere, {.section = 1, .fraction = 0.2, .percent = 90, .updated = 1794860400});
        const SyncSummary again = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QCOMPARE(again.placesTaken, 1);
        QCOMPARE(again.titles.placesTaken, QStringList{here.book(twoHere)->meta.title});
        QCOMPARE(here.progress(twoHere).percent, 80.0);
        QCOMPARE(syncLibrary(here, remote).placesTaken, 1);
        QCOMPARE(here.progress(twoHere).percent, 90.0);
        QCOMPARE(here.progress(twoHere).updated, qint64(1794860400));
    }

    void detailsAndReadingTravelThroughTheLogAndEachSideTakesThem() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Told Twice"));
        const QString ssh = recordingSsh(dir, "ssh-details", false);
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("shared.epub")).id;
        const qint64 theirs = there.add(dir.filePath("shared.epub")).id;
        const Remote remote("laptop:" + dir.filePath("There"), {ssh});
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());

        // Newer details there, and each side says something of its reading.
        BookMeta meta = there.book(theirs)->meta;
        meta.publisher = "Harbour Press";
        const qint64 later = QDateTime::currentSecsSinceEpoch() + 60; // after every time the books were stamped with
        QVERIFY(there.setMeta(theirs, meta, nullptr, Hlc::pack(later, 0)));
        QVERIFY(there.setReading(theirs, {}, 4, {}));
        QVERIFY(here.setReading(mine, "read", 0, "2026-11-01"));

        // The manifest names the book and its files, and says nothing of its details or reading.
        const QJsonObject listed = libraryManifest(there, ManifestFor::LogPeer).value("books").toArray().first().toObject();
        QVERIFY(listed.contains("files") && listed.contains("gid") && listed.contains("cover"));
        for (const char *key : {"title", "publisher", "details_at", "field_hlc", "shelf", "rating", "identifiers"})
            QVERIFY2(!listed.contains(key), key);

        const SyncSummary dry = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QCOMPARE(dry.detailsTaken, 1);
        QCOMPARE(dry.readingTaken, 1);
        QCOMPARE(dry.readingSent, 1);
        QCOMPARE(dry.titles.readingSent, QStringList{"Told Twice"});
        QCOMPARE(here.book(mine)->meta.publisher, QString("Fixture Press"));

        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.detailsTaken, 1);
        QCOMPARE(summary.readingTaken, 1);
        QCOMPARE(summary.readingSent, 1);
        QCOMPARE(here.book(mine)->meta.publisher, QString("Harbour Press"));
        QCOMPARE(here.book(mine)->rating, 4);
        QCOMPARE(there.book(theirs)->shelf, QString("read"));
        QCOMPARE(there.book(theirs)->finished, QString("2026-11-01"));
        QCOMPARE(here.book(mine)->detailsAt, there.book(theirs)->detailsAt);

        // A newer cover here: a picture, which is all that is given by command.
        QImage cover(40, 60, QImage::Format_RGB32);
        cover.fill(Qt::darkCyan);
        QVERIFY(here.setCover(mine, cover, Hlc::pack(later + 60, 0)));
        const SyncSummary pictured = syncLibrary(here, remote);
        QVERIFY2(pictured.problems.isEmpty(), qPrintable(pictured.problems.join('\n')));
        QCOMPARE(Library::hashFile(there.coverPath(theirs)), Library::hashFile(here.coverPath(mine)));
        QCOMPARE(there.book(theirs)->detailsAt, later + 60);
        const QString commands = commandsOf(dir, "ssh-details");
        QVERIFY2(commands.contains("'--cover'"), qPrintable(commands));
        QVERIFY2(!commands.contains("'--meta'") && !commands.contains("'--status'"), qPrintable(commands));

        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.detailsTaken + settled.detailsSent + settled.readingTaken + settled.readingSent, 0);
    }

    void undecidedDetailsThroughTheLogWaitForMineOrTheirs() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "No Time Kept"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("shared.epub")).id;
        const qint64 theirs = there.add(dir.filePath("shared.epub")).id;
        for (const auto &[library, id, publisher] : {std::tuple{&here, mine, "Mine"}, std::tuple{&there, theirs, "Theirs"}}) {
            BookMeta meta = library->book(id)->meta;
            meta.publisher = publisher;
            QVERIFY(library->setMeta(id, meta, nullptr, 0));
        }
        const Remote remote("laptop:" + dir.filePath("There"), {recordingSsh(dir, "ssh-undecided", false)});
        for (int round = 0; round < 2; ++round) {
            const SyncSummary undecided = syncLibrary(here, remote);
            QCOMPARE(undecided.detailsUndecided, 1);
            QCOMPARE(undecided.titles.detailsUndecided, QStringList{"No Time Kept"});
            QCOMPARE(here.book(mine)->meta.publisher, QString("Mine"));
            QCOMPARE(there.book(theirs)->meta.publisher, QString("Theirs"));
        }
        const SyncSummary settled = syncLibrary(here, remote, {}, SyncOptions{.details = SyncOptions::Details::Mine});
        QCOMPARE(settled.detailsSent, 1);
        QCOMPARE(there.book(theirs)->meta.publisher, QString("Mine"));
        QCOMPARE(there.book(theirs)->detailsAt, qint64(0));
        QCOMPARE(syncLibrary(here, remote).detailsUndecided, 0);
    }

    void collectionsTravelThroughTheLogRenamesAndAll() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Kept Together"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("shared.epub")).id;
        const qint64 theirs = there.add(dir.filePath("shared.epub")).id;
        const auto made = here.addCollection("Study/Sea", "tag:sea");
        QVERIFY(made);
        const Remote remote("laptop:" + dir.filePath("There"), {recordingSsh(dir, "ssh-collections", false)});
        QCOMPARE(syncLibrary(here, remote).collectionsSent, 2); // "Study" and "Study/Sea"
        QVERIFY(there.collectionNamed("Study/Sea"));
        QVERIFY(!libraryManifest(there, ManifestFor::LogPeer).contains("collections"));

        // Renamed there and given the book by hand: the same collection here, not another.
        const qint64 sea = there.collectionNamed("Study/Sea")->id;
        QVERIFY(there.renameCollection(sea, "Coast"));
        QVERIFY(there.putInCollection(sea, theirs, true));
        const SyncSummary dry = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QCOMPARE(dry.collectionsTaken, 1);
        QVERIFY(here.collectionNamed("Study/Sea"));
        QCOMPARE(syncLibrary(here, remote).collectionsTaken, 1);
        QVERIFY(!here.collectionNamed("Study/Sea"));
        const std::optional<Collection> coast = here.collectionNamed("Study/Coast");
        QVERIFY(coast && coast->id == *made);
        QVERIFY(coast->included.contains(mine));
        QCOMPARE(here.collections().size(), 2);
        QVERIFY2(!commandsOf(dir, "ssh-collections").contains("'collections'"), qPrintable(commandsOf(dir, "ssh-collections")));
        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.collectionsTaken + settled.collectionsSent, 0);
    }

    void removalsTravelAsTombstonesAndABookChangedSinceIsKept() {
        QTemporaryDir dir;
        for (const char *title : {"Gone Both Ways", "Read After", "Gone There"})
            QVERIFY(Fixtures::epub(dir.filePath(QString(title) + ".epub"), title));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        QHash<QString, qint64> mine, theirs;
        for (const char *title : {"Gone Both Ways", "Read After", "Gone There"}) {
            mine.insert(title, here.add(dir.filePath(QString(title) + ".epub")).id);
            theirs.insert(title, there.add(dir.filePath(QString(title) + ".epub")).id);
        }
        const Remote remote("laptop:" + dir.filePath("There"), {recordingSsh(dir, "ssh-removals", false)});
        QVERIFY(syncLibrary(here, remote).problems.isEmpty());
        QVERIFY(!libraryManifest(there, ManifestFor::LogPeer).contains("removals"));

        // Removed here; removed there, and the other read there after.
        QVERIFY(here.remove(mine.value("Gone Both Ways")));
        QVERIFY(there.remove(theirs.value("Gone There")));
        QVERIFY(here.remove(mine.value("Read After")));
        const qint64 after = QDateTime::currentSecsSinceEpoch() + 60; // after the removal, whatever the two clocks say
        there.setProgress(theirs.value("Read After"), {.section = 2, .fraction = 0.5, .percent = 40, .updated = after});
        const SyncSummary dry = syncLibrary(here, remote, {}, SyncOptions{.dryRun = true});
        QCOMPARE(dry.titles.removedHere, QStringList{"Gone There"});
        QCOMPARE(dry.titles.removedThere, QStringList{"Gone Both Ways"});
        QVERIFY(here.book(mine.value("Gone There")));

        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.removedHere, 1);
        QCOMPARE(summary.removedThere, 1);
        QVERIFY(!here.book(mine.value("Gone There")) && !there.book(theirs.value("Gone Both Ways")));
        // Removed here at the time it was removed there.
        const QString goneThere = Library::hashFile(dir.filePath("Gone There.epub"));
        QCOMPARE(here.removedAt(goneThere), there.removedAt(goneThere));
        // Read there after it was removed here: kept there, and never brought back here.
        QCOMPARE(summary.received + summary.sent, 0);
        QVERIFY(here.books("Read After").isEmpty());
        QVERIFY(there.book(theirs.value("Read After")));
        QVERIFY2(!commandsOf(dir, "ssh-removals").contains("'remove'"), qPrintable(commandsOf(dir, "ssh-removals")));

        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.removedHere + settled.removedThere + settled.received + settled.sent, 0);
    }

    void theGraphTravelsThroughTheLogAndAThingOfAnotherNameIsLinkedOnce() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Of The Boat"));
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const qint64 mine = here.add(dir.filePath("shared.epub")).id;
        const qint64 theirs = there.add(dir.filePath("shared.epub")).id;
        // Here she is Mara, also called the Pilot; there only the Pilot is known, met further on.
        Graph graphHere(here);
        const qint64 mara = graphHere.upsert({}, "character", "Mara", {}, {}, GraphPlace{mine, 3, 0.5}, "Mara");
        QVERIFY(graphHere.learn(mara, "alias", "The Pilot") > 0);
        Graph graphThere(there);
        const qint64 pilot = graphThere.upsert({}, "character", "The Pilot", "Steers.", {}, GraphPlace{theirs, 1, 0.25}, "the Pilot");
        QVERIFY(pilot > 0
                && graphThere.link(pilot, graphThere.upsert({}, "place", "The Boat"), "steers", {}, GraphPlace{theirs, 1, 0.3}) > 0);
        QVERIFY(!libraryManifest(there, ManifestFor::LogPeer).contains("graph"));

        const Remote remote("laptop:" + dir.filePath("There"), {recordingSsh(dir, "ssh-graph", false)});
        const SyncSummary summary = syncLibrary(here, remote);
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QVERIFY(summary.graphTaken > 0);
        // One thing on each side, met where it was met first, with what was learned there.
        QCOMPARE(graphHere.named({}, "character", "The Pilot"), mara);
        QCOMPARE(graphHere.firstMet(mara, mine)->place.section, 1);
        QCOMPARE(graphHere.node(mara)->summary, QString("Steers."));
        QCOMPARE(graphHere.around(mara).edges.size(), 1);
        QCOMPARE(graphThere.named({}, "character", "Mara"), pilot);
        QCOMPARE(countOf(SpaceReplay::operations(here), "same_as"), 1);
        QVERIFY2(!commandsOf(dir, "ssh-graph").contains("'graph'"), qPrintable(commandsOf(dir, "ssh-graph")));

        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.graphTaken + settled.graphSent, 0);
        QCOMPARE(countOf(SpaceReplay::operations(here), "same_as"), 1);
    }

    // Compaction writes a snapshot and reads it with what came after: what that
    // merges to is what all of the log merges to, on every computer and for
    // each device. It drops from `ops` what the snapshot covers and leaves
    // out, of what every member has seen, and a peer behind the snapshot is
    // given the snapshot.
    void compactionDropsWhatEveryoneHasSeenAndMergesTheSame() {
        const QDir data(QFileInfo(QFINDTESTDATA("data/library-0.1.0.sql")).absolutePath());
        for (const QString &dump : data.entryList({"library-*.sql"}, QDir::Files, QDir::Name)) {
            SamplePairs p(data.filePath(dump), dump);
            QVERIFY(!QTest::currentTestFailed());
            // A while of reading and editing on both sides, synced now and then,
            // and the last three rounds here after the last sync.
            for (int round = 0; round < 23; ++round) {
                for (Library *library : {&p.logHere, &p.logThere}) {
                    if (round >= 20 && library == &p.logThere)
                        continue;
                    library->setProgress(1, {.section = round, .fraction = 0.5, .percent = round * 4.0, .updated = 1794730000 + round});
                    BookMeta meta = library->book(3)->meta;
                    meta.description = QString("Draft %1 of %2").arg(round).arg(library->root().section('/', -1));
                    QVERIFY(library->setMeta(3, meta, nullptr, Hlc::pack(1794730000 + (2 * round) + (library == &p.logHere ? 1 : 0), 0)));
                }
                if (round % 5 == 4)
                    QVERIFY(syncLibrary(p.logHere, p.current).problems.isEmpty());
            }
            const QString mine = Spaces::deviceId(p.logHere);
            const QString theirs = Spaces::deviceId(p.logThere);
            const auto places = [&] {
                QSqlQuery query = ChangeLog::query(p.logHere,
                                                   "SELECT COUNT(*) FROM ops WHERE field = 'progress' AND device = "
                                                   "(SELECT id FROM log_ids WHERE name = ?)",
                                                   {mine});
                return query.next() ? query.value(0).toLongLong() : -1;
            };
            const QList<SpaceOperation> all = SpaceReplay::operations(p.logHere);
            const qint64 placesBefore = places();
            const qint64 sizeBefore = fileSize(p.logHere.databasePath(), false);
            const qint64 sizeBeforeVacuumed = fileSize(p.logHere.databasePath(), true);
            const QJsonObject merged = Samples::librarySnapshot(p.logHere);
            const auto views = [](std::span<const SpaceOperation> ops, const QString &device) {
                QJsonObject books;
                const ReducedState state = SpaceReducer::reduce(ops, device);
                for (auto book = state.books.cbegin(); book != state.books.cend(); ++book)
                    books.insert(book.key(), Replication::bookView(*book));
                return books;
            };

            const Compaction::Result compacted = Compaction::compact(p.logHere);
            const QList<SpaceOperation> live = SpaceReplay::operations(p.logHere);
            QCOMPARE(compacted.operations + compacted.dropped, qint64(all.size()));
            QCOMPARE(compacted.live, qint64(live.size()));
            const qint64 sizeAfter = fileSize(p.logHere.databasePath(), false);
            const qint64 sizeVacuumed = fileSize(p.logHere.databasePath(), true);
            qInfo().noquote() << dump << ": ops" << all.size() << "before," << compacted.operations << "after," << live.size()
                              << "read; database" << sizeBefore << "bytes before," << sizeAfter << "after;" << sizeBeforeVacuumed
                              << "vacuumed before," << sizeVacuumed << "vacuumed after";
            // The snapshot takes the place of what it covers: the library is
            // smaller than before, and of what the snapshot covers only what
            // the other computer hasn't seen is still in `ops`.
            QVERIFY2(sizeVacuumed < sizeBeforeVacuumed, qPrintable(dump));
            QSqlQuery covered = ChangeLog::query(p.logHere,
                                                 "SELECT COUNT(*) FROM ops AS o JOIN snapshot_marks AS m ON m.space = o.space "
                                                 "AND m.device = o.device WHERE o.seq <= m.seq AND o.seq <= COALESCE((SELECT v.seq "
                                                 "FROM member_vectors AS v WHERE v.space = o.space AND v.device = o.device "
                                                 "AND v.member = ?), 0)",
                                                 {theirs});
            QVERIFY(covered.next());
            QCOMPARE(covered.value(0).toLongLong(), qint64(0));
            covered.finish();
            // Of the places and descriptions each side wrote, only the last of each is read;
            // what the other side has seen of them is gone, what it hasn't is kept.
            QVERIFY2(all.size() - live.size() >= 2 * 2 * 19, qPrintable(dump));
            QVERIFY2(compacted.dropped >= 2 * 2 * 18, qPrintable(dump));
            QVERIFY2(places() >= 3 && places() < placesBefore, qPrintable(dump));
            for (const QString &device : {mine, theirs})
                QCOMPARE(views(live, device), views(all, device));
            Library replayed(p.dir.filePath("Replayed"));
            QVERIFY(replayed.open() && SpaceReplay::apply(replayed, live));
            Library full(p.dir.filePath("Full"));
            QVERIFY(full.open() && SpaceReplay::apply(full, all));
            QCOMPARE(firstDifference(Samples::librarySnapshot(replayed), Samples::librarySnapshot(full)), QString());
            QCOMPARE(Samples::librarySnapshot(p.logHere), merged);

            // Syncing on: what changed after the snapshot travels, on top of it.
            p.logThere.setProgress(1, {.section = 40, .fraction = 0.1, .percent = 99, .updated = 1794740000});
            QVERIFY(syncLibrary(p.logHere, p.current).problems.isEmpty());
            QCOMPARE(p.logHere.progress(1).section, 40);
            QCOMPARE(p.logThere.book(3)->meta.description, p.logHere.book(3)->meta.description);
            // And a computer that never synced before is given the snapshot, and ends alike.
            const Remote third("laptop:" + p.dir.filePath("OldThere"), {recordingSsh(p.dir, "ssh-third", false)});
            QVERIFY(syncLibrary(p.logHere, third).problems.isEmpty());
            QCOMPARE(p.oldThere.book(3)->meta.description, p.logHere.book(3)->meta.description);
            QCOMPARE(p.oldThere.progress(1).section, 40);
            QCOMPARE(syncLibrary(p.logHere, third).detailsTaken + syncLibrary(p.logHere, third).detailsSent, 0);
        }
    }

    // Nothing a member may lack is dropped: not of a computer of one's
    // personal space that hasn't synced since, nor of a published space,
    // whose owner is its member by their person id. Once each has said what
    // it holds, under the name its space knows it by, what it has seen goes.
    void compactionKeepsWhatAMemberHasNotSeen() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("book.epub"), "Kept"));
        Library library(dir.filePath("Here"));
        QVERIFY(library.open());
        const qint64 book = library.add(dir.filePath("book.epub")).id;
        const QString personal = Spaces::personalSpace(library);
        const QString published = "published:compaction";
        Spaces::ensurePublishedSpace(library, published, "Kept", "person-of-the-owner", "Ada");
        QVERIFY(ChangeLog::exec(library, "INSERT INTO space_members (space, person, role) VALUES (?, 'laptop-not-synced', 'writer')",
                                {personal}));
        for (int round = 0; round < 5; ++round) {
            library.setProgress(book, {.section = round, .fraction = 0.1, .percent = round * 10.0, .updated = 1794730000 + round});
            ChangeLog::append(library, {.space = published,
                                        .entity = "collection",
                                        .id = "c:kept",
                                        .field = "name",
                                        .value = QString("Kept %1").arg(round),
                                        .hlc = std::nullopt});
        }
        const auto count = [&library](const QString &space) {
            QSqlQuery query =
                ChangeLog::query(library, "SELECT COUNT(*) FROM ops WHERE space = (SELECT id FROM log_ids WHERE name = ?)", {space});
            return query.next() ? query.value(0).toLongLong() : -1;
        };
        const qint64 personalBefore = count(personal);
        const qint64 publishedBefore = count(published);
        const QJsonObject before = Samples::librarySnapshot(library);
        QCOMPARE(Compaction::compact(library).dropped, qint64(0));
        QCOMPARE(count(personal), personalBefore);
        QCOMPARE(count(published), publishedBefore);
        QCOMPARE(Samples::librarySnapshot(library), before);

        Compaction::rememberSeen(library, "laptop-not-synced", Replication::vector(library));
        QVERIFY(Compaction::compact(library).dropped > 0);
        QVERIFY(count(personal) < personalBefore);
        QCOMPARE(count(published), publishedBefore);
        Compaction::rememberSeen(library, "person-of-the-owner", Replication::vector(library));
        QVERIFY(Compaction::compact(library).dropped > 0);
        QVERIFY(count(published) < publishedBefore);
        QCOMPARE(Samples::librarySnapshot(library), before);
    }

    void anOlderOmabookStillSyncsByTheManifest() {
        QTemporaryDir dir;
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "On Both"));
        QVERIFY(Fixtures::epub(dir.filePath("other.epub"), "Read Further There"));
        const QString ssh = recordingSsh(dir, "ssh-older", true);
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        const QString hereSpace = Spaces::personalSpace(here);
        const qint64 sharedHere = here.add(dir.filePath("shared.epub")).id;
        const qint64 sharedThere = there.add(dir.filePath("shared.epub")).id;
        const qint64 otherHere = here.add(dir.filePath("other.epub")).id;
        const qint64 otherThere = there.add(dir.filePath("other.epub")).id;
        here.setProgress(sharedHere, {.section = 3, .fraction = 0.3, .percent = 30, .updated = 1794860500});
        there.setProgress(otherThere, {.section = 2, .fraction = 0.6, .percent = 60, .updated = 1794860600});
        BookMeta meta = there.book(otherThere)->meta;
        meta.publisher = "Older Press";
        QVERIFY(there.setMeta(otherThere, meta, nullptr, Hlc::pack(QDateTime::currentSecsSinceEpoch() + 60, 0)));
        QVERIFY(here.setReading(sharedHere, "read", 5, {}));
        QVERIFY(there.addCollection("From There", "status:reading"));
        QVERIFY(Graph(there).upsert({}, "character", "Pilot", {}, {}, GraphPlace{sharedThere, 1, 0.5}, "Pilot") > 0);
        QVERIFY(Fixtures::epub(dir.filePath("gone.epub"), "Gone Here"));
        const qint64 goneHere = here.add(dir.filePath("gone.epub")).id;
        QVERIFY(there.add(dir.filePath("gone.epub")).error.isEmpty());
        Spaces::setClockOffset(here, 120); // removed here after anything there, whose clock saw a minute ahead
        QVERIFY(here.remove(goneHere));
        Spaces::setClockOffset(here, 0);

        // An older Omabook's manifest says nothing of the log, and has the places.
        const QJsonObject manifest = libraryManifest(there);
        QVERIFY(!manifest.contains("log"));
        QVERIFY(manifest.value("books").toArray().first().toObject().contains("progress"));

        const SyncSummary summary = syncLibrary(here, Remote("laptop:" + dir.filePath("There"), {ssh}));
        QVERIFY2(summary.error.isEmpty(), qPrintable(summary.error));
        QVERIFY2(summary.problems.isEmpty(), qPrintable(summary.problems.join('\n')));
        QCOMPARE(summary.placesTaken, 1);
        QCOMPARE(summary.placesSent, 1);
        QCOMPARE(here.progress(otherHere).section, 2);
        QCOMPARE(here.progress(otherHere).updated, qint64(1794860600));
        QCOMPARE(there.progress(sharedThere).section, 3);
        QCOMPARE(there.progress(sharedThere).updated, qint64(1794860500));
        QCOMPARE(summary.detailsTaken, 1);
        QCOMPARE(here.book(otherHere)->meta.publisher, QString("Older Press"));
        QCOMPARE(summary.readingSent, 1);
        QCOMPARE(there.book(sharedThere)->rating, 5);
        QCOMPARE(summary.collectionsTaken, 1);
        QVERIFY(here.collectionNamed("From There"));
        QCOMPARE(summary.removedThere, 1);
        QVERIFY(there.books("Gone Here").isEmpty());
        QVERIFY(summary.graphTaken > 0);
        QVERIFY(Graph(here).named({}, "character", "Pilot") > 0);

        // Asked for the log, it refused; it was asked for its manifest and
        // given the place by the command it has always taken, as plain seconds.
        const QString commands = commandsOf(dir, "ssh-older");
        QVERIFY2(commands.contains("'manifest' '--log'") && commands.contains("'manifest' --library"), qPrintable(commands));
        QVERIFY2(commands.contains("'progress'") && commands.contains("'--updated' '1794860500'"), qPrintable(commands));
        QVERIFY2(commands.contains("'--status' 'read'") && commands.contains("'collections' 'take'") && commands.contains("'remove'")
                     && commands.contains("'graph' 'take'"),
                 qPrintable(commands));
        QVERIFY2(!commands.contains("'--replicate'"), qPrintable(commands));
        // No pairing with a computer that doesn't speak the log.
        QCOMPARE(Spaces::personalSpace(here), hereSpace);
        QCOMPARE(syncLibrary(here, Remote("laptop:" + dir.filePath("There"), {ssh})).placesTaken, 0);
    }

    // ADR 0001, Migration 7: two copies of each library sample, kept in step
    // the old way (by the manifest, as with an older Omabook) and by migrating
    // both and replicating the log, end up the same.
    //
    // But for two things, on purpose. The old way gives the other side a place
    // with `omabook progress`, which counts a place set by hand as reading
    // done there that day (a reading day, which the graph then carries back).
    // A place that comes through the log is no reading done on the computer
    // it comes to, so there the reading days stay as they were. And the old
    // way gives the other side details with `omabook set`, which makes that
    // side's knowledge graph nodes of the book (its own, its authors') follow
    // them, but not this side's; through the log, the side that takes them. A
    // removal the old way takes is stamped with the time of the sync; through
    // the log, with the time it was made. And a book removed from a collection
    // with it moves the collection's time the old way, not through the log.
    void everySampleSyncsAlikeTheOldWayAndThroughTheLog() {
        const QDir data(QFileInfo(QFINDTESTDATA("data/library-0.1.0.sql")).absolutePath());
        const QStringList dumps = data.entryList({"library-*.sql"}, QDir::Files, QDir::Name);
        QVERIFY(dumps.size() >= 5);
        for (const QString &dump : dumps) {
            SamplePairs pairs(data.filePath(dump), dump);
            for (const auto step : {readFurtherAndChangeDetailsBothSides, readFurtherThere, changeDetailsWithNoTime,
                                    makeAndRemoveCollections, removeOnEachSide, learnOnEachSide})
                if (!QTest::currentTestFailed())
                    step(pairs);
            QVERIFY(!QTest::currentTestFailed());

            // Never by the manifest's commands between two that speak the log.
            const QString logCommands = commandsOf(pairs.dir, "ssh-log");
            QVERIFY2(logCommands.contains("'--replicate'"), qPrintable(dump));
            QVERIFY2(!logCommands.contains("'--meta'") && !logCommands.contains("'--status'") && !logCommands.contains("'remove'")
                         && !logCommands.contains("'graph'") && !logCommands.contains("'collections'"),
                     qPrintable(dump));
            QVERIFY2(!commandsOf(pairs.dir, "ssh-older").contains("'--replicate'"), qPrintable(dump));
        }
    }
};

QTEST_MAIN(ReplicationTests)
#include "replication_tests.moc"
