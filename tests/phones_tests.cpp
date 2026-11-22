// Serving phones and e-readers, and who does it (#177): a server says which
// Omabook it is (the window, its background service, omabook serve in a
// terminal) to this computer, and how many devices asked it lately; a port
// that is taken says what took it, an Omabook by name and another program by
// its own; the command line uses the window serving already instead of
// failing over the port; and the window writes and starts its background
// service (with a stand-in systemctl), leaves serving to it, restarts it as
// the settings change, and serves again itself once it is gone.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>
#include <chrono>

#include "devicedesk.h"
#include "deviceoptions.h"
#include "deviceserver.h"
#include "deviceserving.h"
#include "fakedevice.h"
#include "fixtures.h"
#include "portholder.h"
#include "secretstore.h"
#include "settings.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

const QHash<QByteArray, QByteArray> withToken{{"Authorization", "Basic " + QByteArray("kobo:token").toBase64()}};

[[nodiscard]] quint16 freePort() {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, 0) ? probe.serverPort() : 0;
}

[[nodiscard]] bool canListen(quint16 port) {
    QTcpServer probe;
    return probe.listen(QHostAddress::LocalHost, port);
}

[[nodiscard]] DeviceServer::Setup serving(quint16 port, DeviceServing::By by) {
    DeviceOptions options;
    options.reach = DeviceOptions::Reach::ThisComputer;
    options.port = port;
    options.opds = true;
    return {.options = options, .opdsToken = "token", .koreaderKey = {}, .by = by};
}

// A server on a thread of its own, as the window's is, or as another
// process's would be: what asks it may wait for its answer.
struct Elsewhere {
    QThread thread;
    DeviceServer *server = nullptr;

    explicit Elsewhere(const QString &root) {
        thread.start();
        server = new DeviceServer(root); // the thread's: deleted when it finishes
        server->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, server, &QObject::deleteLater);
    }
    ~Elsewhere() {
        stop();
        thread.quit();
        thread.wait();
    }
    Elsewhere(const Elsewhere &) = delete;
    Elsewhere &operator=(const Elsewhere &) = delete;

    std::expected<QString, Failure> start(const DeviceServer::Setup &setup) {
        std::expected<QString, Failure> result;
        QMetaObject::invokeMethod(server, [&] { result = server->start(setup); }, Qt::BlockingQueuedConnection);
        return result;
    }
    void stop() { QMetaObject::invokeMethod(server, &DeviceServer::stop, Qt::BlockingQueuedConnection); }
};

[[nodiscard]] bool program(const QString &path, const QByteArray &script) {
    return Fixtures::write(path, script) && QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

[[nodiscard]] QStringList lines(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
}

} // namespace

class PhonesTests : public QObject {
    Q_OBJECT

    SessionSecrets m_secrets;

private slots:
    void initTestCase() { Secrets::useInstead(&m_secrets); }
    void cleanupTestCase() { Secrets::useInstead(nullptr); }

    void aServerSaysWhoItIs() {
        QTemporaryDir dir;
        Elsewhere window(dir.filePath("Library"));
        const quint16 port = freePort();
        const auto address = window.start(serving(port, DeviceServing::By::Window));
        QVERIFY2(address, qPrintable(whyNot(address)));
        QCOMPARE(*address, u"http://127.0.0.1:%1"_s.arg(port));
        QCOMPARE(FakeDevice::ask("GET", QUrl(*address + "/opds"), withToken).status, 200);
        const auto status = DeviceServing::ask(QHostAddress(QHostAddress::LocalHost), port);
        QVERIFY(status);
        QCOMPARE(status->by, DeviceServing::By::Window);
        QVERIFY(status->opds);
        QVERIFY(!status->koreader);
        QCOMPARE(status->devices, 1); // the device; asking who serves isn't counted
        // Asked on every address, it is asked on this computer's own.
        QVERIFY(DeviceServing::ask(QHostAddress(QHostAddress::Any), port));
        // Only this computer is told.
        QVERIFY(DeviceServing::fromHere(QHostAddress(QHostAddress::LocalHost)));
        QVERIFY(DeviceServing::fromHere(QHostAddress(u"::ffff:127.0.0.1"_s)));
        QVERIFY(!DeviceServing::fromHere(QHostAddress(u"192.0.2.7"_s)));
        // Something that isn't Omabook is nobody.
        QTcpServer other;
        QVERIFY(other.listen(QHostAddress::LocalHost, 0));
        QVERIFY(!DeviceServing::ask(QHostAddress(QHostAddress::LocalHost), other.serverPort(), 300ms));
        QVERIFY(!DeviceServing::ask(QHostAddress(QHostAddress::LocalHost), freePort(), 300ms));
    }

