// A sync of a large change log (#103): it reads only what the other side
// lacks, by its version vector, and the log a pairing brings in is compacted
// at once, of what both computers have seen (ADR 0001, *Compaction*).

#include <QDir>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <limits>
#include <utility>

#include "changelog.h"
#include "compaction.h"
#include "fixtures.h"
#include "graph.h"
#include "library.h"
#include "remote.h"
#include "replication.h"
#include "samples.h"
#include "spaces.h"
#include "sync.h"

namespace {

// The first value `sql` answers of a library, or -1.
qint64 answer(const Library &library, const QString &sql) {
    QSqlQuery query = ChangeLog::query(library, sql);
    return query.next() ? query.value(0).toLongLong() : -1;
}

// The best of three runs of `run`, in milliseconds.
template <typename Run> double bestOfThree(const Run &run) {
    double best = std::numeric_limits<double>::max();
    for (int time = 0; time < 3; ++time) {
        QElapsedTimer timer;
        timer.start();
        run();
        best = std::min(best, static_cast<double>(timer.nsecsElapsed()) / 1e6);
    }
    return best;
}

// The first place two library snapshots differ, by section, or nothing.
QString firstDifference(const QJsonObject &actual, const QJsonObject &expected) {
    for (const QString &key : expected.keys() + actual.keys())
        if (actual.value(key) != expected.value(key))
            return key + ": " + QString::fromUtf8(QJsonDocument(actual.value(key).toArray()).toJson(QJsonDocument::Compact)).left(300);
    return {};
}

constexpr qint64 inSnapshot = 60000;
constexpr qint64 inOps = 30000;

// A log of `inSnapshot` operations compacted into the snapshot (as a
// baseline is written) and `inOps` written after it, of collections a
// thousand at a time, all of which travel.
void writeLargeLog(const Library &library) {
    const Library::Batch batch(library);
    const qint64 space = ChangeLog::internId(library, Spaces::personalSpace(library));
    const qint64 device = ChangeLog::internId(library, Spaces::deviceId(library));
    for (qint64 seq = 1; seq <= inSnapshot; ++seq)
        Compaction::holdInSnapshot(library, {.space = space,
                                             .device = device,
                                             .seq = seq,
                                             .hlc = seq,
                                             .entity = "collection",
                                             .id = ChangeLog::internId(library, QString("c:%1").arg(seq / 20)),
                                             .field = QString("note:%1").arg(seq % 20),
                                             .value = QString("Said %1").arg(seq)});
    Compaction::markHeld(library);
    for (qint64 written = 0; written < inOps; ++written)
        ChangeLog::append(library, {.space = {},
                                    .entity = "collection",
                                    .id = QString("c:later-%1").arg(written / 20),
                                    .field = QString("note:%1").arg(written % 20),
                                    .value = QString("Said later %1").arg(written),
                                    .hlc = std::nullopt});
}

} // namespace

class LargeLogTests : public QObject {
    Q_OBJECT

private slots:
    // A peer that holds all but the last of a large log is sent only what it
    // lacks, read so in SQL, a range of the log by space, device and number:
    // far quicker than reading it all, as a peer with nothing of it is sent.
    // One behind the snapshot's mark is sent the snapshot's operations after
    // its number too, and what it says it holds is read without reading it all.
    void aSyncReadsOnlyWhatThePeerLacksOfALargeLog() {
        QTemporaryDir dir;
        Library library(dir.filePath("Large"));
        QVERIFY(library.open());
        writeLargeLog(library);
        const QString space = Spaces::personalSpace(library);
        const QString device = Spaces::deviceId(library);
        const qint64 last = inSnapshot + inOps;

        const QList<SpaceOperation> all = Replication::operations(library);
        QCOMPARE(qint64(all.size()), last);
        QCOMPARE(qint64(Replication::since(library, {}).size()), last);
        const Replication::Vector held = Replication::vector(library);
        QCOMPARE(held.value(space).value(device), last);

        // A hundredth behind: what is after its number, and nothing else.
        Replication::Vector behind = held;
        behind[space][device] = last - (last / 100);
        const QList<SpaceOperation> lacking = Replication::since(library, behind);
        QCOMPARE(lacking, all.mid(all.size() - (last / 100)));
        QCOMPARE(Replication::since(library, behind, behind), lacking);
        QCOMPARE(qint64(Replication::since(library, held).size()), qint64(0));
        // Behind the mark: the snapshot after its number, then all of `ops`.
        Replication::Vector beforeMark = held;
        beforeMark[space][device] = inSnapshot - 100;
        QCOMPARE(Replication::since(library, beforeMark), all.mid(inSnapshot - 100));
        // Of another space or device it knows, nothing it lacks.
        Replication::Vector others = held;
        others["personal:someone-else"]["their-device"] = 5;
        QCOMPARE(qint64(Replication::since(library, others).size()), qint64(0));

        const double whole = bestOfThree([&] { QCOMPARE(qint64(Replication::since(library, {}).size()), last); });
        const double near = bestOfThree([&] { QCOMPARE(qint64(Replication::since(library, behind).size()), last / 100); });
        const double vector = bestOfThree([&] { QCOMPARE(Replication::vector(library), held); });
        qInfo().noquote() << QString("a log of %1 operations: all of it read in %2 ms, the last hundredth in %3 ms, its vector in %4 ms")
                                 .arg(last)
                                 .arg(whole, 0, 'f', 1)
                                 .arg(near, 0, 'f', 1)
                                 .arg(vector, 0, 'f', 1);
        QVERIFY2(near * 10 < whole, qPrintable(QString("the last hundredth took %1 ms of %2 for all").arg(near).arg(whole)));
        QVERIFY2(vector * 10 < whole, qPrintable(QString("the vector took %1 ms of %2 for all").arg(vector).arg(whole)));
        QVERIFY2(near < 500, qPrintable(QString("the last hundredth took %1 ms").arg(near)));
    }

