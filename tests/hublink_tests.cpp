// A computer linked to its person's hub (#180, ADR 0012): `omabook hub add`
// installs a key of the computer's own on the hub, over the person's ssh,
// that runs nothing but the hub's session, whatever the client asks; the
// session speaks only its protocol, and knows the computer by its key; a
// computer of another person's is refused; and `hub remove` (or `hub forget`
// on the hub) takes the key away, leaving the hub user's other keys alone.
//
// Nothing here touches the person's own ~/.ssh: every home is a temporary
// folder. The commands run behind a stand-in for ssh that plays the hub's
// sshd too: with a key (-i), it runs the forced command written on that key's
// line in the hub's authorized_keys, as sshd would, and only that. One test
// starts a real sshd, of its own, on this computer, and is skipped where
// there is none.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <algorithm>
#include <tuple>

#include "crypto.h"
#include "fixtures.h"
#include "hubcomputers.h"
#include "hublinks.h"
#include "hubremote.h"
#include "hubsession.h"
#include "library.h"
#include "log.h"
#include "person.h"
#include "sshkey.h"

using Fixtures::whyNot;
using namespace Qt::StringLiterals;

namespace {

// The environment of a program whose home is `home`, never the person's.
[[nodiscard]] QProcessEnvironment homeAt(const QString &home, const QString &path) {
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *folder : {"config", "data", "cache", "state", "runtime"})
        QDir().mkpath(QDir(home).filePath(QString::fromLatin1(folder)));
    environment.insert(u"HOME"_s, home);
    environment.insert(u"XDG_CONFIG_HOME"_s, home + u"/config"_s);
    environment.insert(u"XDG_DATA_HOME"_s, home + u"/data"_s);
    environment.insert(u"XDG_CACHE_HOME"_s, home + u"/cache"_s);
    environment.insert(u"XDG_STATE_HOME"_s, home + u"/state"_s);
    environment.insert(u"XDG_RUNTIME_DIR"_s, home + u"/runtime"_s);
    environment.insert(u"DBUS_SESSION_BUS_ADDRESS"_s, u"unix:path="_s + home + u"/no-bus"_s);
    environment.insert(u"PATH"_s, path);
    for (const char *variable : {"STATE_DIRECTORY", "RUNTIME_DIRECTORY", "CREDENTIALS_DIRECTORY", "SSH_AUTH_SOCK", "SSH_ASKPASS"})
        environment.remove(QString::fromLatin1(variable));
    return environment;
}

// The hub's sshd, played by a stand-in for ssh. Without a key it is the
// person's own access, a shell there; with one (-i), only the command on that
// key's line in authorized_keys runs, with what the client asked for in
// SSH_ORIGINAL_COMMAND, and a key that isn't there is turned away.
[[nodiscard]] QByteArray standInSshd(const QString &hubHome) {
    const QByteArray home = hubHome.toUtf8();
    return "key=\n"
           "while [ \"$1\" != \"--\" ]; do\n"
           "  if [ \"$1\" = -i ]; then key=\"$2\"; shift; fi\n"
           "  shift\n"
           "done\n"
           "shift; shift\n"
           "export HOME='"
           + home + "' XDG_CONFIG_HOME='" + home + "/config' XDG_DATA_HOME='" + home + "/data' XDG_CACHE_HOME='" + home
           + "/cache' XDG_STATE_HOME='" + home + "/state' XDG_RUNTIME_DIR='" + home
           + "/runtime' DBUS_SESSION_BUS_ADDRESS='unix:path=" + home
           + "/no-bus'\n"
             "[ -n \"$key\" ] || exec sh -c \"$1\"\n"
             "blob=$(cut -d' ' -f2 \"$key.pub\")\n"
             "line=$(grep -F \" $blob \" \"$HOME/.ssh/authorized_keys\" 2>/dev/null)\n"
             "[ -n \"$line\" ] || { echo 'Permission denied (publickey).' >&2; exit 255; }\n"
             "command=$(printf '%s\\n' \"$line\" | sed -n 's/^restrict,command=\"\\([^\"]*\\)\" .*/\\1/p')\n"
             "[ -n \"$command\" ] || { echo 'Not a forced command.' >&2; exit 255; }\n"
             "export SSH_ORIGINAL_COMMAND=\"$1\"\n"
             "exec sh -c \"$command\"\n";
}

// A person's computer ("here", or another person's) and their hub ("there").
struct Computers {
    QTemporaryDir dir;
    QString path;

    Computers() {
        QDir().mkpath(dir.filePath(u"bin"_s));
        Fixtures::script(dir.filePath(u"bin/ssh"_s), standInSshd(hubHome()));
        path = dir.filePath(u"bin"_s) + ':' + QFileInfo(QString::fromLatin1(OMABOOK_BINARY)).absolutePath() + ':'
               + qEnvironmentVariable("PATH");
        QDir().mkpath(hubHome() + u"/.ssh"_s);
    }

    // A key the hub user put there themselves, a line as sshd reads it.
    [[nodiscard]] QByteArray someoneElsesKey() const {
        const auto key = SshKey::generate(dir.filePath(u"someone"_s), u"someone@elsewhere"_s);
        return key ? (SshKey::line(*key) + u" someone@elsewhere\n"_s).toUtf8() : QByteArray();
    }

    [[nodiscard]] QString hubHome() const { return dir.filePath(u"there"_s); }
    [[nodiscard]] QString home(const QString &name) const { return dir.filePath(name); }
    [[nodiscard]] QString authorizedKeys() const { return hubHome() + u"/.ssh/authorized_keys"_s; }
    [[nodiscard]] QString keyOf(const QString &name) const { return home(name) + u"/data/omabook/hubs/key"_s; }

    [[nodiscard]] QStringList keyLines() const {
        QFile file(authorizedKeys());
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    }