    void aTakenPortSaysWhatTookIt() {
        QTemporaryDir dir;
        Elsewhere window(dir.filePath("Library"));
        const quint16 port = freePort();
        QVERIFY(window.start(serving(port, DeviceServing::By::Window)));
        DeviceServer second(dir.filePath("Library"));
        const auto served = second.start(serving(port, DeviceServing::By::Command));
        QVERIFY(!served);
        QCOMPARE(served.error().code(), Failure::Code::ServedByWindow);
        QCOMPARE(served.error().text(), u"The Omabook window is serving the library already, at http://127.0.0.1:%1."_s.arg(port));
        window.stop();
        Elsewhere service(dir.filePath("Library"));
        QVERIFY(service.start(serving(port, DeviceServing::By::Service)));
        QCOMPARE(second.start(serving(port, DeviceServing::By::Command)).error().code(), Failure::Code::ServedByService);
        // Another program: this test's own, by its name.
        QTcpServer other;
        QVERIFY(other.listen(QHostAddress::LocalHost, 0));
        const auto taken = second.start(serving(other.serverPort(), DeviceServing::By::Command));
        QVERIFY(!taken);
        QCOMPARE(taken.error().code(), Failure::Code::PortTakenBy);
        const QString name = QFileInfo(QCoreApplication::applicationFilePath()).fileName().left(15);
        QCOMPARE(taken.error().text(),
                 u"%1 is using port %2 on 127.0.0.1, so Omabook can't serve there. Choose another port, or stop %1."_s.arg(name).arg(
                     other.serverPort()));
    }

    // A stand-in /proc: a socket listening on port 6666 held by a reader app, and another process.
    void theProgramOnAPortIsFoundByItsSocket() {
        QTemporaryDir proc;
        QVERIFY(QDir().mkpath(proc.filePath("net")) && QDir().mkpath(proc.filePath("1234/fd")) && QDir().mkpath(proc.filePath("99/fd")));
        QVERIFY(Fixtures::write(proc.filePath("net/tcp"),
                                "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n"
                                "   0: 0100007F:1A0A 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 4242 1 0\n"
                                "   1: 0100007F:1A0B 0100007F:9C40 01 00000000:00000000 00:00000000 00000000  1000        0 4343 1 0\n"));
        QVERIFY(QFile::link("socket:[4242]", proc.filePath("1234/fd/7")));
        QVERIFY(QFile::link("socket:[1]", proc.filePath("99/fd/3")));
        QVERIFY(Fixtures::write(proc.filePath("1234/comm"), "reader-app\n"));
        QVERIFY(Fixtures::write(proc.filePath("99/comm"), "other\n"));
        QCOMPARE(PortHolder::program(6666, proc.path()), QString("reader-app"));
        QCOMPARE(PortHolder::program(6667, proc.path()), QString()); // connected, not listening
        QCOMPARE(PortHolder::program(6668, proc.path()), QString());
        QCOMPARE(PortHolder::program(6666, proc.filePath("none")), QString());
    }

