#include "snapshotcommands.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>
#include <cstdio>
#include <print>
#include <tuple>

#include "hubplace.h"
#include "library.h"
#include "librarymigrations.h"
#include "settings.h"

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

// A command that changes much at once: by its name, the word after it, or an option it is given.
struct Risky {
    QLatin1StringView command, word, option;
    SnapshotReason reason;
};

constexpr std::array risky{
    Risky{.command = "sync"_L1, .word = {}, .option = {}, .reason = SnapshotReason::Sync},
    Risky{.command = "spaces"_L1, .word = "sync"_L1, .option = {}, .reason = SnapshotReason::Sync},
    Risky{.command = "tidy"_L1, .word = {}, .option = "--apply"_L1, .reason = SnapshotReason::Tidy},
    Risky{.command = "tidy"_L1, .word = {}, .option = "--pick"_L1, .reason = SnapshotReason::Tidy},
    Risky{.command = "calibre"_L1, .word = "import"_L1, .option = {}, .reason = SnapshotReason::Import},
    Risky{.command = "goodreads"_L1, .word = "import"_L1, .option = {}, .reason = SnapshotReason::Import},
    Risky{.command = "goodreads"_L1, .word = "sync"_L1, .option = {}, .reason = SnapshotReason::Import},
    Risky{.command = "openlibrary"_L1, .word = "sync"_L1, .option = {}, .reason = SnapshotReason::Import},
    Risky{.command = "opds"_L1, .word = {}, .option = "--add"_L1, .reason = SnapshotReason::Import},
};

// Options that only look, and change nothing.
constexpr std::array looks{"--help"_L1, "-h"_L1, "--dry-run"_L1, "--status"_L1};

// Options of the risky commands that take a value, which is no word of the command's.
constexpr std::array valued{"--library"_L1, "--model"_L1,     "--user"_L1,
                            "--pick"_L1,    "--details"_L1,   "--unlink"_L1,
                            "--add"_L1,     "--replicate"_L1, "--take-plugin-files"_L1};

[[nodiscard]] bool given(const QStringList &arguments, QLatin1StringView option) {
    return std::ranges::any_of(arguments,
                               [option](const QString &word) { return word == option || word.startsWith(QString(option) + u'='); });
}

// The first word after the command's name that is no option, nor an option's value.
[[nodiscard]] QString firstWord(const QStringList &arguments) {
    for (qsizetype i = 2; i < arguments.size(); ++i) {
        const QString &word = arguments.at(i);
        if (std::ranges::contains(valued, word))
            ++i;
        else if (!word.startsWith(u'-'))
            return word;
    }
    return {};
}

// The library a command works on: --library's, or the one in use.
[[nodiscard]] QString libraryOf(const QStringList &arguments) {
    for (qsizetype i = 2; i < arguments.size(); ++i) {
        if (arguments.at(i) == "--library"_L1 && i + 1 < arguments.size())
            return arguments.at(i + 1);
        if (arguments.at(i).startsWith("--library="_L1))
            return arguments.at(i).mid(10);
    }
    return Settings().libraryPath();
}

[[nodiscard]] QJsonObject snapshotJson(const Restic::Snapshot &snapshot) {
    return {{u"id"_s, snapshot.id},
            {u"time"_s, snapshot.time.toString(Qt::ISODate)},
            {u"kind"_s, snapshot.tags.contains(u"nightly"_s) ? u"nightly"_s : u"moment"_s},
            {u"reason"_s, snapshot.tagged(u"reason:"_s)}};
}

int setUp(const QCommandLineParser &parser, Snapshots &snapshots) {
    const auto key = snapshots.setUp(parser.value(u"repository"_s));
    if (!key)
        return fail(key.error());
    const QString repository = snapshots.place().repository.location;
    if (parser.isSet(u"json"_s)) {
        printJson({{u"repository"_s, repository}, {u"recovery_key"_s, *key}});
        return 0;
    }
    printLine(stdout, u"Snapshots are set up in %1.\n\nThe recovery key, shown this once:\n\n    %2\n\n"
                      "Keep it away from this computer: printed, or in a password manager. Without it no snapshot "
                      "can be restored, on this computer or any other."_s.arg(repository, *key));
    return 0;
}

int list(const QCommandLineParser &parser, const Snapshots &snapshots, const Library &library) {
    const auto found = snapshots.list(library);
    if (!found)
        return fail(found.error());
    if (parser.isSet(u"json"_s)) {
        QJsonArray array;
        for (const Restic::Snapshot &snapshot : *found)
            array << snapshotJson(snapshot);
        printJson({{u"library"_s, library.root()}, {u"snapshots"_s, array}});
        return 0;
    }
    if (found->isEmpty())
        printLine(stdout, u"No snapshots of this library yet."_s);
    for (const Restic::Snapshot &snapshot : *found) {
        const QJsonObject json = snapshotJson(snapshot);
        printLine(stdout, u"%1  %2  %3 (%4)"_s.arg(snapshot.time.toString(u"yyyy-MM-dd HH:mm"_s), snapshot.shortId(),
                                                   json.value("kind"_L1).toString(), json.value("reason"_L1).toString()));
    }
    return 0;
}