    // Runs `program` with `arguments` at home `name`; its exit code, and what it printed.
    [[nodiscard]] std::pair<int, QString> run(const QString &name, const QString &program, const QStringList &arguments,
                                              const QByteArray &input = {}) const {
        QProcess process;
        process.setProcessEnvironment(homeAt(home(name), path));
        process.start(program, arguments);
        process.write(input);
        process.closeWriteChannel();
        if (!process.waitForFinished(60000))
            return {-1, u"still running"_s};
        return {process.exitCode(), QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
    }

    [[nodiscard]] std::pair<int, QString> omabook(const QString &name, const QStringList &arguments) const {
        return run(name, QString::fromLatin1(OMABOOK_BINARY), arguments);
    }

    // What a client of `name`'s asking with its hub key gets: the session, whatever it asks for.
    [[nodiscard]] std::pair<int, QString> withKey(const QString &name, const QString &command, const QByteArray &input) const {
        return run(name, dir.filePath(u"bin/ssh"_s), {u"-i"_s, keyOf(name), u"--"_s, u"hub"_s, command}, input);
    }
};

[[nodiscard]] QList<QJsonObject> answers(const QString &printed) {
    QList<QJsonObject> objects;
    for (const QString &line : printed.split('\n', Qt::SkipEmptyParts))
        objects << QJsonDocument::fromJson(line.toUtf8()).object();
    return objects;
}

const QByteArray hello = R"({"request": "hello", "protocol": 1})"
                         "\n";

// A computer's key, signed for linking by `person`.
[[nodiscard]] HubComputers::Asked asking(const QString &name, const QString &key, const Person &person) {
    const Person::Signature signature = person.sign(Crypto::bytes(HubComputers::linkMessage(name, key)));
    return {.name = name,
            .key = key,
            .identity = person.id(),
            .signature = QString::fromLatin1(
                Crypto::toByteArray(signature).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals))};
}

// A hub in a folder of its own, and a computer's key and person, made in it.
struct InProcess {
    QTemporaryDir dir;
    HubComputers computers{dir.filePath(u"state"_s), dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/usr/bin/omabook"_s}};
    std::expected<Person, QString> person = Person::generate();
    std::expected<SshKey::Public, Failure> key = SshKey::generate(dir.filePath(u"key"_s), u"laptop"_s);
};

// A hub's daemon, `omabook hub run`, started with its home at `home` and
// keeping a library of one book's file (`title`, none when empty) called
// home; why not, when it doesn't answer.
[[nodiscard]] QString startDaemon(QProcess &daemon, const QString &home, const QString &title = {}) {
    const QString folder = home + u"/libraries"_s;
    {
        Library library(folder + u"/home"_s);
        if (!library.open())
            return u"no library made for the hub"_s;
        if (!title.isEmpty()
            && (!Fixtures::epub(home + u"/"_s + title + u".epub"_s, title) || library.add(home + u"/"_s + title + u".epub"_s).id <= 0))
            return u"no book in the hub's library"_s;
    }
    daemon.setProcessChannelMode(QProcess::MergedChannels);
    daemon.setProcessEnvironment(homeAt(home, qEnvironmentVariable("PATH")));
    daemon.start(QString::fromLatin1(OMABOOK_BINARY), {u"hub"_s, u"run"_s, u"--libraries"_s, folder});
    QByteArray said;
    if (QTest::qWaitFor(
            [&] {
                said += daemon.readAll();
                return said.contains("takes requests at") || daemon.state() == QProcess::NotRunning;
            },
            30000)
        && daemon.state() == QProcess::Running)
        return {};
    return u"the hub's daemon didn't start: "_s + QString::fromUtf8(said);
}

void stop(QProcess &daemon) {
    daemon.terminate();
    if (!daemon.waitForFinished(10000))
        daemon.kill();
    daemon.waitForFinished(5000);
}

// A computer's library in `folder` with one book's file, `title`.
[[nodiscard]] bool libraryWith(const QString &folder, const QString &title) {
    Library library(folder);
    return library.open() && Fixtures::epub(folder + u"/../"_s + title + u".epub"_s, title)
           && library.add(folder + u"/../"_s + title + u".epub"_s).id > 0;
}

// A port nothing listens at now.
[[nodiscard]] quint16 freePort() {
    QTcpServer server;
    return server.listen(QHostAddress::LocalHost) ? server.serverPort() : 0;
}

// A hub reached through a real sshd, the test's own (#180): its host key, its
// user (by the passwd stand-in preloaded into sshd and ssh, where the
// container's user has none), that user's home, a free port and two linked
// computers' keys, laptop and desktop, all in a temporary folder. Its log
// is sshd's, and the session's in the hub's log.
struct RealHub {
    QTemporaryDir dir;
    QString home = dir.filePath(u"home"_s);
    QString sshd = QStandardPaths::findExecutable(u"sshd"_s, {u"/usr/sbin"_s, u"/usr/bin"_s, u"/usr/local/sbin"_s});
    QString ssh = QStandardPaths::findExecutable(u"ssh"_s);
    QString agent = QStandardPaths::findExecutable(u"ssh-agent"_s, {u"/usr/sbin"_s, u"/usr/bin"_s, u"/usr/local/bin"_s});
    QString env = QStandardPaths::findExecutable(u"env"_s);
    HubComputers computers{home + u"/state/omabook-hub"_s,
                           home + u"/.ssh/authorized_keys"_s,
                           {env, u"HOME="_s + home, u"XDG_STATE_HOME="_s + home + u"/state"_s, u"XDG_RUNTIME_DIR="_s + home + u"/runtime"_s,
                            u"XDG_CONFIG_HOME="_s + home + u"/config"_s, u"XDG_DATA_HOME="_s + home + u"/data"_s,
                            u"DBUS_SESSION_BUS_ADDRESS=unix:path="_s + home + u"/no-bus"_s,
                            QFileInfo(QString::fromLatin1(OMABOOK_BINARY)).absoluteFilePath()}};
    quint16 port = 0;
    QProcess server;
    QProcess agentProcess;

    RealHub() { server.setProcessChannelMode(QProcess::MergedChannels); }
    RealHub(const RealHub &) = delete;
    RealHub &operator=(const RealHub &) = delete;
    RealHub(RealHub &&) = delete;
    RealHub &operator=(RealHub &&) = delete;
    ~RealHub() {
        for (QProcess *process : {&server, &agentProcess}) {
            process->kill();
            process->waitForFinished(5000);
        }
    }

