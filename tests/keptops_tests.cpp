// Operations of a kind an Omabook doesn't know yet (#118): one older than
// the Omabook that wrote them keeps them in its log, shows nothing of them,
// sends them on and keeps them through compaction, and takes them in once
// it is brought up to date. An older Omabook is this one forgetting a kind
// (OpKinds::Forgetting), here, while the other computers run the omabook.

#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <optional>
#include <tuple>

#include "changelog.h"
#include "compaction.h"
#include "fixtures.h"
#include "keptops.h"
#include "library.h"
#include "opkinds.h"
#include "remote.h"
#include "replication.h"
#include "sync.h"

namespace {

// The first value `sql` answers of a library, or -1.
qint64 answer(const Library &library, const QString &sql) {
    QSqlQuery query = ChangeLog::query(library, sql);
    return query.next() ? query.value(0).toLongLong() : -1;
}

qint64 heldOf(const Library &library, const QString &entity) {
    return answer(library, QString("SELECT COUNT(*) FROM space_ops WHERE entity = '%1'").arg(entity));
}

// Here, the computer that is older (or brought up to date), with the same
// book as two others, "laptop" and "desktop", each synced once so all three
// are paired.
struct ThreeComputers {
    ThreeComputers() : here(dir.filePath("Here")), laptop(dir.filePath("Laptop")), desktop(dir.filePath("Desktop")) {
        QVERIFY(Fixtures::epub(dir.filePath("shared.epub"), "Kept For Later"));
        QVERIFY(here.open() && laptop.open() && desktop.open());
        mine = here.add(dir.filePath("shared.epub")).id;
        onLaptop = laptop.add(dir.filePath("shared.epub")).id;
        onDesktop = desktop.add(dir.filePath("shared.epub")).id;
        ssh = Fixtures::standInSsh(dir.path(), OMABOOK_BINARY);
        for (const Remote &remote : {toLaptop(), toDesktop()}) {
            const SyncSummary paired = syncLibrary(here, remote);
            QVERIFY2(paired.problems.isEmpty(), qPrintable(paired.problems.join('\n')));
        }
    }

    // Each command there within two minutes: one that hangs is a sync's problem, said, never a test that waits.
    [[nodiscard]] Remote toLaptop() const { return Remote("laptop:" + dir.filePath("Laptop"), {"timeout", "120", ssh}); }
    [[nodiscard]] Remote toDesktop() const { return Remote("desktop:" + dir.filePath("Desktop"), {"timeout", "120", ssh}); }

    QTemporaryDir dir;
    Library here, laptop, desktop;
    QString ssh;
    qint64 mine = 0, onLaptop = 0, onDesktop = 0;
};

} // namespace

