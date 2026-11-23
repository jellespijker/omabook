#include "hubautocommands.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QString>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <print>
#include <tuple>

#include "desktopnotice.h"
#include "hubautorun.h"
#include "hubautosync.h"
#include "hublinks.h"
#include "hubremote.h"
#include "library.h"
#include "serveservice.h"
#include "settings.h"

using namespace Qt::StringLiterals;

namespace {

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

[[nodiscard]] bool has(const QStringList &given, const QString &option) {
    return given.contains(option) || std::ranges::any_of(given, [&option](const QString &word) { return word.startsWith(option + u'='); });
}

// "on" or "off", or nothing when it is neither.
[[nodiscard]] std::optional<bool> onOrOff(const QString &said) {
    if (said == u"on"_s)
        return true;
    if (said == u"off"_s)
        return false;
    return std::nullopt;
}

int automatic(const QString &host, bool on) {
    HubAutoSync autoSync(HubLinks::usualFolder());
    const bool wasRefused = !autoSync.hub(host).refused.isEmpty();
    if (const auto turned = autoSync.turn(host, on); !turned)
        return failLine(turned.error().text());
    if (!on) {
        printLine(stdout, u"Automatic sync with %1 is off: it syncs only when asked."_s.arg(host));
        return 0;
    }
    printLine(stdout, u"Automatic sync with %1 is on: a little after a change once the window is left alone, every quarter of an hour, "
                      "and when the hub is back in reach; it waits on battery or a metered connection. While the window is open, "
                      "or with it closed too: omabook hub sync --background on"_s.arg(host));
    if (wasRefused)
        printLine(stdout, u"It had stopped after a refused sync: it tries again."_s);
    return 0;
}

int background(bool on) {
    const ServeService service(ServeService::usual(), ServeService::hubSync());
    if (const auto done = on ? service.install() : service.remove(); !done)
        return failLine(done.error().text());
    printLine(stdout,
              on ? u"The hubs switched on sync by themselves with the window closed too, from login on (%1)."_s.arg(service.unitPath())
                 : u"The hubs sync by themselves only while the window is open."_s);
    return 0;
}

// The background service: syncs with the hubs switched on, until stopped.
int service() {
    HubAutoRun run({.folder = HubLinks::usualFolder(),
                    .hosts =
                        [] {
                            QStringList hosts;
                            for (const HubLinks::Hub &hub : HubLinks().hubs())
                                hosts << hub.host;
                            return hosts;
                        },
                    .sync =
                        [](const QString &host) {
                            // The library in use, opened for each sync, as `omabook hub sync` does: this
                            // computer's own, never a hub's, which has no keeper here.
                            // ast-grep-ignore: hub-writes-through-the-keeper
                            Library library(Settings().libraryPath());
                            SyncSummary summary;
                            if (!library.open(&summary.error))
                                return summary;
                            return HubRemote::syncWith(library, host);
                        },
                    .now = [] { return QDateTime::currentDateTimeUtc(); },
                    .wait = SyncWait::now,
                    .notify = [](const QString &title,
                                 const QString &body) { std::ignore = DesktopNotice::show(title, body); }, // Said in the panel either way.
                    .every = std::chrono::seconds(30)});
    run.start();
    printLine(stdout, u"Syncing with the hubs switched on, by itself, until stopped."_s);
    return QCoreApplication::exec();
}

} // namespace

bool isHubAutoCommand(const QStringList &given) {
    return has(given, u"--auto"_s) || has(given, u"--background"_s) || given.contains(u"--service"_s);
}

bool hubAutoNamesAHub(const QStringList &given) { return has(given, u"--auto"_s); }

int runHubAuto(const QStringList &given) {
    QCommandLineParser parser;
    parser.setApplicationDescription(u"Automatic sync with a hub: --auto on|off for the hub named (the one this computer is linked "
                                     "to when it names none), --background on|off to sync with the window closed too."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(u"hub"_s, u"The hub, as ssh reaches it."_s, u"[<hub>]"_s);
    parser.addOption({u"auto"_s, u"Sync with the hub by itself, or not."_s, u"on|off"_s});
    parser.addOption({u"background"_s, u"Sync by itself with the window closed too, from login on, or not."_s, u"on|off"_s});
    parser.addOption({u"service"_s, u"Sync by itself until stopped: what the background service runs."_s});
    QStringList words = given.mid(1);
    words[0] = u"omabook hub sync"_s;
    if (!parser.parse(words))
        return failLine(parser.errorText());
    if (parser.isSet(u"service"_s))
        return service();
    if (parser.isSet(u"background"_s)) {
        const auto on = onOrOff(parser.value(u"background"_s));
        return on ? background(*on) : failLine(u"--background is on or off."_s);
    }
    const auto on = onOrOff(parser.value(u"auto"_s));
    if (!on)
        return failLine(u"--auto is on or off."_s);
    return automatic(parser.positionalArguments().value(0).section(u':', 0, 0), *on);
}
