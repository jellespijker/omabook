#include "hubcommands.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTime>
#include <chrono>
#include <cstdio>
#include <functional>
#include <optional>
#include <print>
#include <tuple>
#include <unistd.h>

#include "hubcomputers.h"
#include "hubdaemon.h"
#include "hubkeeper.h"
#include "hublinkcommands.h"
#include "hublinks.h"
#include "hubplace.h"
#include "hubrequests.h"
#include "hubschedule.h"
#include "hubsession.h"
#include "hubsnapshots.h"
#include "hubsync.h"
#include "hubwork.h"
#include "log.h"
#include "snapshotdestinations.h"
#include "snapshothealth.h"
#include "snapshotretention.h"

using namespace Qt::StringLiterals;

namespace {

// What systemd is told by a daemon that can't run as it is set up: not to
// start it again and again (the unit's RestartPreventExitStatus=), as
// sysexits.h's EX_CONFIG.
constexpr int notSetUp = 78;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

[[nodiscard]] QByteArray compact(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// A connection to the daemon, which answers each request line with one.
class Asking {
public:
    explicit Asking(const QString &socket) { m_socket.connectToServer(socket); }

    [[nodiscard]] bool connected() { return m_socket.waitForConnected(2000); }

    // The daemon's answer to `line`; a refusal when it went away first. It
    // waits as long as the daemon takes: another's write may be before it.
    [[nodiscard]] QByteArray ask(const QByteArray &line) {
        m_socket.write(line + '\n');
        while (!m_socket.canReadLine())
            if (!m_socket.waitForReadyRead(-1))
                return compact(HubRequests::refusal(Failure(Failure::Code::HubNotRunning, {m_socket.serverName()})));
        return m_socket.readLine().trimmed();
    }

private:
    QLocalSocket m_socket;
};

int run(const QCommandLineParser &parser) {
    const HubPlace place = HubPlace::usual();
    // Its log is its own, beside its state; what it says reaches the journal too, through standard error.
    Log::install(u"hub"_s, place.state());
    if (parser.isSet(u"libraries"_s))
        if (const auto chosen = place.chooseLibraries(parser.value(u"libraries"_s)); !chosen)
            return failLine(chosen.error().text());
    const QString folder = place.libraries();
    if (folder.isEmpty()) {
        printLine(stderr, Failure(Failure::Code::HubNoLibraries).text());
        return notSetUp;
    }
    if (HubDaemon::answersAt(place.socket()))
        return failLine(Failure(Failure::Code::HubRunning, {place.socket()}).text());
    const Snapshots snapshots(SnapshotPlace::ofHub(place)); // before the keeper, which runs its moment snapshots
    const SnapshotDestinations destinations(SnapshotPlace::ofHub(place));
    HubKeeper keeper(folder);
    const SnapshotHealth health(snapshots);
    HubRequests requests = HubRequests::usual();
    HubSync syncs; // before the daemon, which hands it its requests
    syncs.addTo(requests);
    HubSchedule schedule(keeper, place.state());
    // What comes later hooks in here, before the libraries open (requests.add, #182 on).
    // The moment snapshots before each write that changes much, and the nightly one (#181).
    keeper.before(HubSnapshots::before(snapshots));
    HubSnapshots::answer(requests, snapshots); // their diff and restore (#183)
    schedule.daily(u"snapshot"_s, QTime(SnapshotRetention::nightHour, 0), HubSnapshots::noted(health, HubSnapshots::nightly(snapshots)));
    // Then copied to the second disk and away from home, an hour on (#182), then looked at (#184).
    schedule.daily(u"destinations"_s, QTime(SnapshotRetention::nightHour + 1, 0),
                   HubSnapshots::looked(health, HubSnapshots::destinations(destinations)));
    // Their checks, and the warnings told in the journal and asked by the person's computers (#184).
    schedule.every(u"check"_s, std::chrono::days(7), HubSnapshots::check(health, SnapshotHealth::Depth::Light));
    schedule.every(u"check-data"_s, std::chrono::days(30), HubSnapshots::check(health, SnapshotHealth::Depth::Data));
    schedule.every(u"warnings"_s, std::chrono::hours(1), HubSnapshots::warn(health));
    requests.add(u"snapshots"_s, HubSnapshots::status(health));
    // The libraries' long work, by the hub's own AI, a job at a time beside the requests (#192).
    HubWork work(keeper, place.state(), place.settings());
    work.addTo(requests);
    schedule.every(u"work"_s, std::chrono::minutes(10), work.job());
    if (const auto opened = keeper.open(); !opened)
        return failLine(opened.error().text());
    HubDaemon daemon(keeper, requests);
    if (const auto listening = daemon.listen(place.socket()); !listening)
        return failLine(listening.error().text());
    schedule.start();
    qCInfo(lcHub).noquote() << "The hub keeps" << keeper.names().size() << "libraries in" << folder;
    printLine(stdout, u"The hub keeps the libraries in %1 (%2), and takes requests at %3."_s.arg(folder, keeper.names().join(u", "_s),
                                                                                                 place.socket()));
    return QCoreApplication::exec();
}

// Reads standard input a line at a time, each line printed back answered by
// `answer`, until the input ends, a line is too long, or `over` says the
// session has ended.
int eachLine(const std::function<QByteArray(const QByteArray &)> &answer, const std::function<bool()> &over) {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 1;
    while (true) {
        const QByteArray line = input.readLine(static_cast<qint64>(HubDaemon::longestLine) + 1);
        if (line.isEmpty())
            return 0;
        if (!line.endsWith('\n') && !input.atEnd()) {
            printLine(stdout, QString::fromUtf8(compact(HubRequests::refusal(Failure(Failure::Code::HubBadRequest)))));
            return 1;
        }
        if (const QByteArray request = line.trimmed(); !request.isEmpty())
            printLine(stdout, QString::fromUtf8(answer(request)));
        if (over())
            return 1;
    }
}

[[nodiscard]] QByteArray notRunning() {
    return compact(HubRequests::refusal(Failure(Failure::Code::HubNotRunning, {HubPlace::usual().socket()})));
}

// On the hub itself, by its own user: each line handed to the daemon.
int session() {
    Asking daemon(HubPlace::usual().socket());
    if (!daemon.connected()) {
        printLine(stdout, QString::fromUtf8(notRunning()));
        return 1;
    }
    return eachLine([&daemon](const QByteArray &request) { return daemon.ask(request); }, [] { return false; });
}

// A computer's key's forced command (ADR 0012): the session's protocol
// (HubSession), whatever the client asked ssh to run, as the computer whose
// key has `fingerprint`. The daemon is asked only once a request needs it.
int keyedSession(const QString &fingerprint) {
    const HubPlace place = HubPlace::usual();
    // What it refuses is in the hub's log, beside what its daemon says, and
    // only there: its standard error goes back to the client over ssh.
    qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
    Log::install(u"hub"_s, place.state());
    const auto refuse = [&fingerprint](const QString &what, const QString &logged) {
        qCWarning(lcHub).noquote() << "The key" << fingerprint << logged << "instead of the session: refused.";
        QJsonObject refused = HubRequests::refusal(Failure(Failure::Code::HubOnlySession, {what}));
        refused.insert("protocol"_L1, HubSession::protocol);
        printLine(stdout, QString::fromUtf8(compact(refused)));
        return 1;
    };
    if (const QString asked = qEnvironmentVariable("SSH_ORIGINAL_COMMAND"); !HubSession::asksForSession(asked))
        return refuse(asked, u"asked to run "_s + HubSession::shown(asked));
    // The key's line forbids both (restrict); should either come along all the same, nothing runs.
    if (qEnvironmentVariableIsSet("SSH_TTY") || ::isatty(STDIN_FILENO) != 0)
        return refuse(u"a terminal"_s, u"came with a terminal"_s);
    if (qEnvironmentVariableIsSet("SSH_AUTH_SOCK"))
        return refuse(u"a forwarded agent"_s, u"came with a forwarded agent"_s);
    std::optional<Asking> daemon;
    HubSession session(HubComputers::usual(place), fingerprint, [&daemon, &place](const QByteArray &request) {
        if (!daemon) {
            daemon.emplace(place.socket());
            if (!daemon->connected()) {
                daemon.reset();
                return notRunning();
            }
        }
        return daemon->ask(request);
    });
    return eachLine([&session](const QByteArray &line) { return session.answer(line); }, [&session] { return session.over(); });
}

// How the hub's snapshots stand (#184), as its daemon says: what to see to, or that all is well.
void printSnapshots(const QJsonObject &snapshots) {
    if (snapshots.isEmpty())
        return;
    if (!snapshots.value("set_up"_L1).toBool()) {
        printLine(stdout, u"Snapshots aren't set up on this hub, so nothing is kept to go back to: omabook snapshot setup --hub"_s);
        return;
    }
    const QJsonArray warnings = snapshots.value("warnings"_L1).toArray();
    printLine(stdout, warnings.isEmpty() ? u"Snapshots: all is well."_s : u"Snapshots, to see to:"_s);
    for (const auto &warning : warnings)
        printLine(stdout, u"  - "_s + warning["text"_L1].toString());
}

// What the hub's background work is doing (#192): the job under way and how far it got, what waits, or why nothing can be done.
void printWork(const QJsonObject &work) {
    if (work.isEmpty())
        return;
    const int waiting = work.value("waiting"_L1).toInt();
    const QString more = waiting > 0 ? u", and %1 more waiting"_s.arg(waiting) : QString();
    if (const QJsonObject running = work.value("running"_L1).toObject(); !running.isEmpty())
        printLine(stdout, u"Work: building the graph of %1 (%2 of %3 requests, by %4)%5. omabook hub cancel stops it."_s.arg(
                              running["title"_L1].toString(), QString::number(running["done"_L1].toInt()),
                              QString::number(running["total"_L1].toInt()), running["model"_L1].toString(), more));
    else if (const QJsonObject lacking = work.value("unavailable"_L1).toObject(); !lacking.isEmpty())
        printLine(stdout, u"Work: none, as the hub has no model for quick work: %1"_s.arg(lacking["message"_L1].toString()));
    else
        printLine(stdout, u"Work: nothing under way%1."_s.arg(more));
    for (const auto &done : work.value("last"_L1).toArray())
        if (const QString outcome = done["outcome"_L1].toString(); outcome == "failed"_L1)
            printLine(stdout, u"  - The graph of %1 failed: %2"_s.arg(done["title"_L1].toString(), done["said"_L1].toString()));
}

int status(const QCommandLineParser &parser, const QStringList &words) {
    QJsonObject answer;
    if (!words.isEmpty()) {
        // A hub elsewhere, through this computer's key for it.
        const auto asked = HubLinks().ask(words.first(), {{"request", "status"}});
        if (!asked)
            return failLine(asked.error().text());
        answer = *asked;
        if (const auto kept = HubLinks().ask(words.first(), {{"request", "snapshots"}}); kept && kept->value("ok"_L1).toBool())
            answer.insert("snapshots"_L1, *kept);
        if (const auto work = HubLinks().ask(words.first(), {{"request", "work"}}); work && work->value("ok"_L1).toBool())
            answer.insert("work"_L1, *work);
    } else {
        Asking daemon(HubPlace::usual().socket());
        if (!daemon.connected())
            return failLine(Failure(Failure::Code::HubNotRunning, {HubPlace::usual().socket()}).text());
        answer = QJsonDocument::fromJson(daemon.ask(compact({{"request", "status"}}))).object();
        const QJsonObject kept = QJsonDocument::fromJson(daemon.ask(compact({{"request", "snapshots"}}))).object();
        if (answer.value("ok"_L1).toBool() && kept.value("ok"_L1).toBool())
            answer.insert("snapshots"_L1, kept);
        if (const QJsonObject work = QJsonDocument::fromJson(daemon.ask(compact({{"request", "work"}}))).object();
            work.value("ok"_L1).toBool())
            answer.insert("work"_L1, work);
    }
    if (parser.isSet(u"json"_s))
        printLine(stdout, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Indented)).trimmed());
    if (!answer.value("ok"_L1).toBool())
        return failLine(answer.value("error"_L1).toString());
    if (parser.isSet(u"json"_s))
        return 0;
    printLine(stdout, u"Omabook %1, keeping the libraries in %2:"_s.arg(answer.value("version"_L1).toString(),
                                                                        answer.value("folder"_L1).toString()));
    for (const auto &library : answer.value("libraries"_L1).toArray())
        printLine(stdout, u"  %1  %2 books"_s.arg(library["name"_L1].toString(), QString::number(library["books"_L1].toInt())));
    printSnapshots(answer.value("snapshots"_L1).toObject());
    printWork(answer.value("work"_L1).toObject());
    return 0;
}