    [[nodiscard]] bool found() const { return !sshd.isEmpty() && !ssh.isEmpty() && !agent.isEmpty() && !env.isEmpty(); }

    // sshd, ssh and ssh-agent see the test's user, with its home here.
    [[nodiscard]] QProcessEnvironment environment() const {
        QProcessEnvironment environment = homeAt(dir.filePath(u"client"_s), qEnvironmentVariable("PATH"));
        environment.insert(u"LD_PRELOAD"_s, QString::fromLatin1(HUBLINK_PASSWD));
        environment.insert(u"HUBLINK_PASSWD_NAME"_s, u"hubtest"_s);
        environment.insert(u"HUBLINK_PASSWD_HOME"_s, home);
        return environment;
    }

    [[nodiscard]] QString fingerprint(const QString &computer) const {
        const auto key = SshKey::publicOf(dir.filePath(computer));
        return key ? SshKey::fingerprint(*key) : QString();
    }

    // Links laptop and desktop, and starts sshd and an agent; why not, when not.
    [[nodiscard]] QString start() {
        const auto person = Person::generate();
        if (!person || !SshKey::generate(dir.filePath(u"host_key"_s), u"the test's sshd"_s))
            return u"no keys made"_s;
        for (const QString &computer : {u"laptop"_s, u"desktop"_s}) {
            const auto key = SshKey::generate(dir.filePath(computer), computer);
            if (!key)
                return whyNot(key);
            if (const auto linked = computers.link(asking(computer, SshKey::line(*key), *person)); !linked)
                return whyNot(linked);
        }
        port = freePort();
        const QString config = u"Port %1\nListenAddress 127.0.0.1\nHostKey %2\nPidFile none\nUsePAM no\nStrictModes no\n"
                               "PasswordAuthentication no\nKbdInteractiveAuthentication no\nPubkeyAuthentication yes\n"
                               "AuthorizedKeysFile %3\nLogLevel DEBUG1\n"_s.arg(QString::number(port), dir.filePath(u"host_key"_s),
                                                                                home + u"/.ssh/authorized_keys"_s);
        if (port == 0 || !Fixtures::write(dir.filePath(u"sshd_config"_s), config.toUtf8()))
            return u"no port, or no sshd_config"_s;
        server.setProcessEnvironment(environment());
        server.start(sshd, {u"-D"_s, u"-E"_s, dir.filePath(u"sshd.log"_s), u"-f"_s, dir.filePath(u"sshd_config"_s)});
        const bool listening =
            QTest::qWaitFor([this] { return sshdLog().contains(u"listening on"_s) || server.state() == QProcess::NotRunning; }, 10000);
        if (!listening || server.state() != QProcess::Running)
            return u"sshd didn't start: "_s + QString::fromUtf8(server.readAll()) + sshdLog();
        return {};
    }

    // An agent of the test's own, for a client to try to forward; why not, when not.
    [[nodiscard]] QString startAgent() {
        agentProcess.setProcessChannelMode(QProcess::MergedChannels);
        agentProcess.setProcessEnvironment(environment());
        agentProcess.start(agent, {u"-D"_s, u"-a"_s, agentSocket()});
        if (QTest::qWaitFor([this] { return QFileInfo::exists(agentSocket()) || agentProcess.state() == QProcess::NotRunning; }, 20000)
            && agentProcess.state() == QProcess::Running)
            return {};
        return u"ssh-agent didn't start (%1): "_s.arg(agentProcess.state() == QProcess::Running ? u"no socket yet"_s : u"it ended"_s)
               + QString::fromUtf8(agentProcess.readAll());
    }

    [[nodiscard]] QString agentSocket() const { return dir.filePath(u"agent.sock"_s); }