    // A computer pairing with another brings in its whole log at once: its
    // snapshot (the other compacts before it answers) and what came after.
    // That is compacted then, in the same sync, of what both have seen,
    // though it is shorter than the snapshot here, and the two still end
    // alike and sync on.
    void aLogBroughtInByPairingIsCompactedOfWhatBothHaveSeen() {
        QTemporaryDir dir;
        int format = 0;
        for (const char *copy : {"Here", "There"}) {
            QVERIFY(QDir(dir.path()).mkpath(copy));
            Samples::loadSqlDump(QFINDTESTDATA("data/library-0.1.5.sql"), dir.filePath(QString(copy) + "/omabook.db"), format);
        }
        Library here(dir.filePath("Here"));
        Library there(dir.filePath("There"));
        QVERIFY(here.open() && there.open());
        // Each learned much, Here more, and read on There; Here's is compacted, so its snapshot is large.
        for (const auto &[library, things] : {std::pair{&here, 800}, std::pair{&there, 300}}) {
            Graph graph(*library);
            for (int thing = 0; thing < things; ++thing)
                QVERIFY(graph.upsert({}, "character", QString("%1 %2").arg(library == &here ? "Here" : "There").arg(thing),
                                     QString("Met in round %1.").arg(thing), {}, GraphPlace{1, thing % 7, 0.5},
                                     QString("Thing %1").arg(thing))
                        > 0);
        }
        Compaction::compact(here);
        for (int round = 0; round < 200; ++round)
            there.setProgress(1, {.section = round % 30, .fraction = 0.5, .percent = round / 2.0, .updated = 1794730000 + round});
        const qint64 snapshotHere = answer(here, "SELECT COUNT(*) FROM snapshot_ops");

        const Remote remote("laptop:" + dir.filePath("There"), {Fixtures::standInSsh(dir.path(), OMABOOK_BINARY)});
        const SyncSummary paired = syncLibrary(here, remote);
        QVERIFY2(paired.error.isEmpty() && paired.problems.isEmpty(), qPrintable(paired.error + paired.problems.join('\n')));
        QCOMPARE(here.progress(1).updated, qint64(1794730000 + 199));
        const QString ofTheirs = QString("(SELECT id FROM log_ids WHERE name = '%1')").arg(Spaces::deviceId(there));
        const qint64 theirs = answer(here, "SELECT COUNT(*) FROM space_ops WHERE device = " + ofTheirs);
        const qint64 left = answer(here, "SELECT COUNT(*) FROM ops WHERE device = " + ofTheirs);
        qInfo().noquote() << QString("pairing brought in %1 operations, %2 held here; %3 of them left in ops after the sync")
                                 .arg(theirs)
                                 .arg(snapshotHere)
                                 .arg(left);
        // Too few to compact before (fewer than the snapshot here), more than a thousand of a device with no mark.
        QVERIFY2(theirs > 1000 && theirs < snapshotHere, qPrintable(QString("%1 of theirs, %2 here").arg(theirs).arg(snapshotHere)));
        QVERIFY2(left < theirs / 10, qPrintable(QString("%1 of %2 still in ops").arg(left).arg(theirs)));
        // Of what the snapshot covers, only what There hasn't seen stays in `ops`: nothing.
        QCOMPARE(answer(here, QString("SELECT COUNT(*) FROM ops AS o JOIN snapshot_marks AS m ON m.space = o.space "
                                      "AND m.device = o.device WHERE o.seq <= m.seq AND o.seq <= COALESCE((SELECT v.seq "
                                      "FROM member_vectors AS v WHERE v.space = o.space AND v.device = o.device "
                                      "AND v.member = '%1'), 0)")
                                  .arg(Spaces::deviceId(there))),
                 qint64(0));
        QCOMPARE(firstDifference(Samples::librarySnapshot(there), Samples::librarySnapshot(here)), QString());
        QCOMPARE(qint64(Replication::since(here, Replication::vector(there)).size()), qint64(0));
        QCOMPARE(qint64(Replication::since(there, Replication::vector(here)).size()), qint64(0));

        // Syncing on, both ways, on top of the snapshot.
        there.setProgress(1, {.section = 3, .fraction = 0.1, .percent = 99, .updated = 1794740000});
        QVERIFY(Graph(here).upsert({}, "place", "The Lighthouse", {}, {}, GraphPlace{1, 3, 0.1}, "lighthouse") > 0);
        const SyncSummary on = syncLibrary(here, remote);
        QVERIFY2(on.problems.isEmpty(), qPrintable(on.problems.join('\n')));
        QCOMPARE(here.progress(1).percent, 99.0);
        QVERIFY(Graph(there).named({}, "place", "The Lighthouse") > 0);
        QCOMPARE(firstDifference(Samples::librarySnapshot(there), Samples::librarySnapshot(here)), QString());
        const SyncSummary settled = syncLibrary(here, remote);
        QCOMPARE(settled.received + settled.sent + settled.placesTaken + settled.placesSent + settled.graphTaken + settled.graphSent, 0);
    }
};

QTEST_MAIN(LargeLogTests)
#include "largelog_tests.moc"
