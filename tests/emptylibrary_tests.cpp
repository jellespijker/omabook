// A library folder given as nothing (`--library "$UNSET"`) is refused, and
// never read as the folder the command runs in (#234).

#include <QDir>
#include <QProcess>
#include <QtTest>

#include "library.h"
#include "mcpharness.h"

class EmptyLibraryTests : public QObject {
    Q_OBJECT

private slots:
    void anEmptyFolderIsRefused_data() {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("a command") << QStringList{"list", "--library", ""};
        QTest::newRow("before the command") << QStringList{"--library", "", "list"};
        QTest::newRow("with =") << QStringList{"list", "--library="};
        QTest::newRow("the window") << QStringList{"open", "--library", ""};
        QTest::newRow("the MCP server") << QStringList{"mcp", "--library", ""};
        QTest::newRow("the one to use") << QStringList{"libraries", "--use", "", "--create"};
    }

    void anEmptyFolderIsRefused() {
        QFETCH(QStringList, arguments);
        const McpHarness::Machine machine;
        const QString here = machine.dir.filePath("here");
        QVERIFY(QDir().mkpath(here));
        QProcess process;
        process.setProcessEnvironment(machine.environment());
        process.setWorkingDirectory(here);
        process.start(OMABOOK_BINARY, arguments);
        process.closeWriteChannel();
        QVERIFY2(process.waitForFinished(30000), "it went on running");
        QCOMPARE(process.exitCode(), 1);
        const QString said = QString::fromUtf8(process.readAllStandardError());
        QVERIFY2(said.contains("needs a folder"), qPrintable(said));
        QCOMPARE(QDir(here).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), QStringList());
    }

    // Nor does anything below the commands open the current folder for it.
    void noLibraryIsOpenedWithoutAFolder() {
        QTemporaryDir here;
        const QString before = QDir::currentPath();
        QVERIFY(QDir::setCurrent(here.path()));
        const bool opened = Library(QString()).open();
        const bool made = QFileInfo::exists(here.filePath("omabook.db"));
        QVERIFY(QDir::setCurrent(before));
        QVERIFY(!opened);
        QVERIFY(!made);
    }
};

QTEST_MAIN(EmptyLibraryTests)
#include "emptylibrary_tests.moc"
