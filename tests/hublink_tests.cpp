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
#include <QStandardPaths>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QtTest>
#include <pwd.h>
#include <unistd.h>

#include "crypto.h"
#include "fixtures.h"
#include "hubcomputers.h"
#include "hubsession.h"
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

// A port nothing listens at now.
[[nodiscard]] quint16 freePort() {
    QTcpServer server;
    return server.listen(QHostAddress::LocalHost) ? server.serverPort() : 0;
}

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
        QCOMPARE(greeted.value("requests").toArray().toVariantList(), (QVariantList{u"computers"_s, u"hello"_s, u"status"_s, u"unlink"_s}));
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

    void aRealSshdRunsOnlyTheSession() {
        QString sshd = QStandardPaths::findExecutable(u"sshd"_s);
        if (sshd.isEmpty())
            sshd = QStandardPaths::findExecutable(u"sshd"_s, {u"/usr/sbin"_s, u"/usr/bin"_s, u"/usr/local/sbin"_s});
        const QString ssh = QStandardPaths::findExecutable(u"ssh"_s);
        const QString env = QStandardPaths::findExecutable(u"env"_s);
        if (sshd.isEmpty() || ssh.isEmpty() || env.isEmpty())
            QSKIP("No sshd and ssh on this computer to start one of the test's own.");
        const QTemporaryDir dir;
        const QString home = dir.filePath(u"home"_s);
        const auto hostKey = SshKey::generate(dir.filePath(u"host_key"_s), u"the test's sshd"_s);
        const auto key = SshKey::generate(dir.filePath(u"key"_s), u"laptop"_s);
        const auto person = Person::generate();
        QVERIFY2(hostKey && key && person, "made");
        // The session finds its state in the test's folder: sshd hands it its user's own home.
        HubComputers computers(home + u"/state/omabook-hub"_s, home + u"/.ssh/authorized_keys"_s,
                               {env, u"HOME="_s + home, u"XDG_STATE_HOME="_s + home + u"/state"_s,
                                u"XDG_RUNTIME_DIR="_s + home + u"/runtime"_s, u"XDG_CONFIG_HOME="_s + home + u"/config"_s,
                                u"XDG_DATA_HOME="_s + home + u"/data"_s, u"DBUS_SESSION_BUS_ADDRESS=unix:path="_s + home + u"/no-bus"_s,
                                QFileInfo(QString::fromLatin1(OMABOOK_BINARY)).absoluteFilePath()});
        const auto linked = computers.link(asking(u"laptop"_s, SshKey::line(*key), *person));
        QVERIFY2(linked, qPrintable(whyNot(linked)));
        const quint16 port = freePort();
        QVERIFY(port != 0);
        QVERIFY(Fixtures::write(dir.filePath(u"sshd_config"_s),
                                u"Port %1\nListenAddress 127.0.0.1\nHostKey %2\nPidFile none\nUsePAM no\n"
                                "StrictModes no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n"
                                "PubkeyAuthentication yes\nAuthorizedKeysFile %3\n"_s
                                    .arg(QString::number(port), dir.filePath(u"host_key"_s), home + u"/.ssh/authorized_keys"_s)
                                    .toUtf8()));
        QProcess server;
        server.setProcessChannelMode(QProcess::MergedChannels);
        server.start(sshd, {u"-D"_s, u"-e"_s, u"-f"_s, dir.filePath(u"sshd_config"_s)});
        QByteArray log;
        const bool listening = QTest::qWaitFor(
            [&] { return (log += server.readAll()).contains("listening on") || server.state() == QProcess::NotRunning; }, 10000);
        QVERIFY2(listening && server.state() == QProcess::Running, log.constData());
        const passwd *user = ::getpwuid(::geteuid());
        QVERIFY(user != nullptr);
        const QString target = QString::fromLocal8Bit(user->pw_name) + u"@127.0.0.1"_s;
        const QStringList options{u"-F"_s, u"/dev/null"_s,
                                  u"-p"_s, QString::number(port),
                                  u"-o"_s, u"BatchMode=yes"_s,
                                  u"-o"_s, u"StrictHostKeyChecking=no"_s,
                                  u"-o"_s, u"UserKnownHostsFile=/dev/null"_s,
                                  u"-o"_s, u"GlobalKnownHostsFile=/dev/null"_s,
                                  u"-o"_s, u"IdentityAgent=none"_s,
                                  u"-o"_s, u"LogLevel=ERROR"_s,
                                  u"-i"_s, dir.filePath(u"key"_s),
                                  u"-o"_s, u"IdentitiesOnly=yes"_s};
        const auto client = [&](const QStringList &more, const QByteArray &input) {
            QProcess process;
            process.start(ssh, options + more);
            process.write(input);
            process.closeWriteChannel();
            const bool ended = process.waitForFinished(30000);
            return std::pair{ended ? process.exitCode() : -1,
                             QString::fromUtf8(process.readAllStandardOutput() + process.readAllStandardError())};
        };
        const auto [greeted, said] = client({u"--"_s, target, u"omabook hub session"_s}, hello);
        QVERIFY2(greeted == 0, qPrintable(said + QString::fromUtf8(log + server.readAll())));
        QCOMPARE(answers(said).value(0).value("computer").toString(), u"laptop"_s);
        const QString pwned = dir.filePath(u"pwned"_s);
        const auto [refused, why] = client({u"--"_s, target, u"touch '%1'"_s.arg(pwned)}, {});
        QVERIFY2(refused == 1 && why.contains(u"opens only the hub's session"_s), qPrintable(why));
        // No forwarding through it either (restrict): not even to the sshd itself.
        const auto [forwarded, banner] = client({u"-W"_s, u"127.0.0.1:%1"_s.arg(port), target}, {});
        QVERIFY2(forwarded != 0 && !banner.contains(u"SSH-2.0"_s), qPrintable(banner));
        QVERIFY(!QFileInfo::exists(pwned));
        server.kill();
        QVERIFY(server.waitForFinished(10000));
    }
};

QTEST_GUILESS_MAIN(HubLinkTests)
#include "hublink_tests.moc"
