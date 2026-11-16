// Spaces, global ids, the hybrid logical clock and the change log (ADR 0001).

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

#include "hlc.h"

class SpacesTests : public QObject {
    Q_OBJECT

private slots:
    void hlcIsMonotonicThroughBurstsAndBackwardClockJumps() {
        qint64 wall = 1794860000;
        Hlc clock(0, [&wall] { return wall; });

        const qint64 first = clock.tick();
        const qint64 second = clock.tick();
        const qint64 third = clock.tick();
        QVERIFY(first < second);
        QVERIFY(second < third);
        QCOMPARE(Hlc::seconds(first), wall);
        QCOMPARE(Hlc::counter(first), 0);
        QCOMPARE(Hlc::counter(second), 1);
        QCOMPARE(Hlc::counter(third), 2);

        // If the system clock jumps backwards, the logical clock never goes back.
        wall -= 300;
        const qint64 afterBackwardJump = clock.tick();
        QVERIFY(afterBackwardJump > third);
        QCOMPARE(Hlc::seconds(afterBackwardJump), qint64(1794860000));
        QCOMPARE(Hlc::counter(afterBackwardJump), 3);

        // Once the system clock catches up and passes the logical second,
        // the counter resets to 0 on the new second.
        wall = 1794860010;
        const qint64 caughtUp = clock.tick();
        QVERIFY(caughtUp > afterBackwardJump);
        QCOMPARE(Hlc::seconds(caughtUp), wall);
        QCOMPARE(Hlc::counter(caughtUp), 0);
    }

    void hlcMergesRemoteClocksAndRoundTripsThroughJson() {
        qint64 wall = 1794860000;
        Hlc local(0, [&wall] { return wall; });

        const qint64 t1 = local.tick();
        // A remote event from a clock 5 minutes ahead arrives: observing it
        // lifts our floor, and the next local tick orders after it.
        const qint64 remoteAhead = Hlc::pack(wall + 300, 7);
        local.observe(remoteAhead);
        const qint64 t2 = local.tick();
        QVERIFY(t2 > remoteAhead);
        QVERIFY(remoteAhead > t1);
        QCOMPARE(Hlc::seconds(t2), wall + 300);
        QCOMPARE(Hlc::counter(t2), 8);

        // receive() both observes a remote clock and ticks immediately.
        const qint64 remoteFurther = Hlc::pack(wall + 600, 0);
        const qint64 t3 = local.receive(remoteFurther);
        QVERIFY(t3 > remoteFurther);
        QCOMPARE(Hlc::seconds(t3), wall + 600);
        QCOMPARE(Hlc::counter(t3), 1);

        // Plain Unix-second timestamps from older libraries normalize to
        // counter 0 on the same scale, while 0 stays 0 (undecided).
        QCOMPARE(Hlc::normalize(0), qint64(0));
        QCOMPARE(Hlc::seconds(0), qint64(0));
        QCOMPARE(Hlc::normalize(1794694143), Hlc::pack(1794694143, 0));
        QCOMPARE(Hlc::seconds(1794694143), qint64(1794694143));
        QCOMPARE(Hlc::seconds(t3), wall + 600);
        QVERIFY(Hlc::compare(1794694143, t3) < 0);

        // Packed values fit in 47 bits and round-trip through JSON numbers.
        const QByteArray json = QJsonDocument(QJsonObject{{"hlc", t3}}).toJson(QJsonDocument::Compact);
        const qint64 parsed = QJsonDocument::fromJson(json).object().value("hlc").toInteger();
        QCOMPARE(parsed, t3);
    }

    void hlcSurvivesTwoComputersWithClocksMinutesApart() {
        // Laptop's wall clock is 10 minutes ahead of desktop's.
        qint64 laptopWall = 1794860600;
        qint64 desktopWall = 1794860000;
        Hlc laptop(0, [&laptopWall] { return laptopWall; });
        Hlc desktop(0, [&desktopWall] { return desktopWall; });

        // 1. Laptop edits first.
        const qint64 step1 = laptop.tick();
        // 2. Desktop syncs (observes laptop's timestamp) and then edits while
        //    its own wall clock is still 10 minutes behind laptop's.
        desktop.observe(step1);
        const qint64 step2 = desktop.tick();
        // 3. Laptop syncs (observes desktop's edit) and edits again a second later.
        ++laptopWall;
        laptop.observe(step2);
        const qint64 step3 = laptop.tick();
        // 4. Desktop syncs and edits once more, still minutes behind.
        ++desktopWall;
        desktop.observe(step3);
        const qint64 step4 = desktop.tick();

        QVERIFY(step1 < step2);
        QVERIFY(step2 < step3);
        QVERIFY(step3 < step4);
    }
};

QTEST_MAIN(SpacesTests)
#include "spaces_tests.moc"
