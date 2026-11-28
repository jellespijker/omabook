#include "destinationcommands.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <cstdio>
#include <iostream>
#include <print>
#include <string>
#include <termios.h>
#include <tuple>
#include <unistd.h>
#include <utility>

#include "hubplace.h"
#include "snapshotdestinations.h"

namespace {

using namespace Qt::StringLiterals;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int fail(const Failure &failure) {
    printLine(stderr, failure.text());
    return 1;
}

void printJson(const QJsonObject &object) {
    printLine(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

[[nodiscard]] QString when(const QDateTime &time) { return time.toString(u"yyyy-MM-dd HH:mm"_s); }

// Each credential named: from this command's environment, else typed.
[[nodiscard]] QList<std::pair<QString, QByteArray>> credentialsGiven(const QStringList &names) {
    QList<std::pair<QString, QByteArray>> given;
    for (const QString &name : names) {
        const QByteArray value = qgetenv(qPrintable(name));
        given.append({name, value.isEmpty() ? typedSecret(name) : value});
    }
    return given;
}

[[nodiscard]] QString whereOf(SnapshotDestination::Kind kind) {
    switch (kind) {
    case SnapshotDestination::Kind::Disk:
        return u"on a second disk, in"_s;
    case SnapshotDestination::Kind::Hub:
        return u"on another of your hubs, which keeps them and can't read them,"_s;
    case SnapshotDestination::Kind::Away:
        break;
    }
    return u"away from home, at"_s;
}

int add(const QCommandLineParser &parser, SnapshotDestinations &destinations) {
    const QStringList words = parser.positionalArguments();
    if (words.size() != 3) {
        printLine(stderr, u"omabook snapshot destination add <name> <where> [--credential NAME]..."_s);
        return 1;
    }
    const QStringList credentials = parser.values(u"credential"_s);
    const SnapshotDestination destination{.name = words.at(1), .location = words.at(2), .credentials = credentials};
    const auto key = destinations.add(destination, credentialsGiven(credentials));
    if (!key)
        return fail(key.error());
    const SnapshotDestination added = destinations.find(destination.name).value_or(destination);
    if (parser.isSet(u"json"_s)) {
        printJson({{u"name"_s, added.name},
                   {u"location"_s, added.location},
                   {u"kind"_s, SnapshotDestination::kindName(added.kind())},
                   {u"recovery_key"_s, *key}});
        return 0;
    }
    const QString where = whereOf(added.kind());
    printLine(stdout, u"The snapshots go to %1 too, %2 %3. They are copied there every night, or now with "
                      "omabook snapshot destination copy.\n\nIts recovery key, shown this once:\n\n    %4\n\n"
                      "Keep it as you keep the other one, away from this computer: without it nothing kept there can be restored."_s.arg(
                          added.name, where, added.location, *key));
    return 0;
}

[[nodiscard]] QString howItWent(const std::optional<DestinationState> &state) {
    if (!state)
        return u"not copied to yet"_s;
    if (state->done)
        return u"copied %1, its newest snapshot of %2, %3 there"_s.arg(when(state->copied), when(state->latest),
                                                                       QLocale::c().formattedDataSize(state->size));
    const QString last = state->copied.isValid() ? u"; last copied %1"_s.arg(when(state->copied)) : QString();
    if (state->absent)
        return u"isn't there since %1: is its disk plugged in?%2"_s.arg(when(state->tried), last);
    return u"couldn't be copied to at %1: %2%3"_s.arg(when(state->tried), state->said, last);
}

int list(const QCommandLineParser &parser, const SnapshotDestinations &destinations) {
    const QList<SnapshotDestination> all = destinations.list();
    const bool backedUp = destinations.backedUp();
    if (parser.isSet(u"json"_s)) {
        QJsonArray array;
        for (const SnapshotDestination &destination : all)
            array << destinations.json(destination);
        printJson({{u"backed_up"_s, backedUp}, {u"destinations"_s, array}});
        return 0;
    }
    if (all.isEmpty())
        printLine(stdout, u"The snapshots go nowhere else yet: omabook snapshot destination add <name> <where> adds a second disk, "
                          "or a place away from home."_s);
    for (const SnapshotDestination &destination : all)
        printLine(stdout, u"%1  %2  %3  %4"_s.arg(destination.name, SnapshotDestination::kindName(destination.kind()), destination.location,
                                                  howItWent(destinations.stateOf(destination.name))));
    printLine(stdout, backedUp ? u"\nBacked up: three copies, on two kinds of medium, one away from home."_s
                               : u"\nNot backed up as three copies on two kinds of medium, one away from home, ask: that takes a "
                                 "second disk and a place away whose last copies went through."_s);
    return 0;
}

int test(const QCommandLineParser &parser, const SnapshotDestinations &destinations, const QString &name) {
    const auto tested = destinations.test(name);
    if (!tested)
        return fail(tested.error());
    if (parser.isSet(u"json"_s)) {
        printJson({{u"name"_s, name},
                   {u"snapshots"_s, tested->snapshots},
                   {u"latest"_s, tested->latest ? tested->latest->time.toString(Qt::ISODate) : QString()}});
        return 0;
    }
    const QString count = tested->snapshots == 1 ? u"1 snapshot"_s : u"%1 snapshots"_s.arg(tested->snapshots);
    printLine(stdout, u"%1 is reached and opened with its password: %2%3."_s.arg(
                          name, count, tested->latest ? u", the newest of %1"_s.arg(when(tested->latest->time)) : QString()));
    return 0;
}

int copy(const SnapshotDestinations &destinations, const QString &name) {
    SnapshotDestinations::Round round;
    if (name.isEmpty()) {
        round = destinations.copyAll(QDateTime::currentDateTime());
    } else if (const auto copied = destinations.copy(name, QDateTime::currentDateTime()); copied) {
        round.copied << name;
    } else {
        round.failed.append({name, copied.error()});
    }
    for (const QString &copied : round.copied)
        printLine(stdout, u"Copied to %1."_s.arg(copied));
    for (const auto &[failedName, failure] : round.failed)
        printLine(stderr, u"Not copied to %1: %2"_s.arg(failedName, failure.text()));
    return round.failed.isEmpty() ? 0 : 1;
}

} // namespace

// A line typed that nobody should see: the terminal stops echoing for it.
QByteArray typedSecret(const QString &name) {
    termios before{};
    const bool terminal = isatty(STDIN_FILENO) != 0 && tcgetattr(STDIN_FILENO, &before) == 0;
    if (terminal) {
        std::print(stderr, "{}: ", qPrintable(name));
        std::ignore = fflush(stderr);
        termios quiet = before;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        std::ignore = tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::string line;
    std::getline(std::cin, line);
    if (terminal) {
        std::ignore = tcsetattr(STDIN_FILENO, TCSANOW, &before);
        std::println(stderr);
    }
    return QByteArray::fromStdString(line).trimmed();
}

int runSnapshotDestinationCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"Where else the snapshots are kept: a second disk (a folder on it), a place away from home that restic reaches "
        "(sftp:host:/path, rest:https://host/, s3:..., b2:..., rclone:...), or another of your hubs (hub:<host>, a hub "
        "this computer is linked to, omabook hub add), reached over the session with it, which keeps them and can't read them. Each is a "
        "restic repository of its own, with "
        "its own recovery key, shown once; the snapshots are copied there every night (omabook snapshot nightly, or the "
        "hub's daemon) and thinned as they are here. Three copies on two kinds of medium, one away from home, are backed up.\n\n"
        "add <name> <where> makes it; list says how each last copy went; test opens one; copy [name] copies now; remove "
        "<name> forgets it, and leaves what is kept there."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(u"what"_s, u"add, list, test, copy or remove."_s, u"<add|list|test|copy|remove>"_s);
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"hub"_s, u"The hub's destinations on this computer (omabook hub run), their secrets the hub's."_s});
    parser.addOption({u"credential"_s,
                      u"add: what restic needs to reach the place, by the name restic gives it (AWS_ACCESS_KEY_ID, "
                      "AWS_SECRET_ACCESS_KEY, B2_ACCOUNT_KEY, RESTIC_REST_PASSWORD...), taken from the environment or typed, "
                      "and kept as a secret, never with the choices."_s,
                      u"name"_s});
    QStringList rest = arguments;
    rest.remove(1, 2);
    rest[0] = u"omabook snapshot destination"_s;
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    const QString what = words.value(0);
    SnapshotDestinations destinations(parser.isSet(u"hub"_s) ? SnapshotPlace::ofHub(HubPlace::usual()) : SnapshotPlace::here());
    if (what == "add"_L1)
        return add(parser, destinations);
    if (what == "list"_L1 && words.size() == 1)
        return list(parser, destinations);
    if (what == "copy"_L1 && words.size() <= 2)
        return copy(destinations, words.value(1));
    if (what == "test"_L1 && words.size() == 2)
        return test(parser, destinations, words.at(1));
    if (what == "remove"_L1 && words.size() == 2) {
        if (const auto removed = destinations.remove(words.at(1)); !removed)
            return fail(removed.error());
        printLine(stdout, u"The snapshots go to %1 no more. What is kept there stays, for its recovery key to restore."_s.arg(words.at(1)));
        return 0;
    }
    printLine(stderr, parser.helpText().trimmed());
    return 1;
}