    // ssh to the hub with `computer`'s key, `options` before the host and `command` after it.
    [[nodiscard]] std::pair<int, QString> client(const QString &computer, const QStringList &options, const QString &command,
                                                 const QByteArray &input) {
        QStringList arguments{u"-F"_s, u"/dev/null"_s,
                              u"-p"_s, QString::number(port),
                              u"-o"_s, u"BatchMode=yes"_s,
                              u"-o"_s, u"StrictHostKeyChecking=no"_s,
                              u"-o"_s, u"UserKnownHostsFile=/dev/null"_s,
                              u"-o"_s, u"GlobalKnownHostsFile=/dev/null"_s,
                              u"-o"_s, u"IdentityAgent=none"_s,
                              u"-o"_s, u"LogLevel=ERROR"_s,
                              u"-i"_s, dir.filePath(computer),
                              u"-o"_s, u"IdentitiesOnly=yes"_s};
        // ssh keeps the first value of an option: the attempt's own come first.
        arguments = options + arguments;
        arguments << u"hubtest@127.0.0.1"_s;
        if (!command.isEmpty())
            arguments << command;
        QProcess process;
        process.setProcessEnvironment(environment());
        process.start(ssh, arguments);
        process.write(input);
        process.closeWriteChannel();
        const bool ended = process.waitForFinished(30000);
        if (!ended)
            process.kill();
        return {ended ? process.exitCode() : -1, QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
    }

    // What sshd and its sessions wrote (-E): the hub's sshd log.
    [[nodiscard]] QString sshdLog() const {
        QFile file(dir.filePath(u"sshd.log"_s));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    // The hub's own log, where the session writes.
    [[nodiscard]] QList<Log::Entry> log() const { return Log::read({.lines = 1000}, home + u"/state/omabook-hub"_s); }
    [[nodiscard]] bool logged(const QString &words) const {
        return std::ranges::any_of(log(), [&words](const Log::Entry &entry) { return entry.message.contains(words); });
    }
    [[nodiscard]] QString logLines() const {
        QStringList lines;
        for (const Log::Entry &entry : log())
            lines << entry.level + u" "_s + entry.message;
        return lines.join('\n');
    }
};
} // namespace

class HubLinkTests : public QObject {
    Q_OBJECT

private slots:
    void aComputerLinksToItsPersonsHub() {
        const Computers computers;
        // A key of the hub user's own, which linking leaves as it is.
        const QByteArray theirs = computers.someoneElsesKey();
        QVERIFY(!theirs.isEmpty());
        QVERIFY(Fixtures::write(computers.authorizedKeys(), theirs));
        const auto [added, said] = computers.omabook(u"here"_s, {u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"laptop"_s});
        QVERIFY2(added == 0, qPrintable(said));
        QVERIFY2(said.contains(u"linked to the hub at hub as laptop"_s), qPrintable(said));
        const QStringList lines = computers.keyLines();
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines.at(0).toUtf8() + '\n', theirs);
        const auto key = SshKey::publicOf(computers.keyOf(u"here"_s));
        QVERIFY2(key, qPrintable(whyNot(key)));
        QCOMPARE(lines.at(1),
                 u"restrict,command=\"'%1' hub session --key '%2'\" %3 omabook-hub:laptop"_s.arg(
                     QFileInfo(QString::fromLatin1(OMABOOK_BINARY)).canonicalFilePath(), SshKey::fingerprint(*key), SshKey::line(*key)));
        // The key's secret half is this computer's user's alone.
        QCOMPARE(QFile::permissions(computers.keyOf(u"here"_s)), QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
        // Both sides list it: the hub itself, and the computer through its key.
        const auto [listed, there] = computers.omabook(u"there"_s, {u"hub"_s, u"computers"_s, u"--json"_s});
        QVERIFY2(listed == 0, qPrintable(there));
        const QJsonObject linked = QJsonDocument::fromJson(there.toUtf8()).array().at(0).toObject();
        QCOMPARE(linked.value("name").toString(), u"laptop"_s);
        QCOMPARE(linked.value("fingerprint").toString(), SshKey::fingerprint(*key));
        const auto [asked, here] = computers.omabook(u"here"_s, {u"hub"_s, u"computers"_s, u"hub"_s});
        QVERIFY2(asked == 0 && here.contains(u"laptop"_s) && here.contains(u"(this computer)"_s), qPrintable(here));
        // The hub's own requests reach its daemon, which isn't running here.
        const auto [status, notRunning] = computers.omabook(u"here"_s, {u"hub"_s, u"status"_s, u"hub"_s});
        QVERIFY2(status != 0 && notRunning.contains(u"No hub is running"_s), qPrintable(notRunning));
    }

    void theKeyOpensOnlyTheSession() {
        const Computers computers;
        const auto [added, said] = computers.omabook(u"here"_s, {u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"laptop"_s});
        QVERIFY2(added == 0, qPrintable(said));
        const QString pwned = computers.dir.filePath(u"pwned"_s);
        for (const QString &command : {u"touch '%1'"_s.arg(pwned), u"sh"_s, u"omabook hub session; touch '%1'"_s.arg(pwned)}) {
            const auto [code, refused] = computers.withKey(u"here"_s, command, "touch " + pwned.toUtf8() + "\n" + hello);
            QCOMPARE(code, 1);
            QVERIFY2(refused.contains(u"opens only the hub's session"_s), qPrintable(refused));
        }
        QVERIFY(!QFileInfo::exists(pwned));
        // The session itself: hello first, then only its requests.
        const auto [code, said2] = computers.withKey(u"here"_s, u"omabook hub session"_s,
                                                     hello
                                                         + R"({"request": "shell", "command": "rm -rf ~"})"
                                                           "\n"
                                                           R"({"request": "computers", "computer": "another"})"
                                                           "\n");
        QCOMPARE(code, 0);
        const QList<QJsonObject> answered = answers(said2);
        QCOMPARE(answered.size(), 3);
        QCOMPARE(answered.at(0).value("computer").toString(), u"laptop"_s);
        QCOMPARE(answered.at(0).value("protocol").toInt(), 1);
        QCOMPARE(answered.at(1).value("error").toString(), u"The hub takes no request called shell."_s);
        QCOMPARE(answered.at(2).value("computers").toArray().size(), 1);
        const auto [unsaid, noHello] = computers.withKey(u"here"_s, u"omabook hub session"_s,
                                                         R"({"request": "status"})"
                                                         "\n");
        QCOMPARE(unsaid, 1);
        QVERIFY2(noHello.contains(u"speaks protocol 1"_s), qPrintable(noHello));
    }

    void anotherPersonsComputerIsRefused() {
        const Computers computers;
        const auto [added, said] = computers.omabook(u"here"_s, {u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"laptop"_s});
        QVERIFY2(added == 0, qPrintable(said));
        // Someone else's computer, with an identity of its own.
        const auto [refused, why] = computers.omabook(u"elsewhere"_s, {u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"intruder"_s});
        QVERIFY2(refused != 0 && why.contains(u"belongs to someone else"_s), qPrintable(why));
        QCOMPARE(computers.keyLines().size(), 1);
        QVERIFY(computers.keyLines().at(0).endsWith(u" omabook-hub:laptop"_s));
        const auto [code, turnedAway] = computers.withKey(u"elsewhere"_s, u"omabook hub session"_s, hello);
        QVERIFY2(code != 0 && turnedAway.contains(u"Permission denied"_s), qPrintable(turnedAway));
    }

