#include "collectionimport.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cstdio>
#include <print>
#include <tuple>
#include <utility>

#include "following.h"
#include "githublink.h"
#include "library.h"
#include "spacefiles.h"
#include "subscriptions.h"

using namespace Qt::StringLiterals;

namespace {

void say(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = std::fflush(stream); // Nothing better to do when the terminal is gone.
}

int fail(const QString &why) {
    say(stderr, why);
    return 1;
}

// What a command did, as JSON and as text.
using Shown = std::expected<std::pair<QJsonObject, QString>, QString>;

int show(const Shown &shown, bool json) {
    if (!shown)
        return fail(shown.error());
    say(stdout, json ? QString::fromUtf8(QJsonDocument(shown->first).toJson(QJsonDocument::Indented)).trimmed() : shown->second);
    return 0;
}

// A file is imported as it is; anything else a transport knows is fetched by it.
Shown subscribe(Library &library, const QString &what, const Subscriptions::Options &options) {
    GitHubLink github;
    const bool link = !QFileInfo::exists(what) && github.handles(what);
    std::expected<Subscriptions::Report, QString> report = std::unexpected(QString());
    if (link) {
        report = Following::subscribe(library, what, github, options);
    } else {
        const auto snapshot = Subscriptions::fetch(what);
        report = snapshot ? Subscriptions::subscribe(library, *snapshot, options)
                          : std::expected<Subscriptions::Report, QString>(std::unexpected(snapshot.error()));
        if (report && !options.dryRun)
            SpaceFiles::takenFrom(library, report->space.space, what); // its books' files, if it lets them travel, are beside it
    }
    if (!report)
        return std::unexpected(report.error());
    QString text = Subscriptions::describe(*report).join(u'\n');
    if (link && !options.dryRun)
        text += u"\nIt is followed from %1: omabook collections import --follow \"%2\" checks it for a newer version."_s.arg(
            what.trimmed(), report->space.name);
    return std::pair{Subscriptions::toJson(*report), text};
}

int follow(Library &library, const QString &which, bool take, bool json) {
    GitHubLink github;
    const auto offers = Following::check(library, which, {&github}, take);
    if (!offers)
        return fail(offers.error());
    if (const int shown = show(std::pair{Following::toJson(*offers), Following::describe(*offers).join(u'\n')}, json); shown != 0)
        return shown;
    return std::ranges::any_of(*offers, [](const Following::Offer &offer) { return !offer.problem.isEmpty(); }) ? 1 : 0;
}

} // namespace

void CollectionImport::addOptions(QCommandLineParser &parser) {
    parser.addOption(
        {u"as"_s, u"import: the collection's name here, instead of the shared one's; share: the name you go by in it."_s, u"name"_s});
    parser.addOption({u"dry-run"_s, u"import, share: say what it would do, and change nothing."_s});
    parser.addOption({u"follow"_s, u"import: check the link a shared collection was imported from for a newer version (all of them, "
                                   "or the one named), and say what it would change."_s});
    parser.addOption({u"yes"_s, u"import --follow: take the newer version in."_s});
}

int CollectionImport::run(Library &library, const QCommandLineParser &parser, const QStringList &words, bool json) {
    const QString what = words.mid(1).join(u' ');
    if (words.value(0) == "unsubscribe"_L1) {
        if (what.isEmpty())
            return fail(u"Name the shared collection to drop: omabook collections unsubscribe <name|space>."_s);
        const auto dropped = Subscriptions::unsubscribe(library, what);
        if (!dropped)
            return fail(dropped.error());
        return show(std::pair{Subscriptions::toJson(*dropped), Subscriptions::describe(*dropped)}, json);
    }
    if (parser.isSet(u"follow"_s))
        return follow(library, what, parser.isSet(u"yes"_s) && !parser.isSet(u"dry-run"_s), json);
    if (what.isEmpty())
        return fail(u"Name what to import: omabook collections import <file|link>, a .omabook-space file, a gist or a file of a "
                    "GitHub repository."_s);
    return show(subscribe(library, what, {.as = parser.value(u"as"_s), .dryRun = parser.isSet(u"dry-run"_s)}), json);
}
