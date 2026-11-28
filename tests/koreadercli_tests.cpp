#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

// `omabook koreader` and `omabook serve --koreader` on a computer with no
// keyring (#274): a password that would be gone when the command ends is not
// said to be kept. Everything is made up, in a temporary folder, and no
// session bus is reachable.

namespace {

struct World {
    QTemporaryDir dir;

    void start(QProcess &process, const QStringList &arguments) const {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert("HOME", dir.filePath("home"));
        for (const char *name : {"XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME", "XDG_RUNTIME_DIR"})
            environment.insert(name, dir.filePath(QString("home/") + name));
        environment.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + dir.filePath("no-bus"));
        process.setProcessEnvironment(environment);
        process.start(OMABOOK_BINARY, QStringList(arguments) << "--library" << dir.filePath("Library"));
    }
};

} // namespace

class KoreaderCliTests : public QObject {
    Q_OBJECT

private slots:
    void aPasswordThatCantBeKeptIsRefused_data() {
        QTest::addColumn<QStringList>("command");
        QTest::newRow("serve-as") << QStringList{"koreader", "serve-as", "reader"};
        QTest::newRow("login") << QStringList{"koreader", "login", "http://127.0.0.1:9", "reader"};
    }
    void aPasswordThatCantBeKeptIsRefused() {
        QFETCH(QStringList, command);
        const World world;
        QProcess process;
        world.start(process, command);
        process.write("secretpw\n");
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(60000));
        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        const QString errors = QString::fromUtf8(process.readAllStandardError());
        QCOMPARE(process.exitCode(), 1);
        QVERIFY2(errors.contains("keyring") && errors.contains("Nothing was kept."), qPrintable(errors));
        QVERIFY2(output.isEmpty(), qPrintable(output));
    }

    void servingSaysThatKoreaderHasNoPassword() {
        const World world;
        QProcess process;
        world.start(process, {"serve", "--reach", "localhost", "--port", "47274", "--koreader"});
        QString errors;
        const auto said = [&] {
            errors += QString::fromUtf8(process.readAllStandardError());
            return errors.contains("KOReader's sync has no password");
        };
        QTRY_VERIFY2_WITH_TIMEOUT(said(), qPrintable(errors), 30000);
        process.terminate();
        QVERIFY(process.waitForFinished(10000));
        const QString output = QString::fromUtf8(process.readAllStandardOutput());
        QVERIFY2(!output.contains("logging in as"), qPrintable(output));
    }
};

QTEST_GUILESS_MAIN(KoreaderCliTests)
#include "koreadercli_tests.moc"
