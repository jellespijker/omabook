#include <QDir>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <array>
#include <atomic>
#include <latch>
#include <thread>
#include <vector>

#include "library.h"
#include "librarymigrations.h"

// Opening one library folder from several places at once (#11): the window,
// a command and a worker of the window can all open it at the same moment,
// also the first time, when there is no database yet, and the first time
// after an update, when it needs bringing up to date.
class OpeningTests : public QObject {
    Q_OBJECT

    static constexpr int openers = 4;

    struct Opened {
        int failed = 0;
        QString why;
    };

    // Opens the library at `root` from `openers` threads at once, each with
    // its own connection, as separate programs would.
    [[nodiscard]] static Opened openAtOnce(const QString &root) {
        std::latch start{openers};
        std::array<QString, openers> errors;
        {
            std::vector<std::jthread> threads;
            for (QString &error : errors)
                threads.emplace_back([&start, &root, &error] {
                    Library library(root);
                    start.arrive_and_wait();
                    if (QString why; !library.open(&why))
                        error = why.isEmpty() ? QStringLiteral("failed without saying why") : why;
                });
        }
        Opened opened;
        for (const QString &error : errors) {
            if (!error.isEmpty()) {
                ++opened.failed;
                opened.why = error;
            }
        }
        return opened;
    }

    // Runs `statement` on the library's database, as another program would,
    // with the first value it answers (or -1).
    [[nodiscard]] static int run(const QString &root, const QString &statement) {
        int value = -1;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("outside"));
            database.setDatabaseName(root + QStringLiteral("/omabook.db"));
            if (database.open()) {
                QSqlQuery query(database);
                if (query.exec(statement) && query.next())
                    value = query.value(0).toInt();
                else if (query.isActive())
                    value = 0;
            }
            database.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("outside"));
        return value;
    }

    [[nodiscard]] static int personalSpaces(const QString &root) {
        return run(root, QStringLiteral("SELECT COUNT(*) FROM spaces WHERE kind = 'personal'"));
    }

    // Counts the times a library is brought to a newer format, as the log
    // says, wherever in the program it is.
    static inline std::atomic<int> s_migrations{0};
    static inline QtMessageHandler s_previous = nullptr;
    static void countMigrations(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        if (message.contains(QStringLiteral("bringing")) && message.contains(QStringLiteral("from format")))
            ++s_migrations;
        s_previous(type, context, message);
    }

private slots:
    void manyAtOnceMakeOneNewLibrary() {
        for (int round = 0; round < 25; ++round) {
            const QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString root = dir.filePath(QStringLiteral("Library"));
            const Opened opened = openAtOnce(root);
            QVERIFY2(opened.failed == 0, qPrintable(QStringLiteral("round %1: %2").arg(round).arg(opened.why)));
            // One library came of it, set up once: in the newest format, with one personal space.
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
            QCOMPARE(personalSpaces(root), 1);
        }
    }

    void twoCommandsAtOnceMakeOneNewLibrary() {
        for (int round = 0; round < 8; ++round) {
            const QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString root = dir.filePath(QStringLiteral("Library"));
            std::array<QProcess, 2> commands;
            for (QProcess &command : commands)
                command.start(QStringLiteral(OMABOOK_BINARY), {QStringLiteral("list"), QStringLiteral("--library"), root});
            // Both are waited for before either is judged, so none is left running.
            for (QProcess &command : commands)
                command.waitForFinished(30'000);
            for (QProcess &command : commands) {
                QVERIFY2(command.exitStatus() == QProcess::NormalExit && command.exitCode() == 0,
                         qPrintable(QStringLiteral("round %1: %2").arg(round).arg(QString::fromUtf8(command.readAllStandardError()))));
            }
            QCOMPARE(personalSpaces(root), 1);
        }
    }

    void manyAtOnceBringAnOldLibraryUpToDateOnce() {
        const int older = LibraryMigrations::currentFormat() - 1;
        s_previous = qInstallMessageHandler(countMigrations);
        const auto restore = qScopeGuard([] { qInstallMessageHandler(s_previous); });
        for (int round = 0; round < 10; ++round) {
            const QTemporaryDir dir;
            QVERIFY(dir.isValid());
            const QString root = dir.filePath(QStringLiteral("Library"));
            {
                Library library(root);
                QVERIFY(library.open());
            }
            // The newest step can be taken again, so a second time would not
            // fail: only the count of times shows it.
            QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = %1").arg(older)), 0);
            s_migrations = 0;
            const Opened opened = openAtOnce(root);
            QVERIFY2(opened.failed == 0, qPrintable(QStringLiteral("round %1: %2").arg(round).arg(opened.why)));
            QCOMPARE(s_migrations.load(), 1);
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
            QCOMPARE(QDir(library.backupFolder()).entryList({QStringLiteral("omabook-format-%1-*.db").arg(older)}).size(), 1);
        }
    }
};

QTEST_GUILESS_MAIN(OpeningTests)
#include "opening_tests.moc"