int said(const QCommandLineParser &parser, const std::optional<Snapshots::Taken> &taken) {
    if (parser.isSet(u"json"_s)) {
        printJson(taken ? QJsonObject{{u"id"_s, taken->id}, {u"unchanged"_s, taken->unchanged}} : QJsonObject{{u"due"_s, false}});
        return 0;
    }
    if (!taken)
        printLine(stdout, u"No nightly snapshot is due yet."_s);
    else if (taken->unchanged)
        printLine(stdout, u"Nothing changed since snapshot %1, so none was taken."_s.arg(taken->id.left(8)));
    else
        printLine(stdout, u"Snapshot %1 taken."_s.arg(taken->id.left(8)));
    return 0;
}

} // namespace

int runSnapshotCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"Snapshots of the library, made with restic: encrypted, each keeping only what changed, kept to go back to. "
        "setup makes the place for them and the recovery key, shown once; now takes one (when something changed); "
        "list lists them; nightly takes the night's, when it is due, and thins the older ones: every one of 14 days, "
        "then one a day for 7 days, a week for 4 weeks, a month for 12 months, and one of every year. A sync, an "
        "import or a tidy that changes something takes a moment snapshot first, once snapshots are set up."_s);
    parser.addHelpOption();
    parser.addPositionalArgument(u"what"_s, u"setup, now, list or nightly."_s, u"<setup|now|list|nightly>"_s);
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"library"_s,
                      u"Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use)."_s,
                      u"folder"_s});
    parser.addOption({u"repository"_s,
                      u"setup: where to keep the snapshots, a folder or anything restic takes (sftp:host:/path, "
                      "s3:..., rclone:...); a folder in your Omabook data when not given."_s,
                      u"where"_s});
    parser.addOption({u"hub"_s, u"setup: set them up for the hub on this computer (omabook hub run), its password one of the hub's "
                                "secrets; its daemon takes them from then on."_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook snapshot"_s;
    parser.process(rest);
    const QString what = parser.positionalArguments().value(0);
    if (parser.positionalArguments().size() != 1 || !QStringList{u"setup"_s, u"now"_s, u"list"_s, u"nightly"_s}.contains(what)) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    if (parser.isSet(u"hub"_s) && what != "setup"_L1) {
        printLine(stderr, u"The hub's daemon takes its snapshots; --hub only sets them up."_s);
        return 1;
    }
    Snapshots snapshots(parser.isSet(u"hub"_s) ? SnapshotPlace::ofHub(HubPlace::usual()) : SnapshotPlace::here());
    if (what == "setup"_L1)
        return setUp(parser, snapshots);
    if (!snapshots.ready())
        return fail(Failure(Failure::Code::SnapshotsNotSetUp));
    Library library(parser.isSet(u"library"_s) ? parser.value(u"library"_s) : Settings().libraryPath());
    library.onBringingUpToDate(LibraryMigrations::sayOnStandardError);
    if (QString error; !library.open(&error)) {
        printLine(stderr, error);
        return 1;
    }
    if (what == "list"_L1)
        return list(parser, snapshots, library);
    if (what == "now"_L1) {
        const auto taken = snapshots.take(library, SnapshotReason::Asked);
        return taken ? said(parser, *taken) : fail(taken.error());
    }
    const auto taken = snapshots.nightly(library, QDateTime::currentDateTime());
    return taken ? said(parser, *taken) : fail(taken.error());
}

std::optional<SnapshotReason> riskyMoment(const QStringList &arguments) {
    if (std::ranges::any_of(looks, [&arguments](QLatin1StringView look) { return given(arguments, look); }))
        return std::nullopt;
    const QString command = arguments.value(1);
    const QString word = firstWord(arguments);
    const auto *const found = std::ranges::find_if(risky, [&](const Risky &one) {
        return command == one.command && (one.word.isEmpty() || word == one.word) && (one.option.isEmpty() || given(arguments, one.option));
    });
    return found == risky.end() ? std::nullopt : std::optional(found->reason);
}

void snapshotBeforeRisky(const QStringList &arguments) {
    const auto moment = riskyMoment(arguments);
    if (!moment)
        return;
    const Snapshots snapshots(SnapshotPlace::here());
    if (!snapshots.ready())
        return;
    const QString root = libraryOf(arguments);
    if (!QFileInfo::exists(Library::databaseIn(root)))
        return;
    // As it is: a library to bring up to date is copied by that (.backups).
    Library library(root);
    library.keepFormat();
    if (!library.open())
        return;
    const auto taken = snapshots.before(library, *moment);
    if (!taken)
        printLine(stderr, u"No moment snapshot could be taken first, so this goes on without one: "_s + taken.error().text());
    else if (*taken && !(*taken)->unchanged)
        printLine(stderr, u"A moment snapshot of the library was taken first: %1."_s.arg((*taken)->id.left(8)));
}