class KeptOpsTests : public QObject {
    Q_OBJECT

private slots:
    // A bookmark written on the laptop and changed there, synced to an
    // Omabook that doesn't know bookmarks: kept, every one of its
    // operations, shown nowhere, kept whole by compaction, and sent on to the
    // desktop, which shows it. Brought up to date, this one shows it too.
    void aKindNotKnownIsKeptSentOnAndTakenInOnceKnown() {
        ThreeComputers three;
        const qint64 made = three.laptop.addAnnotation(
            three.onLaptop,
            {.id = 0, .kind = "bookmark", .place = {.section = 1, .fraction = 0.5}, .text = "The tide", .note = {}, .created = 0});
        QVERIFY(made > 0);
        QVERIFY(three.laptop.renameAnnotation(made, "The tide turns"));
        const qint64 written = heldOf(three.laptop, "annotation");
        QVERIFY(written >= 2);

        {
            const OpKinds::Forgetting older(QStringLiteral("annotation"));
            const SyncSummary taken = syncLibrary(three.here, three.toLaptop());
            QVERIFY2(taken.problems.isEmpty(), qPrintable(taken.problems.join('\n')));
            QCOMPARE(taken.annotationsTaken, 0);
            QVERIFY(three.here.annotations(three.mine).isEmpty());
            QCOMPARE(qint64(KeptOps::kept(three.here).size()), written);
            QCOMPARE(heldOf(three.here, "annotation"), written);
            // Held: nothing of it is sent here again.
            QCOMPARE(Replication::since(three.laptop, Replication::vector(three.here), Replication::annotationVector(three.here)).size(),
                     0);

            // Compacted, the first name is still there beside the second.
            std::ignore = Compaction::compact(three.here);
            QCOMPARE(heldOf(three.here, "annotation"), written);
            QCOMPARE(qint64(KeptOps::kept(three.here).size()), written);
            QVERIFY(three.here.annotations(three.mine).isEmpty());

            // Sent on, to a computer that knows bookmarks.
            const SyncSummary sent = syncLibrary(three.here, three.toDesktop());
            QVERIFY2(sent.problems.isEmpty(), qPrintable(sent.problems.join('\n')));
            const QList<Annotation> there = three.desktop.annotations(three.onDesktop);
            QCOMPARE(there.size(), 1);
            QCOMPARE(there.first().text, QStringLiteral("The tide turns"));
            QVERIFY(three.here.annotations(three.mine).isEmpty());
        }

        // Brought up to date: opened by an Omabook that knows bookmarks.
        Library upgraded(three.dir.filePath("Here"));
        QVERIFY(upgraded.open());
        const QList<Annotation> here = upgraded.annotations(three.mine);
        QCOMPARE(here.size(), 1);
        QCOMPARE(here.first().text, QStringLiteral("The tide turns"));
        QCOMPARE(here.first().place.section, 1);
        QVERIFY(KeptOps::kept(upgraded).isEmpty());
        // Taken in once: opened again, and synced, nothing changes.
        Library again(three.dir.filePath("Here"));
        QVERIFY(again.open());
        QCOMPARE(again.annotations(three.mine).size(), 1);
        const SyncSummary settled = syncLibrary(again, three.toLaptop());
        QCOMPARE(settled.annotationsTaken + settled.annotationsSent, 0);
    }

    // A field of a book this Omabook doesn't know (its stars, here) is kept
    // as a whole kind is, and the book's other details taken: brought up to
    // date, it takes the stars from the computer that gave them.
    void aFieldNotKnownIsKeptAndTakenInOnceKnown() {
        ThreeComputers three;
        QVERIFY(three.laptop.setReading(three.onLaptop, "reading", 4, {}));
        {
            const OpKinds::Forgetting older(QStringLiteral("book"), QStringLiteral("rating"));
            QVERIFY(syncLibrary(three.here, three.toLaptop()).problems.isEmpty());
            const std::optional<Book> book = three.here.book(three.mine);
            QVERIFY(book);
            QCOMPARE(book->rating, 0);
            QCOMPARE(book->shelf, QStringLiteral("reading"));
            QVERIFY(!KeptOps::kept(three.here).isEmpty());
            for (const SpaceOperation &op : KeptOps::kept(three.here))
                QCOMPARE(op.field, QStringLiteral("rating"));
        }
        Library upgraded(three.dir.filePath("Here"));
        QVERIFY(upgraded.open());
        const std::optional<Book> book = upgraded.book(three.mine);
        QVERIFY(book);
        QCOMPARE(book->rating, 4);
        QVERIFY(KeptOps::kept(upgraded).isEmpty());
    }

    // What an Omabook knows no kind of at all, a newer one's (a badge for
    // a year's reading, say), is taken from what a peer sends, to be kept, never left out;
    // what is only another computer's own, or a shared space's, still is.
    void anOperationOfAnUnknownEntityIsTakenFromWhatIsSent() {
        const SpaceOperation badge{.space = "personal:p",
                                   .device = "d",
                                   .seq = 3,
                                   .hlc = 1,
                                   .author = "d",
                                   .entity = "badge",
                                   .id = "b:2026",
                                   .field = "books",
                                   .value = "12"};
        SpaceOperation path = badge;
        path.entity = "book";
        path.field = "path";
        SpaceOperation shared = badge;
        shared.space = "shared:s";
        const QList<SpaceOperation> sent{badge, path, shared};
        const QList<SpaceOperation> taken = Replication::operationsFrom(Replication::toJson(sent));
        QCOMPARE(taken.size(), 1);
        QCOMPARE(taken.first().entity, QStringLiteral("badge"));
        QVERIFY(!OpKinds::known(badge));
        QVERIFY(OpKinds::known(path));
    }
};

QTEST_MAIN(KeptOpsTests)
#include "keptops_tests.moc"