// The job under way on the hub stopped (#192), on this hub or the one at <host>.
int cancel(const QStringList &words) {
    QJsonObject answer;
    if (!words.isEmpty()) {
        const auto asked = HubLinks().ask(words.first(), {{"request", "work-cancel"}});
        if (!asked)
            return failLine(asked.error().text());
        answer = *asked;
    } else {
        Asking daemon(HubPlace::usual().socket());
        if (!daemon.connected())
            return failLine(Failure(Failure::Code::HubNotRunning, {HubPlace::usual().socket()}).text());
        answer = QJsonDocument::fromJson(daemon.ask(compact({{"request", "work-cancel"}}))).object();
    }
    if (!answer.value("ok"_L1).toBool())
        return failLine(answer.value("error"_L1).toString());
    printLine(stdout, answer.value("cancelled"_L1).toBool() ? u"The hub's work under way is stopped; nothing of it is kept."_s
                                                            : u"The hub has no work under way."_s);
    return 0;
}

} // namespace

int runHubCli(const QStringList &arguments) {
    // Its options are omabook sync's.
    if (arguments.value(2) == u"sync"_s)
        return runHubSync(arguments);
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"The hub: an Omabook without a window that is always on. run keeps its libraries, as their one writer, until "
        "stopped (its systemd unit starts it); status asks the running hub what it keeps, or the hub at <host>. On a "
        "computer, add <host> links it to its person's hub over ssh: a key of its own, installed there to run nothing but "
        "the hub's session; remove <host> takes it off again, computers <host> lists the hub's computers, and forget <host> "
        "<computer> takes another of them off. On the hub, computers lists them and forget <computer> takes one's key away; "
        "cancel [<host>] stops the hub's background work under way (status says what it is doing), keeping nothing of it; "
        "sync [<host>] syncs this library with the hub, as omabook sync <host> does (--dry-run first, to see what it would "
        "do; --pair, once, for a library the hub's isn't paired with yet; --auto on|off to sync with it by itself, and --background on|off "
        "to do so with the window closed too); session is what a computer's key runs there, one JSON request a "
        "line in and one answer a line out, and allow "
        "is what add runs there."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(
        u"what"_s, u"run | status [<host>] | cancel [<host>] | add <host> | remove <host> | computers [<host>] | sync [<host>] | "
                   "forget [<host>] <computer> | session"_s);
    parser.addOption({u"libraries"_s, u"With run: keep the libraries in this folder, from now on."_s, u"folder"_s});
    parser.addOption({u"json"_s, u"With status or computers: print it as JSON."_s});
    parser.addOption({u"name"_s, u"With add: what the hub calls this computer (its host name otherwise)."_s, u"name"_s});
    parser.addOption(
        {u"key"_s, u"With session: the fingerprint of the computer's key it runs for (the hub writes it)."_s, u"fingerprint"_s});
    parser.addOption({u"identity"_s, u"With allow: the person whose computer it is."_s, u"id"_s});
    parser.addOption({u"signature"_s, u"With allow: the person's signature of the link."_s, u"signature"_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook hub"_s;
    parser.process(rest);
    QStringList words = parser.positionalArguments();
    const QString what = words.isEmpty() ? QString() : words.takeFirst();
    if (isHubLinkCommand(what))
        return runHubLink(what, words, parser);
    if (what == u"status"_s && words.size() <= 1)
        return status(parser, words);
    if (what == u"cancel"_s && words.size() <= 1)
        return cancel(words);
    if (!words.isEmpty() || !QStringList{u"run"_s, u"session"_s}.contains(what))
        return failLine(parser.helpText().trimmed());
    if (what == u"run"_s)
        return run(parser);
    return parser.isSet(u"key"_s) ? keyedSession(parser.value(u"key"_s)) : session();
}
