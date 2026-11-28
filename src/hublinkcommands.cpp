#include "hublinkcommands.h"

#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <cstdio>
#include <print>
#include <tuple>

#include "hubautocommands.h"
#include "hubautosync.h"
#include "hubcomputers.h"
#include "hublinks.h"
#include "hubplace.h"
#include "hubrequests.h"
#include "person.h"
#include "settings.h"
#include "synccommands.h"

using namespace Qt::StringLiterals;

namespace {

// A public line is short; more than this on the input is no key.
constexpr qint64 longestKey = 8192;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

int failLine(const QString &why) {
    printLine(stderr, why);
    return 1;
}

int add(const QString &host, const QCommandLineParser &parser) {
    PersonFile kept;
    const Person *person = kept.get(Settings().libraryPath());
    if (person == nullptr)
        return failLine(kept.problem());
    const QString name = parser.isSet(u"name"_s) ? parser.value(u"name"_s) : HubLinks::thisComputer();
    const auto linked = HubLinks().add(host, name, *person);
    if (!linked)
        return failLine(linked.error().text());
    printLine(stdout,
              u"This computer is linked to the hub at %1 as %2. Its key there (%3) opens only the hub's session, never a shell."_s.arg(
                  host, linked->computer, linked->fingerprint));
    return 0;
}

int remove(const QString &host) {
    const auto removed = HubLinks().remove(host);
    if (!removed)
        return failLine(removed.error().text());
    printLine(stdout, u"This computer (%1) is no longer linked to the hub at %2: its key there is gone."_s.arg(removed->computer, host));
    return 0;
}

void printComputers(const QJsonArray &computers) {
    if (computers.isEmpty())
        printLine(stdout, u"No computers are linked to the hub."_s);
    for (const auto &value : computers) {
        const QJsonObject computer = value.toObject();
        printLine(stdout, u"  %1  %2  linked %3%4"_s.arg(computer["name"_L1].toString(), computer["fingerprint"_L1].toString(),
                                                         computer["linked"_L1].toString(),
                                                         computer["this"_L1].toBool() ? u"  (this computer)"_s : QString()));
    }
}

int computers(const QStringList &words, const QCommandLineParser &parser) {
    QJsonArray listed;
    QString host = words.value(0);
    // On a computer that is no hub, the hub it is linked to is meant, as by `hub sync` (#294).
    if (host.isEmpty() && HubPlace::usual().libraries().isEmpty()) {
        const QList<HubLinks::Hub> linked = HubLinks().hubs();
        if (linked.size() > 1)
            return failLine(u"This computer is linked to more than one hub: name one, omabook hub computers <host>."_s);
        if (!linked.isEmpty())
            host = linked.first().host;
    }
    if (host.isEmpty()) {
        for (const HubComputers::Computer &computer : HubComputers::usual(HubPlace::usual()).computers())
            listed.append(QJsonObject{
                {"name", computer.name}, {"fingerprint", computer.fingerprint}, {"linked", computer.linked.toString(Qt::ISODate)}});
    } else {
        const auto answer = HubLinks().ask(host, {{"request", "computers"}});
        if (!answer)
            return failLine(answer.error().text());
        listed = answer->value("computers"_L1).toArray();
    }
    if (parser.isSet(u"json"_s))
        printLine(stdout, QString::fromUtf8(QJsonDocument(listed).toJson(QJsonDocument::Indented)).trimmed());
    else
        printComputers(listed);
    return 0;
}

// On the hub, over the person's own ssh: the computer's key from the input.
int allow(const QCommandLineParser &parser) {
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 1;
    const HubComputers::Asked asked{.name = parser.value(u"name"_s),
                                    .key = QString::fromUtf8(input.read(longestKey)).trimmed(),
                                    .identity = parser.value(u"identity"_s),
                                    .signature = parser.value(u"signature"_s),
                                    .former = parser.value(u"former"_s)};
    HubComputers computers = HubComputers::usual(HubPlace::usual());
    const auto linked = computers.link(asked);
    const QJsonObject answer = linked ? QJsonObject{{"ok", true}, {"computer", linked->name}, {"fingerprint", linked->fingerprint}}
                                      : HubRequests::refusal(linked.error());
    printLine(stdout, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
    return linked ? 0 : 1;
}

int forget(const QStringList &words) {
    if (words.size() == 2) {
        // On a computer: another of the person's, off the hub at the first word.
        const auto gone = HubLinks().forget(words.first(), words.last());
        if (!gone)
            return failLine(gone.error().text());
        printLine(stdout, u"The computer %1 is no longer linked to the hub at %2: its key is gone."_s.arg(*gone, words.first()));
        return 0;
    }
    const QString &computer = words.first();
    HubComputers computers = HubComputers::usual(HubPlace::usual());
    const auto gone = computers.unlink(computer);
    if (!gone)
        return failLine(gone.error().text());
    printLine(stdout, u"The computer %1 is no longer linked to this hub: its key is gone."_s.arg(gone->name));
    return 0;
}

} // namespace

int runHubSync(const QStringList &arguments) {
    // `omabook hub sync ...` is `omabook sync ...` with a hub, the one this computer is linked to when it names none.
    QStringList given = arguments;
    given.removeAt(1);
    // Automatic sync's switches (#191), the background service's among them, which names no hub.
    if (isHubAutoCommand(given) && !hubAutoNamesAHub(given))
        return runHubAuto(given);
    const HubLinks links;
    const QString named = given.value(2);
    if (named.isEmpty() || named.startsWith(u'-')) {
        const QList<HubLinks::Hub> hubs = links.hubs();
        if (hubs.size() != 1)
            return failLine(hubs.isEmpty() ? u"This computer is linked to no hub. Link it: omabook hub add <host>"_s
                                           : u"This computer is linked to more than one hub: name the one to sync with."_s);
        given.insert(2, hubs.first().host);
    } else if (const QString host = named.section(u':', 0, 0); !links.isLinked(host)) {
        return failLine(Failure(Failure::Code::HubNotLinked, {host}).text());
    }
    if (isHubAutoCommand(given))
        return runHubAuto(given);
    // By hand, after the background service's sync when it is running one; once
    // through, automatic sync goes on from it, a refusal seen to (#191).
    HubAutoSync automatic(HubLinks::usualFolder());
    const auto held = automatic.hold(true);
    const QDateTime began = QDateTime::currentDateTimeUtc();
    const int synced = runSyncCli(given);
    if (synced == 0 && !given.contains(u"--dry-run"_s))
        automatic.byHand(given.value(2).section(u':', 0, 0), began);
    return synced;
}

bool isHubLinkCommand(const QString &what) {
    return QStringList{u"add"_s, u"remove"_s, u"computers"_s, u"allow"_s, u"forget"_s}.contains(what);
}

int runHubLink(const QString &what, const QStringList &words, const QCommandLineParser &parser) {
    const bool oneWord = words.size() == 1;
    if (what == u"computers"_s && words.size() <= 1)
        return computers(words, parser);
    if (what == u"allow"_s && words.isEmpty())
        return allow(parser);
    if (what == u"add"_s && oneWord)
        return add(words.first(), parser);
    if (what == u"remove"_s && oneWord)
        return remove(words.first());
    if (what == u"forget"_s && (oneWord || words.size() == 2))
        return forget(words);
    return failLine(parser.helpText().trimmed());
}