    void removingTheComputerTakesItsKeyAway() {
        const Computers computers;
        const QByteArray theirs = computers.someoneElsesKey();
        QVERIFY(!theirs.isEmpty());
        QVERIFY(Fixtures::write(computers.authorizedKeys(), theirs));
        const QStringList add{u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"laptop"_s};
        QCOMPARE(computers.omabook(u"here"_s, add).first, 0);
        const auto [removed, said] = computers.omabook(u"here"_s, {u"hub"_s, u"remove"_s, u"hub"_s});
        QVERIFY2(removed == 0, qPrintable(said));
        QCOMPARE(computers.keyLines(), QStringList{QString::fromUtf8(theirs).trimmed()});
        const auto [asked, notLinked] = computers.omabook(u"here"_s, {u"hub"_s, u"status"_s, u"hub"_s});
        QVERIFY2(asked != 0 && notLinked.contains(u"isn't linked to the hub at hub"_s), qPrintable(notLinked));
        // Taken away on the hub itself: the computer's own removal still ends the link here.
        QCOMPARE(computers.omabook(u"here"_s, add).first, 0);
        const auto [forgotten, gone] = computers.omabook(u"there"_s, {u"hub"_s, u"forget"_s, u"laptop"_s});
        QVERIFY2(forgotten == 0, qPrintable(gone));
        QCOMPARE(computers.keyLines().size(), 1);
        const auto [code, turnedAway] = computers.withKey(u"here"_s, u"omabook hub session"_s, hello);
        QVERIFY2(code != 0, qPrintable(turnedAway));
        const auto [again, said2] = computers.omabook(u"here"_s, {u"hub"_s, u"remove"_s, u"hub"_s});
        QVERIFY2(again == 0, qPrintable(said2));
        const auto [listed, none] = computers.omabook(u"there"_s, {u"hub"_s, u"computers"_s});
        QVERIFY2(listed == 0 && none.contains(u"No computers are linked"_s), qPrintable(none));
    }

    void theSessionSpeaksOnlyItsProtocol() {
        InProcess hub;
        QVERIFY2(hub.person && hub.key, "made");
        const auto linked = hub.computers.link(asking(u"laptop"_s, SshKey::line(*hub.key), *hub.person));
        QVERIFY2(linked, qPrintable(whyNot(linked)));
        QByteArrayList relayed;
        const auto relay = [&relayed](const QByteArray &request) {
            relayed << request;
            return QByteArray(R"({"ok": true, "libraries": []})");
        };
        const auto answer = [](HubSession &session, const QByteArray &line) {
            return QJsonDocument::fromJson(session.answer(line)).object();
        };
        // Before a hello in its protocol, nothing: the session ends.
        for (const QByteArray &first : {QByteArray("[1]"), QByteArray(R"({"request": "status"})"),
                                        QByteArray(R"({"request": "hello", "protocol": 2})"), QByteArray("sh")}) {
            HubSession session(hub.computers, linked->fingerprint, relay);
            const QJsonObject refused = answer(session, first);
            QVERIFY2(!refused.value("ok").toBool() && session.over(), first.constData());
            QCOMPARE(refused.value("protocol").toInt(), 1);
            QVERIFY(!answer(session, hello).value("ok").toBool());
        }
        QVERIFY(relayed.isEmpty());
        HubSession session(hub.computers, linked->fingerprint, relay);
        const QJsonObject greeted = answer(session, hello);
        QVERIFY(greeted.value("ok").toBool());
        QCOMPARE(greeted.value("computer").toString(), u"laptop"_s);
        QCOMPARE(greeted.value("requests").toArray().toVariantList(),
                 (QVariantList{u"computers"_s, u"forget"_s, u"hello"_s, u"snapshots"_s, u"status"_s, u"sync"_s, u"unlink"_s, u"work"_s,
                               u"work-cancel"_s}));
        // The computer is the key's, never what a request says it is.
        QVERIFY(answer(session, R"({"request": "status", "computer": "desktop"})").value("ok").toBool());
        QCOMPARE(QJsonDocument::fromJson(relayed.value(0)).object().value("computer").toString(), u"laptop"_s);
        for (const QByteArray &refused : {QByteArray(R"({"request": "run", "command": "sh"})"), QByteArray("rm -rf ~"), QByteArray("{}")})
            QVERIFY2(!answer(session, refused).value("ok").toBool() && !session.over(), refused.constData());
        QCOMPARE(relayed.size(), 1);
        QVERIFY(answer(session, R"({"request": "unlink"})").value("ok").toBool());
        QVERIFY(session.over());
        QVERIFY(hub.computers.computers().isEmpty());
        // A key the hub doesn't know is told so, and nothing more.
        HubSession stranger(hub.computers, linked->fingerprint, relay);
        const QJsonObject unknown = answer(stranger, hello);
        QVERIFY(!unknown.value("ok").toBool() && unknown.contains("linked") && stranger.over());
        QVERIFY(HubSession::asksForSession(u""_s) && HubSession::asksForSession(u"omabook hub session"_s)
                && HubSession::asksForSession(u"/usr/bin/omabook hub session"_s));
        QVERIFY(!HubSession::asksForSession(u"omabook hub session; sh"_s) && !HubSession::asksForSession(u"sh"_s));
    }

    // From one of the person's computers, another is taken off the hub (#189), by its name alone.
    void aComputerForgetsAnother() {
        InProcess hub;
        QVERIFY2(hub.person && hub.key, "made");
        const auto desktopKey = SshKey::generate(hub.dir.filePath(u"desktop"_s), u"desktop"_s);
        QVERIFY2(desktopKey, qPrintable(whyNot(desktopKey)));
        const auto laptop = hub.computers.link(asking(u"laptop"_s, SshKey::line(*hub.key), *hub.person));
        const auto desktop = hub.computers.link(asking(u"desktop"_s, SshKey::line(*desktopKey), *hub.person));
        QVERIFY2(laptop && desktop, "linked");
        HubSession session(hub.computers, laptop->fingerprint, [](const QByteArray &) { return QByteArray(R"({"ok": true})"); });
        const auto answer = [&session](const QByteArray &line) { return QJsonDocument::fromJson(session.answer(line)).object(); };
        QVERIFY(answer(hello.trimmed()).value("ok").toBool());
        // A fingerprint is no name, and a computer the hub hasn't is said so.
        for (const QByteArray &refused : {R"({"request": "forget", "name": ")" + desktop->fingerprint.toUtf8() + R"("})",
                                          QByteArray(R"({"request": "forget", "name": "nobody"})"), QByteArray(R"({"request": "forget"})")})
            QVERIFY2(!answer(refused).value("ok").toBool() && !session.over(), refused.constData());
        QCOMPARE(hub.computers.computers().size(), 2);
        const QJsonObject forgotten = answer(R"({"request": "forget", "name": "desktop"})");
        QVERIFY(forgotten.value("ok").toBool());
        QCOMPARE(forgotten.value("computer").toString(), u"desktop"_s);
        QVERIFY(!session.over());
        QCOMPARE(hub.computers.computers().size(), 1);
        QCOMPARE(hub.computers.computers().first().name, u"laptop"_s);
        // Its own name: as unlink, and the session ends.
        QVERIFY(answer(R"({"request": "forget", "name": "laptop"})").value("ok").toBool());
        QVERIFY(session.over());
        QVERIFY(hub.computers.computers().isEmpty());
    }

