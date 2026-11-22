#include "hubcommands.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <cstdio>
#include <optional>
#include <print>
#include <tuple>

#include "hubdaemon.h"
#include "hubkeeper.h"
#include "hubplace.h"
#include "hubrequests.h"
#include "hubschedule.h"
#include "log.h"

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
    HubKeeper keeper(folder);
    const HubRequests requests = HubRequests::usual();
    HubSchedule schedule(keeper, place.state());
    // What comes later hooks in here, before the libraries open: the moment
    // snapshots (keeper.before), the nightly one (schedule.daily) and their
    // requests (requests.add), #181 on.
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

// Each line read is a request, handed to the daemon; each answer a line printed.
int session() {
    Asking daemon(HubPlace::usual().socket());
    if (!daemon.connected()) {
        printLine(stdout,
                  QString::fromUtf8(compact(HubRequests::refusal(Failure(Failure::Code::HubNotRunning, {HubPlace::usual().socket()})))));
        return 1;
    }
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
            printLine(stdout, QString::fromUtf8(daemon.ask(request)));
    }
}

int status(const QCommandLineParser &parser) {
    Asking daemon(HubPlace::usual().socket());
    if (!daemon.connected())
        return failLine(Failure(Failure::Code::HubNotRunning, {HubPlace::usual().socket()}).text());
    const QJsonObject answer = QJsonDocument::fromJson(daemon.ask(compact({{"request", "status"}}))).object();
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
    return 0;
}

} // namespace

int runHubCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"The hub: an Omabook without a window that is always on. run keeps its libraries, as their one writer, until "
        "stopped (its systemd unit starts it); session is what a computer's key runs on the hub, one JSON request a "
        "line in and one answer a line out; status asks the running hub what it keeps."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(u"what"_s, u"run | session | status"_s);
    parser.addOption({u"libraries"_s, u"With run: keep the libraries in this folder, from now on."_s, u"folder"_s});
    parser.addOption({u"json"_s, u"With status: print it as JSON."_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook hub"_s;
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    const QString what = words.value(0);
    if (words.size() != 1 || !QStringList{u"run"_s, u"session"_s, u"status"_s}.contains(what))
        return failLine(parser.helpText().trimmed());
    if (what == u"run"_s)
        return run(parser);
    return what == u"session"_s ? session() : status(parser);
}
