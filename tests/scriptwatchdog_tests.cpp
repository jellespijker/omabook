#include <QJSEngine>
#include <QtTest>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "scriptwatchdog.h"
#include "threadclock.h"

namespace {

// A script's slow call: it waits and doesn't work.
class SlowCall : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE void wait(int milliseconds) const { std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds)); }
};

} // namespace

// The watchdog that stops a plugin's script counts the script's work, not the
// wall clock (#139).
class ScriptWatchdogTests : public QObject {
    Q_OBJECT

private slots:
    // On a machine whose every processor is busy, a script that waits on a
    // slow call is not stopped, and one that loops still is, after its limit
    // of work.
    void aScriptIsStoppedForItsWorkNotForWaiting() {
        std::vector<std::jthread> spinners;
        for (unsigned i = 0; i < std::max(2U, std::thread::hardware_concurrency()); ++i)
            spinners.emplace_back([](const std::stop_token &stop) {
                while (!stop.stop_requested()) {
                }
            });
        QJSEngine engine;
        SlowCall slow;
        engine.globalObject().setProperty("slow", engine.newQObject(&slow));
        std::atomic<bool> fired = false;
        ScriptWatchdog watchdog(&engine, [&fired] { fired = true; });
        const std::chrono::milliseconds limit(300);

        watchdog.arm(limit);
        const QJSValue waited = engine.evaluate("slow.wait(1200); 'done'");
        watchdog.disarm();
        QVERIFY(!fired);
        QCOMPARE(waited.toString(), QString("done"));

        const ThreadClock worked;
        watchdog.arm(limit);
        engine.evaluate("for (;;) {}");
        watchdog.disarm();
        const double spent = worked.milliseconds();
        spinners.clear();
        QVERIFY(fired);
        QVERIFY2(spent >= static_cast<double>(limit.count()), qPrintable(QString("stopped after %1 ms of work").arg(spent)));
        engine.setInterrupted(false);
    }
};

QTEST_GUILESS_MAIN(ScriptWatchdogTests)
#include "scriptwatchdog_tests.moc"