    // The same from the command line, through the key and the hub's session.
    void forgetNamesTheHubFromAComputer() {
        const Computers computers;
        QCOMPARE(computers.omabook(u"here"_s, {u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"laptop"_s}).first, 0);
        const auto [refused, nobody] = computers.omabook(u"here"_s, {u"hub"_s, u"forget"_s, u"hub"_s, u"nobody"_s});
        QVERIFY2(refused != 0 && nobody.contains(u"No computer called nobody is linked to the hub."_s), qPrintable(nobody));
        QCOMPARE(computers.keyLines().size(), 1);
        const auto [forgot, said] = computers.omabook(u"here"_s, {u"hub"_s, u"forget"_s, u"hub"_s, u"laptop"_s});
        QVERIFY2(forgot == 0 && said.contains(u"The computer laptop is no longer linked to the hub at hub"_s), qPrintable(said));
        QVERIFY(computers.keyLines().isEmpty());
    }

    void aLinkIsCheckedBeforeAKeyIsWritten() {
        InProcess hub;
        QVERIFY2(hub.person && hub.key, "made");
        const QString key = SshKey::line(*hub.key);
        const auto code = [&hub](const HubComputers::Asked &asked) { return hub.computers.link(asked).error().code(); };
        QCOMPARE(code(asking(u"laptop\" ,command=\"sh"_s, key, *hub.person)), Failure::Code::HubBadComputerName);
        QCOMPARE(code(asking(u"laptop"_s, u"command=\"sh\" "_s + key, *hub.person)), Failure::Code::HubBadKey);
        QCOMPARE(code(asking(u"laptop"_s, key + u"\n"_s + key, *hub.person)), Failure::Code::HubBadKey);
        QCOMPARE(code(asking(u"laptop"_s, u"ssh-rsa "_s + QString::fromLatin1(QByteArray("\0\0\0\7ssh-rsa", 11).toBase64()), *hub.person)),
                 Failure::Code::HubBadKey);
        QCOMPARE(code(asking(u"laptop"_s, key.left(key.size() - 4), *hub.person)), Failure::Code::HubBadKey);
        // Signed by someone, but not by whom it names.
        const auto other = Person::generate();
        QVERIFY(other);
        HubComputers::Asked forged = asking(u"laptop"_s, key, *other);
        forged.identity = hub.person->id();
        QCOMPARE(code(forged), Failure::Code::HubBadSignature);
        HubComputers::Asked renamed = asking(u"laptop"_s, key, *hub.person);
        renamed.name = u"desktop"_s;
        QCOMPARE(code(renamed), Failure::Code::HubBadSignature);
        // A program the line can't hold.
        HubComputers odd(hub.dir.filePath(u"state"_s), hub.dir.filePath(u"home/.ssh/authorized_keys"_s), {u"/opt/a\"b/omabook"_s});
        QCOMPARE(odd.link(asking(u"laptop"_s, key, *hub.person)).error().code(), Failure::Code::HubKeysUnwritable);
        QVERIFY(!QFileInfo::exists(hub.dir.filePath(u"home/.ssh/authorized_keys"_s)));
        // Linked, and again in place of itself; ~/.ssh made as sshd wants it.
        QVERIFY(hub.computers.link(asking(u"laptop"_s, key, *hub.person)));
        QVERIFY(hub.computers.link(asking(u"laptop"_s, key, *hub.person)));
        QCOMPARE(hub.computers.computers().size(), 1);
        QCOMPARE(QFile::permissions(hub.dir.filePath(u"home/.ssh"_s)) & (QFile::WriteGroup | QFile::WriteOther | QFile::ReadOther),
                 QFile::Permissions());
        QCOMPARE(QFile::permissions(hub.dir.filePath(u"home/.ssh/authorized_keys"_s)) & (QFile::WriteGroup | QFile::WriteOther),
                 QFile::Permissions());
        QCOMPARE(hub.computers.owner(), hub.person->id());
        QCOMPARE(code(asking(u"desktop"_s, key, *other)), Failure::Code::HubNotOwner);
    }

    void aRealSshdRunsNothingButTheSession() {
        RealHub hub;
        if (!hub.found())
            QSKIP("No sshd, ssh and ssh-agent on this computer to start a hub's sshd of the test's own.");
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const auto [greeted, said] = hub.client(u"laptop"_s, {}, u"omabook hub session"_s, hello);
        QVERIFY2(greeted == 0, qPrintable(said + hub.sshdLog()));
        QCOMPARE(answers(said).value(0).value("computer").toString(), u"laptop"_s);
        QVERIFY2(hub.logged(u"A session began for laptop"_s), qPrintable(hub.logLines()));
        // Another command asked of ssh: a shell's, or the session for another computer's key.
        const QString pwned = hub.dir.filePath(u"pwned"_s);
        for (const QString &command :
             {u"touch '%1'"_s.arg(pwned), u"omabook hub session --key '%1'"_s.arg(hub.fingerprint(u"desktop"_s))}) {
            const auto [refused, why] = hub.client(u"laptop"_s, {}, command, hello);
            QVERIFY2(refused == 1 && why.contains(u"opens only the hub's session"_s), qPrintable(why));
            QVERIFY2(hub.logged(u"asked to run "_s + command.left(20)), qPrintable(hub.logLines()));
        }
        QVERIFY(!QFileInfo::exists(pwned));
        // A first line other than the hello.
        const auto [unsaid, noHello] = hub.client(u"laptop"_s, {}, u"omabook hub session"_s,
                                                  R"({"request": "status"})"
                                                  "\n" + hello);
        QVERIFY2(unsaid == 1 && noHello.contains(u"speaks protocol 1"_s) && answers(noHello).size() == 1, qPrintable(noHello));
        QVERIFY2(hub.logged(u"began with something other than a hello"_s), qPrintable(hub.logLines()));
    }

