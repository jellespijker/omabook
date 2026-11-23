#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
#include <array>
#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>

#include "fixtures.h"
#include "graph.h"
#include "library.h"
#include "librarymigrations.h"
#include "readinglog.h"

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

    // The first value `statement` answers on the library's database, as text.
    [[nodiscard]] static QString textOf(const QString &root, const QString &statement) {
        QString value;
        {
            QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("outside"));
            database.setDatabaseName(root + QStringLiteral("/omabook.db"));
            if (database.open()) {
                QSqlQuery query(database);
                if (query.exec(statement) && query.next())
                    value = query.value(0).toString();
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
    // The last time a library was brought to a newer format, as the log says it.
    static inline QString s_brought;
    static void keepMigration(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        if (message.contains(QStringLiteral("bringing")) && message.contains(QStringLiteral("from format")))
            s_brought = message;
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

    // Opened again on the thread that is setting it up, as a plugin that
    // opens the library as it loads would, loaded while it is (#60): that
    // opening doesn't wait for the lock this thread holds, under whatever
    // name of the folder, and the setting up goes on.
    void aLibraryOpenedWhileThisThreadSetsItUpIsNotWaitedFor() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("Library"));
        {
            Library library(root);
            QVERIFY(library.open());
        }
        QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = %1").arg(LibraryMigrations::currentFormat() - 1)), 0);
        Library library(root);
        bool openedAgain = true;
        QString why;
        qint64 waited = -1;
        library.onBringingUpToDate([&](const QString &) {
            QElapsedTimer timer;
            timer.start();
            Library again(dir.filePath(QStringLiteral("Library/../Library")));
            openedAgain = again.open(&why);
            waited = timer.elapsed();
        });
        QVERIFY(library.open());
        QVERIFY2(waited >= 0 && waited < 5000, qPrintable(QStringLiteral("waited %1 ms").arg(waited)));
        QVERIFY(!openedAgain);
        QVERIFY2(!why.contains(QStringLiteral("another program")), qPrintable(why));
        QCOMPARE(library.formatVersion(), LibraryMigrations::currentFormat());
        // Once it is set up, it opens.
        Library after(root);
        QVERIFY(after.open());
    }

    // A library of 0.1.8.1, at format 32 (#112's step), takes the steps that
    // came after it on main (#76's, format 33, #110's, 34, #80's, 35, #81's, 36,
    // #118's, 37, #115's, 38, #68's, 39, #72's, 40, #74's, 41, #136's, 42,
    // #158's, 43, #164's, 44, #213's, 45, and #216's, 46) and not its own again: the steps are
    // positional, so #112's is 32 on main too.
    void aLibraryOf0181TakesTheStepsAfterItsOwn() {
        QCOMPARE(LibraryMigrations::currentFormat(), 46);
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("Library"));
        {
            Library library(root);
            QVERIFY(library.open());
        }
        // As 0.1.8.1 left it: the rows made of every space but the published
        // and shared ones (no plugin's left out yet), and no index of names.
        const QString view = textOf(root, QStringLiteral("SELECT sql FROM sqlite_master WHERE name = 'live_ops'"));
        const QString plugins = QStringLiteral(" OR (name >= 'plugin:' AND name < 'plugin;')");
        QVERIFY(view.contains(plugins));
        QCOMPARE(run(root, QStringLiteral("DROP VIEW live_ops")), 0);
        QCOMPARE(run(root, QString(view).replace(plugins, QString())), 0);
        QCOMPARE(run(root, QStringLiteral("DROP INDEX graph_facts_names")), 0);
        QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = 32")), 0);

        s_brought.clear();
        s_previous = qInstallMessageHandler(keepMigration);
        {
            const auto restore = qScopeGuard([] { qInstallMessageHandler(s_previous); });
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), 46);
            QCOMPARE(QDir(library.backupFolder()).entryList({QStringLiteral("omabook-format-32-*.db")}).size(), 1);
        }
        QVERIFY2(s_brought.contains(QStringLiteral("from format 32 to 46")), qPrintable(s_brought));
        QVERIFY(textOf(root, QStringLiteral("SELECT sql FROM sqlite_master WHERE name = 'live_ops'")).contains(plugins));
        QCOMPARE(run(root, QStringLiteral("SELECT COUNT(*) FROM sqlite_master WHERE name = 'graph_facts_names'")), 1);
    }

    // A library of format 35, its highlights without a colour, takes #81's
    // step: they keep their numbers and passages and are the first colour;
    // and the step can be taken again, by a library put back to 35.
    void aHighlightOfFormat35IsTheFirstColour() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("Library"));
        QVERIFY(Fixtures::epub(dir.filePath(QStringLiteral("Book.epub"))));
        qint64 id = 0;
        {
            Library library(root);
            QVERIFY(library.open());
            const qint64 book = library.add(dir.filePath(QStringLiteral("Book.epub"))).id;
            QVERIFY(book > 0);
            Annotation highlight;
            highlight.kind = QStringLiteral("highlight");
            highlight.quote = QStringLiteral("the ferry came back");
            highlight.color = QStringLiteral("blue");
            id = library.addAnnotation(book, highlight);
            QVERIFY(id > 0);
        }
        for (const bool putBack : {false, true}) {
            if (!putBack)
                QCOMPARE(run(root, QStringLiteral("ALTER TABLE annotations DROP COLUMN color")), 0);
            QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = 35")), 0);
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), 46);
            QCOMPARE(textOf(root, QStringLiteral("SELECT quote FROM annotations WHERE id = %1").arg(id)),
                     QStringLiteral("the ferry came back"));
            QCOMPARE(textOf(root, QStringLiteral("SELECT color FROM annotations WHERE id = %1").arg(id)), QString());
        }
    }

    // A library of format 37 takes #115's step: its highlights and notes keep
    // their numbers, are on no part of a page, and their words are in the
    // word index, which follows what is written after; and the step can be
    // taken again, by a library put back to 37.
    void theNotesOfFormat37AreFoundByTheirWords() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("Library"));
        QVERIFY(Fixtures::epub(dir.filePath(QStringLiteral("Book.epub"))));
        qint64 book = 0, id = 0;
        {
            Library library(root);
            QVERIFY(library.open());
            book = library.add(dir.filePath(QStringLiteral("Book.epub"))).id;
            QVERIFY(book > 0);
            Annotation note;
            note.kind = QStringLiteral("margin-note");
            note.quote = QStringLiteral("the ferry came back");
            note.note = QStringLiteral("Lighthouse keepers");
            id = library.addAnnotation(book, note);
            QVERIFY(id > 0);
        }
        const auto found = [&root](const QString &word) {
            return run(root, QStringLiteral("SELECT COUNT(*) FROM annotation_words WHERE annotation_words MATCH '%1'").arg(word));
        };
        for (const bool putBack : {false, true}) {
            if (!putBack) {
                QCOMPARE(run(root, QStringLiteral("ALTER TABLE annotations DROP COLUMN area")), 0);
                QCOMPARE(run(root, QStringLiteral("DROP TABLE annotation_words")), 0);
            }
            QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = 37")), 0);
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), 46);
            QCOMPARE(textOf(root, QStringLiteral("SELECT note FROM annotations WHERE id = %1").arg(id)),
                     QStringLiteral("Lighthouse keepers"));
            QCOMPARE(textOf(root, QStringLiteral("SELECT area FROM annotations WHERE id = %1").arg(id)), QString());
            QCOMPARE(found(QStringLiteral("lighthouse")), 1);
            QCOMPARE(found(QStringLiteral("ferry")), 1);
        }
        Library library(root);
        QVERIFY(library.open());
        QVERIFY(library.renameAnnotation(id, QStringLiteral("Harbour")));
        QCOMPARE(found(QStringLiteral("harbour")), 1);
        library.removeAnnotation(id);
        QCOMPARE(found(QStringLiteral("ferry")), 0);
    }

    // A library of format 38 takes #68's step: its reading log keeps every
    // day, how far each went, and took no time yet; what is read after it
    // goes into the change log as before; and the step can be taken again,
    // by a library put back to 38 (which, as the other steps, keeps none of
    // what came with it).
    void aReadingLogOfFormat38KeepsItsDaysAndTakesTime() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("Library"));
        QVERIFY(Fixtures::epub(dir.filePath(QStringLiteral("Book.epub"))));
        qint64 book = 0;
        {
            Library library(root);
            QVERIFY(library.open());
            book = library.add(dir.filePath(QStringLiteral("Book.epub"))).id;
            QVERIFY(book > 0);
            Graph(library).logReading(book, 10, 40, QStringLiteral("2026-03-02"));
        }
        for (const bool putBack : {false, true}) {
            if (!putBack)
                QCOMPARE(run(root, QStringLiteral("ALTER TABLE reading_log DROP COLUMN seconds")), 0);
            QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = 38")), 0);
            Library library(root);
            QVERIFY(library.open());
            QCOMPARE(library.formatVersion(), 46);
            const QList<ReadingDay> days = Graph(library).readingLog(book);
            QCOMPARE(days.size(), 1);
            QCOMPARE(days.first().to, putBack ? 55.0 : 40.0);
            QCOMPARE(days.first().seconds, 0);
            ReadingLog::add(library, book, 40, 55, std::chrono::minutes{10}, QStringLiteral("2026-03-02"));
            QCOMPARE(Graph(library).readingLog(book).first().seconds, 600);
            QCOMPARE(Graph(library).readingLog(book).first().to, 55.0);
            QVERIFY(textOf(root, QStringLiteral("SELECT value FROM ops WHERE entity = 'reading_log' ORDER BY seq DESC LIMIT 1"))
                        .contains(QStringLiteral("\"seconds\"")));
        }
    }

    // A command that brings a library up to date says so on standard error
    // while it does (#54), once: the next one finds it up to date.
    void aCommandSaysWhenItBringsALibraryUpToDate() {
        const QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString root = dir.filePath(QStringLiteral("Library"));
        {
            Library library(root);
            QVERIFY(library.open());
        }
        QCOMPARE(run(root, QStringLiteral("PRAGMA user_version = %1").arg(LibraryMigrations::currentFormat() - 1)), 0);
        const auto said = [&root] {
            QProcess command;
            command.start(QStringLiteral(OMABOOK_BINARY), {QStringLiteral("list"), QStringLiteral("--library"), root});
            command.waitForFinished(30'000);
            return command.exitCode() == 0 ? QString::fromUtf8(command.readAllStandardError()) : QStringLiteral("failed");
        };
        const QString first = said();
        QVERIFY2(first.contains(QStringLiteral("Bringing your library up to date")), qPrintable(first));
        QCOMPARE(first.count(QStringLiteral("Bringing")), 1);
        const QString second = said();
        QVERIFY2(!second.contains(QStringLiteral("Bringing")) && second != QStringLiteral("failed"), qPrintable(second));
    }
};

QTEST_GUILESS_MAIN(OpeningTests)
#include "opening_tests.moc"