    // `omabook serve` while the window serves: what the window serves, not "address already in use".
    void theCommandLineUsesTheWindowServing() {
        QTemporaryDir dir;
        Elsewhere window(dir.filePath("Library"));
        const quint16 port = freePort();
        QVERIFY(window.start(serving(port, DeviceServing::By::Window)));
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        for (const char *folder : {"config", "data", "cache", "state"})
            QVERIFY(QDir().mkpath(dir.filePath(folder)));
        environment.insert("HOME", dir.path());
        environment.insert("XDG_CONFIG_HOME", dir.filePath("config"));
        environment.insert("XDG_DATA_HOME", dir.filePath("data"));
        environment.insert("XDG_CACHE_HOME", dir.filePath("cache"));
        environment.insert("XDG_STATE_HOME", dir.filePath("state"));
        environment.insert("DBUS_SESSION_BUS_ADDRESS", "unix:path=" + dir.filePath("no-bus"));
        const auto serve = [&](const QStringList &more) {
            QProcess omabook;
            omabook.setProcessEnvironment(environment);
            omabook.start(
                OMABOOK_BINARY,
                QStringList{"serve", "--reach", "localhost", "--port", QString::number(port), "--library", dir.filePath("Library")} + more);
            omabook.closeWriteChannel();
            const bool ended = omabook.waitForFinished(20000);
            return std::make_pair(ended ? omabook.exitCode() : -1,
                                  QString::fromUtf8(omabook.readAllStandardOutput() + omabook.readAllStandardError()));
        };
        const QString address = u"http://127.0.0.1:%1"_s.arg(port);
        const auto [code, said] = serve({});
        QVERIFY2(code == 0, qPrintable(said));
        QVERIFY2(said.contains(u"The Omabook window is serving the library already, at %1."_s.arg(address)), qPrintable(said));
        QVERIFY2(said.contains(u"OPDS catalog: %1/opds"_s.arg(address)), qPrintable(said));
        QVERIFY2(!said.contains("in use"), qPrintable(said));
        // KOReader's sync asked for, which the window doesn't serve: said, and not done.
        const auto [koreaderCode, koreaderSaid] = serve({"--koreader"});
        QCOMPARE(koreaderCode, 1);
        QVERIFY2(koreaderSaid.contains("It doesn't serve all that was asked for"), qPrintable(koreaderSaid));
    }

    void theTailnetNameIsWhatTailscaleSays() {
        QCOMPARE(DeviceReach::tailnetName(R"({"Self": {"DNSName": "reader-pc.example.ts.net.", "TailscaleIPs": ["100.64.0.9"]}})"),
                 QString("reader-pc.example.ts.net"));
        QCOMPARE(DeviceReach::tailnetName(R"({"Self": {"DNSName": ""}})"), QString());
        QCOMPARE(DeviceReach::tailnetName("not json"), QString());
    }