    void aRealSshdForwardsNothingThroughTheKey() {
        RealHub hub;
        if (!hub.found())
            QSKIP("No sshd, ssh and ssh-agent on this computer to start a hub's sshd of the test's own.");
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        // Ports, either way: to the sshd itself, and from the hub back here.
        const auto [local, banner] = hub.client(u"laptop"_s, {u"-W"_s, u"127.0.0.1:%1"_s.arg(hub.port)}, {}, {});
        QVERIFY2(local != 0 && !banner.contains(u"SSH-2.0"_s), qPrintable(banner));
        const auto [remote, refused] =
            hub.client(u"laptop"_s, {u"-N"_s, u"-o"_s, u"ExitOnForwardFailure=yes"_s, u"-R"_s, u"0:127.0.0.1:%1"_s.arg(hub.port)}, {}, {});
        QVERIFY2(remote != 0 && refused.contains(u"forwarding failed"_s), qPrintable(refused));
        // An agent and a terminal: sshd gives the session neither, which it would refuse.
        const QString agentStarted = hub.startAgent();
        QVERIFY2(agentStarted.isEmpty(), qPrintable(agentStarted));
        const auto [agent, unforwarded] =
            hub.client(u"laptop"_s, {u"-A"_s, u"-o"_s, u"IdentityAgent="_s + hub.agentSocket()}, u"omabook hub session"_s, hello);
        QVERIFY2(agent == 0 && answers(unforwarded).value(0).value("ok").toBool(), qPrintable(unforwarded + hub.sshdLog()));
        // With a terminal asked for, sshd refuses it and the session runs without one.
        const auto [terminal, noTerminal] = hub.client(u"laptop"_s, {u"-tt"_s}, u"omabook hub session"_s, hello);
        QVERIFY2(noTerminal.contains(u"PTY allocation request failed"_s), qPrintable(noTerminal));
        std::ignore = terminal; // ssh, holding a terminal it never got, ends as it likes.
        const QString log = hub.sshdLog();
        for (const QString &trace : {u"refused local port forward"_s, u"tcpip-forward listen"_s, u"agent forwarding disabled"_s,
                                     u"Allocating a pty not permitted"_s})
            QVERIFY2(log.contains(trace, Qt::CaseInsensitive),
                     qPrintable(trace + u" isn't in the hub's sshd log:\n"_s
                                + log.split(u'\n')
                                      .filter(QRegularExpression(u"agent|pty|tty|forward|tcpip|req|disabled|permit|refus"_s,
                                                                 QRegularExpression::CaseInsensitiveOption))
                                      .join(u'\n')
                                      .right(3500)));
        QVERIFY2(!hub.logged(u"came with"_s), qPrintable(hub.logLines()));
    }

    void aKeyCantSpeakForAnotherComputer() {
        RealHub hub;
        if (!hub.found())
            QSKIP("No sshd, ssh and ssh-agent on this computer to start a hub's sshd of the test's own.");
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const auto [code, said] = hub.client(u"laptop"_s, {}, u"omabook hub session"_s,
                                             R"({"request": "hello", "protocol": 1, "computer": "desktop"})"
                                             "\n"
                                             R"({"request": "unlink", "computer": "desktop"})"
                                             "\n");
        const QList<QJsonObject> answered = answers(said);
        QVERIFY2(answered.size() == 2, qPrintable(said));
        QCOMPARE(answered.at(0).value("computer").toString(), u"laptop"_s);
        QCOMPARE(answered.at(1).value("computer").toString(), u"laptop"_s);
        std::ignore = code;
        // Its own key went, the other's stayed: and that one still opens the hub.
        QCOMPARE(hub.computers.computers().size(), 1);
        QCOMPARE(hub.computers.computers().at(0).name, u"desktop"_s);
        const auto [other, still] = hub.client(u"desktop"_s, {}, u"omabook hub session"_s, hello);
        QVERIFY2(other == 0 && answers(still).value(0).value("computer").toString() == u"desktop"_s, qPrintable(still));
        QVERIFY2(hub.logged(u"said it was desktop"_s), qPrintable(hub.logLines()));
        const auto [gone, turnedAway] = hub.client(u"laptop"_s, {}, u"omabook hub session"_s, hello);
        QVERIFY2(gone != 0 && turnedAway.contains(u"Permission denied"_s), qPrintable(turnedAway));
    }

