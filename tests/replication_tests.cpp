// Replicating the change log between computers, and syncing with an Omabook
// from before it (ADR 0001, Migration 3, 4 and 7).

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <tuple>

#include "fixtures.h"
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
                return QString("%1[%2]: %3 where %4").arg(key).arg(i).arg(text(one), text(other));
        }
    }
    return {};
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

        // Asked for the log, it refused; it was asked for its manifest and
        // given the place by the command it has always taken, as plain seconds.
        const QString commands = commandsOf(dir, "ssh-older");
        QVERIFY2(commands.contains("'manifest' '--log'") && commands.contains("'manifest' --library"), qPrintable(commands));
        QVERIFY2(commands.contains("'progress'") && commands.contains("'--updated' '1794860500'"), qPrintable(commands));
        QVERIFY2(commands.contains("'--status' 'read'"), qPrintable(commands));
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
    // them, but not this side's; through the log, the side that takes them.
    void everySampleSyncsAlikeTheOldWayAndThroughTheLog() {
        const QDir data(QFileInfo(QFINDTESTDATA("data/library-0.1.0.sql")).absolutePath());
        const QStringList dumps = data.entryList({"library-*.sql"}, QDir::Files, QDir::Name);
        QVERIFY(dumps.size() >= 5);
        for (const QString &dump : dumps) {
            QTemporaryDir dir;
            int format = 0;
            for (const char *copy : {"OldHere", "OldThere", "LogHere", "LogThere"}) {
                QVERIFY(QDir(dir.path()).mkpath(copy));
                Samples::loadSqlDump(data.filePath(dump), dir.filePath(QString(copy) + "/omabook.db"), format);
            }
            Library oldHere(dir.filePath("OldHere")), oldThere(dir.filePath("OldThere"));
            Library logHere(dir.filePath("LogHere")), logThere(dir.filePath("LogThere"));
            QVERIFY(oldHere.open() && oldThere.open() && logHere.open() && logThere.open());
            const Remote older("laptop:" + dir.filePath("OldThere"), {recordingSsh(dir, "ssh-older", true)});
            const Remote current("laptop:" + dir.filePath("LogThere"), {recordingSsh(dir, "ssh-log", false)});
            const QJsonValue days = Samples::librarySnapshot(logHere).value("days");
            const auto expectAlike = [&](const char *step) {
                const QString at = dump + ", step " + step + ", ";
                const QString here = firstDifference(withoutMadeNodes(without(Samples::librarySnapshot(logHere), "days")),
                                                     withoutMadeNodes(without(Samples::librarySnapshot(oldHere), "days")));
                const QString there = firstDifference(without(Samples::librarySnapshot(logThere), "days"),
                                                      without(Samples::librarySnapshot(oldThere), "days"));
                QVERIFY2(here.isEmpty(), qPrintable(at + "here: " + here));
                QVERIFY2(there.isEmpty(), qPrintable(at + "there: " + there));
                QCOMPARE(Samples::librarySnapshot(logHere).value("days"), days);
                QCOMPARE(Samples::librarySnapshot(logThere).value("days"), days);
            };

            // 1. Read further here, and the details changed both sides.
            for (Library *library : {&oldHere, &logHere})
                readOn(*library, {.section = 2, .fraction = 0.25, .percent = 70, .updated = 1794700000}, Hlc::pack(1794700100, 0),
                       "Here Press");
            for (Library *library : {&oldThere, &logThere})
                readOn(*library, {.section = 1, .fraction = 0.9, .percent = 65, .updated = 1794699000}, Hlc::pack(1794700200, 0),
                       "There Press");
            const SyncSummary oldFirst = syncLibrary(oldHere, older);
            const SyncSummary logFirst = syncLibrary(logHere, current);
            QVERIFY2(oldFirst.problems.isEmpty() && logFirst.problems.isEmpty(),
                     qPrintable(dump + ": " + (oldFirst.problems + logFirst.problems).join('\n')));
            QCOMPARE(logFirst.placesSent, oldFirst.placesSent);
            QCOMPARE(logFirst.placesTaken, oldFirst.placesTaken);
            QCOMPARE(logFirst.detailsTaken + logFirst.detailsSent, oldFirst.detailsTaken + oldFirst.detailsSent);
            expectAlike("1");
            QCOMPARE(logThere.progress(1).updated, qint64(1794700000));
            QCOMPARE(Samples::librarySnapshot(oldThere).value("days").toArray().size(), days.toArray().size() + 1);

            // 2. Then read further there.
            for (Library *library : {&oldThere, &logThere})
                library->setProgress(1, {.section = 3, .fraction = 0.1, .percent = 80, .updated = 1794710000});
            const SyncSummary oldSecond = syncLibrary(oldHere, older);
            const SyncSummary logSecond = syncLibrary(logHere, current);
            QCOMPARE(logSecond.placesTaken, oldSecond.placesTaken);
            QCOMPARE(logSecond.placesSent, oldSecond.placesSent);
            expectAlike("2");
            QCOMPARE(logHere.progress(1).section, 3);

            // 3. Details changed both sides with no time to say which are
            //    newer, as before times were kept: left as they are, and
            //    counted, until the other side's are taken.
            for (Library *library : {&oldHere, &logHere, &oldThere, &logThere}) {
                BookMeta meta = library->book(2)->meta;
                meta.publisher = library == &oldHere || library == &logHere ? "Here Books" : "There Books";
                QVERIFY(library->setMeta(2, meta, nullptr, 0));
            }
            const SyncSummary oldUndecided = syncLibrary(oldHere, older);
            const SyncSummary logUndecided = syncLibrary(logHere, current);
            QCOMPARE(oldUndecided.detailsUndecided, 1);
            QCOMPARE(logUndecided.detailsUndecided, oldUndecided.detailsUndecided);
            QCOMPARE(logHere.book(2)->meta.publisher, QString("Here Books"));
            QCOMPARE(logThere.book(2)->meta.publisher, QString("There Books"));
            expectAlike("3");
            const SyncSummary oldSettled = syncLibrary(oldHere, older, {}, SyncOptions{.details = SyncOptions::Details::Theirs});
            const SyncSummary logSettled = syncLibrary(logHere, current, {}, SyncOptions{.details = SyncOptions::Details::Theirs});
            QCOMPARE(logSettled.detailsTaken, oldSettled.detailsTaken);
            QCOMPARE(logHere.book(2)->meta.publisher, QString("There Books"));
            expectAlike("3, settled");
            QCOMPARE(syncLibrary(logHere, current).detailsUndecided, 0);

            // Never by the manifest's commands between two that speak the log.
            const QString logCommands = commandsOf(dir, "ssh-log");
            QVERIFY2(logCommands.contains("'--replicate'"), qPrintable(dump));
            QVERIFY2(!logCommands.contains("'--meta'") && !logCommands.contains("'--status'"), qPrintable(dump));
            QVERIFY2(!commandsOf(dir, "ssh-older").contains("'--replicate'"), qPrintable(dump));
        }
    }
};

QTEST_MAIN(ReplicationTests)
#include "replication_tests.moc"
