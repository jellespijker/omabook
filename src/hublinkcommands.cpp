#include "hublinkcommands.h"

#include <QCommandLineParser>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSysInfo>
#include <cstdio>
#include <print>
#include <tuple>

#include "hubcomputers.h"
#include "hublinks.h"
#include "hubplace.h"
#include "hubrequests.h"
#include "person.h"
#include "settings.h"

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

// What the hub calls this computer unless told: its host name, as a word.
[[nodiscard]] QString thisComputer() {
    static const QRegularExpression notInAWord(u"[^A-Za-z0-9._-]+"_s);
    const QString name = QSysInfo::machineHostName().section('.', 0, 0).replace(notInAWord, u"-"_s).left(63);
    return HubComputers::isName(name) ? name : u"computer"_s;
}

int add(const QString &host, const QCommandLineParser &parser) {
    PersonFile kept;
    const Person *person = kept.get(Settings().libraryPath());
    if (person == nullptr)
        return failLine(kept.problem());
    const QString name = parser.isSet(u"name"_s) ? parser.value(u"name"_s) : thisComputer();
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
    if (words.isEmpty()) {
        for (const HubComputers::Computer &computer : HubComputers::usual(HubPlace::usual()).computers())
            listed.append(QJsonObject{
                {"name", computer.name}, {"fingerprint", computer.fingerprint}, {"linked", computer.linked.toString(Qt::ISODate)}});
    } else {
        const auto answer = HubLinks().ask(words.first(), {{"request", "computers"}});
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
                                    .signature = parser.value(u"signature"_s)};
    HubComputers computers = HubComputers::usual(HubPlace::usual());
    const auto linked = computers.link(asked);
    const QJsonObject answer = linked ? QJsonObject{{"ok", true}, {"computer", linked->name}, {"fingerprint", linked->fingerprint}}
                                      : HubRequests::refusal(linked.error());
    printLine(stdout, QString::fromUtf8(QJsonDocument(answer).toJson(QJsonDocument::Compact)));
    return linked ? 0 : 1;
}

int forget(const QString &computer) {
    HubComputers computers = HubComputers::usual(HubPlace::usual());
    const auto gone = computers.unlink(computer);
    if (!gone)
        return failLine(gone.error().text());
    printLine(stdout, u"The computer %1 is no longer linked to this hub: its key is gone."_s.arg(gone->name));
    return 0;
}

} // namespace

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
    if (what == u"forget"_s && oneWord)
        return forget(words.first());
    return failLine(parser.helpText().trimmed());
}
