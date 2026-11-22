#include "snapshothealthcommands.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>
#include <print>
#include <tuple>
#include <utility>

#include "desktopnotice.h"
#include "hubplace.h"
#include "library.h"
#include "settings.h"
#include "snapshothealth.h"

namespace {

using namespace Qt::StringLiterals;
using Libraries = QList<std::pair<QString, QString>>;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

[[nodiscard]] std::pair<QString, QString> namedOf(const Library &library) {
    return {Snapshots::libraryTag(library), QFileInfo(library.root()).fileName()};
}

// The libraries whose snapshots are looked after: the hub's, or the one of the command.
[[nodiscard]] Libraries librariesOf(const QCommandLineParser &parser) {
    if (parser.isSet(u"hub"_s)) {
        Libraries libraries;
        const QDir folder(HubPlace::usual().libraries());
        for (const QString &name : folder.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            if (const QString root = folder.filePath(name); QFileInfo::exists(Library::databaseIn(root)))
                libraries << namedOf(Library(root));
        return libraries;
    }
    return {namedOf(Library(parser.isSet(u"library"_s) ? parser.value(u"library"_s) : Settings().libraryPath()))};
}

// Each new warning, and each that stood a week since it was told: on the desktop, and on standard error.
void tell(const SnapshotHealth &health, const QList<SnapshotWarnings::Warning> &warnings, const QDateTime &now) {
    for (const auto &warning : health.toTell(warnings, now)) {
        const QString text = warning.why.text();
        printLine(stderr, text);
        std::ignore = DesktopNotice::show(u"Omabook snapshots"_s, text); // Said on standard error either way.
    }
}

[[nodiscard]] QString whenText(const QDateTime &time) { return time.isValid() ? time.toString(u"yyyy-MM-dd HH:mm"_s) : u"never"_s; }

[[nodiscard]] QString mediumText(SnapshotWarnings::Medium medium) {
    switch (medium) {
    case SnapshotWarnings::Medium::Own:
        return u"this computer's"_s;
    case SnapshotWarnings::Medium::Disk:
        return u"a second disk"_s;
    case SnapshotWarnings::Medium::Away:
        return u"away from home"_s;
    }
    return {};
}

int print(const QCommandLineParser &parser, const SnapshotHealth &health, const QList<SnapshotWarnings::Warning> &warnings) {
    if (parser.isSet(u"json"_s)) {
        printLine(stdout, QString::fromUtf8(QJsonDocument(health.json(warnings)).toJson(QJsonDocument::Indented)).trimmed());
        return warnings.isEmpty() ? 0 : 1;
    }
    printLine(stdout, u"The snapshots are kept in:"_s);
    for (const auto &repository : health.repositories()) {
        QDateTime newest;
        for (const auto &[tag, time] : repository.newest)
            newest = std::max(newest, time);
        printLine(stdout, u"  %1 (%2): newest snapshot %3, checked %4, read back %5"_s.arg(
                              repository.shown(), mediumText(repository.medium), whenText(newest), whenText(repository.checked),
                              whenText(repository.dataChecked)));
    }
    if (warnings.isEmpty()) {
        printLine(stdout, u"All is well with them."_s);
        return 0;
    }
    printLine(stdout, u"\nTo see to:"_s);
    for (const auto &warning : warnings)
        printLine(stdout, u"  - "_s + warning.why.text());
    return 1;
}

} // namespace

int runSnapshotHealthCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"How the snapshots stand. status says, for each place they are kept, when the newest was taken and when they were "
        "last checked, and what to see to: a snapshot two days old, a nightly one or a check that failed, a password missing, "
        "a second disk not there or nearly full. check has restic check them now (with --read-data, reading them back too); "
        "the hub checks its own every week, and reads them back every month."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(u"what"_s, u"status or check."_s, u"<status|check>"_s);
    parser.addOption({u"json"_s, u"Print it as JSON."_s});
    parser.addOption({u"library"_s, u"The library in this folder instead of the current one."_s, u"folder"_s});
    parser.addOption({u"read-data"_s, u"check: read the snapshots' data back too, not only how they are kept."_s});
    parser.addOption({u"hub"_s, u"The snapshots of the hub on this computer, and its libraries."_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook snapshot"_s;
    parser.process(rest);
    const QString what = parser.positionalArguments().value(0);
    if (parser.positionalArguments().size() != 1 || (what != "status"_L1 && what != "check"_L1)) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const Snapshots snapshots(parser.isSet(u"hub"_s) ? SnapshotPlace::ofHub(HubPlace::usual()) : SnapshotPlace::here());
    const SnapshotHealth health(snapshots);
    if (!health.chosen()) {
        if (parser.isSet(u"json"_s))
            printLine(stdout, QString::fromUtf8(QJsonDocument(health.json({})).toJson(QJsonDocument::Indented)).trimmed());
        else
            printLine(stdout, Failure(Failure::Code::SnapshotsNotSetUp).text());
        return what == "check"_L1 ? 1 : 0;
    }
    const QDateTime now = QDateTime::currentDateTime();
    if (what == "check"_L1)
        health.check(parser.isSet(u"read-data"_s) ? SnapshotHealth::Depth::Data : SnapshotHealth::Depth::Light, now);
    health.look(now);
    const auto warnings = health.warnings(librariesOf(parser), now);
    if (what == "check"_L1) {
        tell(health, warnings, now);
        if (warnings.isEmpty() && !parser.isSet(u"json"_s))
            printLine(stdout, u"Checked: the snapshots are well."_s);
    }
    return print(parser, health, warnings);
}

void afterNightly(const Snapshots &snapshots, const Library &library, const std::expected<void, Failure> &outcome, const QDateTime &now) {
    const SnapshotHealth health(snapshots);
    if (!health.chosen())
        return;
    health.noteSnapshot(outcome, now);
    health.look(now);
    tell(health, health.warnings({namedOf(library)}, now), now);
}
