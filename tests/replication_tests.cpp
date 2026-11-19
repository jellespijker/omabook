// Replicating the change log between computers (ADR 0001).

#include <QtTest>
#include <algorithm>

#include "hlc.h"
#include "spacereducer.h"

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
};

QTEST_MAIN(ReplicationTests)
#include "replication_tests.moc"