    // Serving with the window closed: a user service, written and started
    // through systemctl, the window letting go of the port to it.
    void theWindowLeavesServingToItsService() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("bin")));
        const QString log = dir.filePath("systemctl.log");
        QVERIFY(program(dir.filePath("bin/systemctl"), "#!/bin/sh\necho \"$@\" >> '" + log.toUtf8() + "'\n"));
        QVERIFY(program(dir.filePath("bin/tailscale"), "#!/bin/sh\necho '{\"Self\": {\"DNSName\": \"reader-pc.example.ts.net.\"}}'\n"));
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        SessionSecrets secrets;
        const DeviceDesk::Programs programs{
            .service = {.folder = dir.filePath("systemd/user"), .systemctl = dir.filePath("bin/systemctl"), .omabook = "/usr/bin/omabook"},
            .tailscale = dir.filePath("bin/tailscale")};
        DeviceDesk desk(&settings, secrets, [&settings] { return settings.libraryPath(); }, programs);
        const quint16 port = freePort();
        desk.set("port", port);
        desk.set("serve", "localhost");
        QTRY_COMPARE(desk.servedBy(), QString("window"));
        QCOMPARE(desk.address(), u"http://127.0.0.1:%1"_s.arg(port));
        QCOMPARE(desk.namedAddress(), QString()); // this computer only: no name to give
        const QString unit = dir.filePath("systemd/user/omabook-serve.service");
        QVERIFY(!QFile::exists(unit));
        QVERIFY(lines(log).isEmpty()); // nothing asked of systemd

        desk.set("serve_background", true);
        QTRY_COMPARE(lines(log), (QStringList{"--user daemon-reload", "--user enable --now omabook-serve.service"}));
        QFile written(unit);
        QVERIFY(written.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(written.readAll());
        QVERIFY2(text.contains("ExecStart=\"/usr/bin/omabook\" serve --service\n"), qPrintable(text));
        QVERIFY2(text.contains("Restart=on-failure\n") && text.contains("WantedBy=default.target\n"), qPrintable(text));
        QTRY_VERIFY(canListen(port)); // the window let go
        // The stand-in starts nothing, which the window says once it has had a moment.
        QTRY_VERIFY_WITH_TIMEOUT(desk.problem().contains("background service isn't answering"), 8000);
        QCOMPARE(desk.servedBy(), QString());
        // Now it serves, as `omabook serve --service` would.
        Elsewhere service(dir.filePath("Library"));
        QVERIFY(service.start(serving(port, DeviceServing::By::Service)));
        desk.refresh();
        QTRY_COMPARE(desk.servedBy(), QString("service"));
        QVERIFY2(desk.problem().isEmpty(), qPrintable(desk.problem()));
        QCOMPARE(desk.address(), u"http://127.0.0.1:%1"_s.arg(port));

        // The settings it serves with changed: it is started again.
        desk.set("koreader_serve", true);
        QTRY_VERIFY(lines(log).contains("--user restart omabook-serve.service"));
        QTRY_COMPARE(desk.servedBy(), QString("service"));

        // Off again: stopped and gone, and the window serves.
        service.stop();
        desk.set("serve_background", false);
        QTRY_VERIFY(lines(log).contains("--user disable --now omabook-serve.service"));
        QTRY_VERIFY(!QFile::exists(unit));
        QTRY_COMPARE(desk.servedBy(), QString("window"));
        QCOMPARE(lines(log).last(), QString("--user daemon-reload"));
    }

    // The window finding another Omabook serving where it would: that one is shown serving.
    void theWindowShowsAnotherOmabookServing() {
        QTemporaryDir dir;
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        SessionSecrets secrets;
        const DeviceDesk::Programs programs{
            .service = {.folder = dir.filePath("systemd/user"), .systemctl = dir.filePath("none"), .omabook = "/usr/bin/omabook"},
            .tailscale = dir.filePath("none")};
        const quint16 port = freePort();
        Elsewhere command(dir.filePath("Library"));
        QVERIFY(command.start(serving(port, DeviceServing::By::Command)));
        DeviceDesk desk(&settings, secrets, [&settings] { return settings.libraryPath(); }, programs);
        desk.set("port", port);
        desk.set("serve", "localhost");
        QTRY_COMPARE(desk.servedBy(), QString("command"));
        QVERIFY2(desk.problem().isEmpty(), qPrintable(desk.problem()));
        QCOMPARE(desk.address(), u"http://127.0.0.1:%1"_s.arg(port));
    }

    // On every network, the tailnet's name is given too.
    void theNameOnTheTailnetIsGiven() {
        QTemporaryDir dir;
        QVERIFY(QDir().mkpath(dir.filePath("bin")));
        QVERIFY(program(dir.filePath("bin/tailscale"), "#!/bin/sh\necho '{\"Self\": {\"DNSName\": \"reader-pc.example.ts.net.\"}}'\n"));
        Settings settings(dir.filePath("config.json"));
        settings.setLibraryPath(dir.filePath("Library"));
        SessionSecrets secrets;
        const DeviceDesk::Programs programs{
            .service = {.folder = dir.filePath("systemd/user"), .systemctl = dir.filePath("none"), .omabook = "/usr/bin/omabook"},
            .tailscale = dir.filePath("bin/tailscale")};
        DeviceDesk desk(&settings, secrets, [&settings] { return settings.libraryPath(); }, programs);
        const quint16 port = freePort();
        desk.set("port", port);
        desk.set("serve", "network");
        QTRY_COMPARE(desk.servedBy(), QString("window"));
        QTRY_COMPARE(desk.namedAddress(), u"http://reader-pc.example.ts.net:%1"_s.arg(port));
    }
};

QTEST_GUILESS_MAIN(PhonesTests)
#include "phones_tests.moc"