    void aComputerSyncsWithItsHubByHand() {
        const Computers computers;
        const auto [added, said] = computers.omabook(u"here"_s, {u"hub"_s, u"add"_s, u"hub"_s, u"--name"_s, u"laptop"_s});
        QVERIFY2(added == 0, qPrintable(said));
        QProcess daemon;
        const QString started = startDaemon(daemon, computers.hubHome(), u"Omega"_s);
        QVERIFY2(started.isEmpty(), qPrintable(started));
        const auto stopping = qScopeGuard([&daemon] { stop(daemon); });
        const QString library = computers.home(u"here"_s) + u"/library"_s;
        QVERIFY(libraryWith(library, u"Alpha"_s));
        const auto [dry, dryRun] = computers.omabook(u"here"_s, {u"sync"_s, u"hub"_s, u"--dry-run"_s, u"--library"_s, library});
        QVERIFY2(dry == 0 && dryRun.contains(u"A dry run"_s), qPrintable(dryRun));
        QVERIFY2(dryRun.contains(u"Sent to hub (1):\n  Alpha"_s) && dryRun.contains(u"Received here (1):\n  Omega"_s), qPrintable(dryRun));
        // The hub it is linked to, named or not.
        const auto [synced, sync] = computers.omabook(u"here"_s, {u"hub"_s, u"sync"_s, u"--library"_s, library});
        QVERIFY2(synced == 0 && sync.contains(u"1 received, 1 sent"_s), qPrintable(sync));
        const auto [status, there] = computers.omabook(u"here"_s, {u"hub"_s, u"status"_s, u"hub"_s, u"--json"_s});
        QVERIFY2(status == 0, qPrintable(there));
        QCOMPARE(QJsonDocument::fromJson(there.toUtf8()).object().value("libraries").toArray().at(0).toObject().value("books").toInt(), 2);
        const auto [again, inStep] = computers.omabook(u"here"_s, {u"hub"_s, u"sync"_s, u"hub"_s, u"--library"_s, library, u"--dry-run"_s});
        QVERIFY2(again == 0 && inStep.contains(u"both libraries are in step"_s), qPrintable(inStep));
        const auto [elsewhere, notLinked] = computers.omabook(u"here"_s, {u"hub"_s, u"sync"_s, u"elsewhere"_s, u"--library"_s, library});
        QVERIFY2(elsewhere != 0 && notLinked.contains(u"isn't linked to the hub at elsewhere"_s), qPrintable(notLinked));
        // Another library, not paired with the hub's (#214): refused, saying how to pair, as JSON too; then paired.
        const QString other = computers.home(u"here"_s) + u"/other/library"_s;
        QVERIFY(QDir().mkpath(other) && libraryWith(other, u"Beta"_s));
        const auto [unpaired, howTo] = computers.omabook(u"here"_s, {u"hub"_s, u"sync"_s, u"--library"_s, other, u"--json"_s});
        QVERIFY2(unpaired != 0 && howTo.startsWith(u'{') && howTo.contains(u"\"error\": \"The hub's library home isn't paired"_s)
                     && howTo.contains(u"omabook hub sync <hub>:home --pair"_s),
                 qPrintable(howTo));
        const auto [paired, pairedSync] = computers.omabook(u"here"_s, {u"hub"_s, u"sync"_s, u"--library"_s, other, u"--pair"_s});
        QVERIFY2(paired == 0 && pairedSync.contains(u"2 received, 1 sent"_s), qPrintable(pairedSync));
    }

    void aComputerSyncsThroughARealSshd() {
        RealHub hub;
        if (!hub.found())
            QSKIP("No sshd, ssh and ssh-agent on this computer to start a hub's sshd of the test's own.");
        const QString started = hub.start();
        QVERIFY2(started.isEmpty(), qPrintable(started));
        QProcess daemon;
        const QString running = startDaemon(daemon, hub.home);
        QVERIFY2(running.isEmpty(), qPrintable(running));
        const auto stopping = qScopeGuard([&daemon] { stop(daemon); });
        // The laptop's side: its key for its hubs, and the one hub it is linked to.
        const QString links = hub.dir.filePath(u"laptop-hubs"_s);
        const QString host = u"hubtest@127.0.0.1"_s;
        QVERIFY(QDir().mkpath(links) && QFile::copy(hub.dir.filePath(u"laptop"_s), links + u"/key"_s));
        QVERIFY(QFile::setPermissions(links + u"/key"_s, QFile::ReadOwner | QFile::WriteOwner));
        QVERIFY(Fixtures::write(links + u"/hubs.json"_s, QJsonDocument(QJsonArray{QJsonObject{{"host", host},
                                                                                              {"computer", "laptop"},
                                                                                              {"fingerprint", hub.fingerprint(u"laptop"_s)},
                                                                                              {"linked", "2026-11-23T00:00:00Z"}}})
                                                             .toJson()));
        const HubLinks linked(links, {hub.env,
                                      u"LD_PRELOAD="_s + QString::fromLatin1(HUBLINK_PASSWD),
                                      u"HUBLINK_PASSWD_NAME=hubtest"_s,
                                      u"HUBLINK_PASSWD_HOME="_s + hub.home,
                                      hub.ssh,
                                      u"-F"_s,
                                      u"/dev/null"_s,
                                      u"-p"_s,
                                      QString::number(hub.port),
                                      u"-o"_s,
                                      u"BatchMode=yes"_s,
                                      u"-o"_s,
                                      u"StrictHostKeyChecking=no"_s,
                                      u"-o"_s,
                                      u"UserKnownHostsFile=/dev/null"_s,
                                      u"-o"_s,
                                      u"GlobalKnownHostsFile=/dev/null"_s,
                                      u"-o"_s,
                                      u"IdentityAgent=none"_s,
                                      u"-o"_s,
                                      u"LogLevel=ERROR"_s});
        const QString folder = hub.dir.filePath(u"laptop-library/library"_s);
        QVERIFY(libraryWith(folder, u"Alpha"_s));
        Library library(folder);
        QVERIFY(library.open());
        for (const bool dryRun : {true, false}) {
            const auto session = linked.open(host);
            QVERIFY2(session, qPrintable(whyNot(session)));
            const SyncSummary summary = HubRemote::sync(
                library, host, [&session](const QByteArray &line) { return (*session)->ask(line); }, {},
                [dryRun] {
                    SyncOptions options;
                    options.dryRun = dryRun;
                    return options;
                }());
            QVERIFY2(summary.error.isEmpty() && summary.problems.isEmpty(),
                     qPrintable(summary.error + summary.problems.join(u'\n') + hub.logLines()));
            QCOMPARE(summary.sent, 1);
            QCOMPARE(summary.dryRun, dryRun);
        }
        const auto status = linked.ask(host, {{"request", "status"}});
        QVERIFY2(status, qPrintable(whyNot(status)));
        QCOMPARE(status->value("libraries").toArray().at(0).toObject().value("books").toInt(), 1);
        QVERIFY2(hub.logged(u"laptop began a sync of home"_s) && hub.logged(u"laptop began a dry run of a sync of home"_s),
                 qPrintable(hub.logLines()));
    }
};

QTEST_GUILESS_MAIN(HubLinkTests)
#include "hublink_tests.moc"
