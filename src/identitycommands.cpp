#include "identitycommands.h"

#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <cstdio>
#include <print>
#include <tuple>

#include "library.h"
#include "person.h"
#include "settings.h"
#include "spaces.h"
#include "syncidentity.h"

namespace {

using namespace Qt::StringLiterals;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

// The computers paired into the personal space of the library in `root`,
// this one first; none when there is no library there yet, or one this
// command would have to bring to a newer format first.
[[nodiscard]] QList<Spaces::Computer> pairedComputers(const QString &root, QString &thisOne) {
    Library library(root);
    library.keepFormat();
    if (!QFileInfo::exists(library.databasePath()) || !library.open())
        return {};
    thisOne = Spaces::deviceId(library);
    QList<Spaces::Computer> computers = Spaces::computers(library);
    std::ranges::stable_partition(computers, [&thisOne](const Spaces::Computer &computer) { return computer.device == thisOne; });
    return computers;
}

// A device id of its own for the library in `root` (#209): for a copy of a
// library, which a sync refuses while it has the id of the one it came from.
int newDevice(const QString &root, bool json) {
    Library library(root);
    if (QString error; !QFileInfo::exists(library.databasePath()) || !library.open(&error)) {
        printLine(stderr, error.isEmpty() ? u"There is no library in %1."_s.arg(root) : error);
        return 1;
    }
    const SyncIdentity::Renewed renewed = SyncIdentity::renew(library);
    if (json)
        printLine(stdout,
                  QString::fromUtf8(
                      QJsonDocument(QJsonObject{{u"device"_s, renewed.now}, {u"was"_s, renewed.was}}).toJson(QJsonDocument::Indented))
                      .trimmed());
    else
        printLine(stdout, u"This library's device id is %1 now, one of its own; it was %2. What it wrote stays under the old one, "
                          "and from now on it syncs as a computer of its own."_s.arg(renewed.now, renewed.was));
    return 0;
}

[[nodiscard]] QString computerLine(const Spaces::Computer &computer, const QString &thisOne) {
    const QString name = computer.name.isEmpty() ? u"(a computer not named yet)"_s : computer.name;
    return u"  %1  %2%3"_s.arg(name, computer.device, computer.device == thisOne ? u"  (this computer)"_s : QString());
}

} // namespace

int runIdentityCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"Print your id and its fingerprint: the public half of the key pair that signs everything you write in your "
        "library's log, and the computers of yours paired into your personal space. The key is made the first time it is "
        "asked for and kept in your Omabook data folder, readable only by you; the secret half leaves this computer only "
        "to another of your own, over ssh, when a sync first pairs the two, so all of them are one person. Compare "
        "fingerprints by eye, or read them out, to be sure an id is whose it says."_s);
    parser.addHelpOption();
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"export"_s, u"Print only your id (the public key), to give to someone."_s});
    parser.addOption({u"library"_s,
                      u"Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use)."_s,
                      u"folder"_s});
    parser.addOption({u"new-device"_s,
                      u"Give the library a device id of its own: for a copy of a library, which a sync refuses while it has the "
                      "id of the one it came from. A library restored from a snapshot is given one by itself."_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook identity"_s;
    parser.process(rest);
    if (!parser.positionalArguments().isEmpty()) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const QString root = parser.isSet(u"library"_s) ? parser.value(u"library"_s) : Settings().libraryPath();
    if (parser.isSet(u"new-device"_s))
        return newDevice(root, parser.isSet(u"json"_s));
    PersonFile kept;
    const Person *person = kept.get(root);
    if (person == nullptr) {
        printLine(stderr, kept.problem());
        return 1;
    }
    if (parser.isSet(u"export"_s)) {
        printLine(stdout, person->exportPublicKey());
        return 0;
    }
    QString thisOne;
    const QList<Spaces::Computer> computers = pairedComputers(root, thisOne);
    if (parser.isSet(u"json"_s)) {
        QJsonArray paired;
        for (const Spaces::Computer &computer : computers)
            paired << QJsonObject{{u"device"_s, computer.device}, {u"name"_s, computer.name}, {u"this"_s, computer.device == thisOne}};
        printLine(stdout, QString::fromUtf8(QJsonDocument(QJsonObject{{u"id"_s, person->id()},
                                                                      {u"fingerprint"_s, person->fingerprint()},
                                                                      {u"key_file"_s, kept.file()},
                                                                      {u"computers"_s, paired}})
                                                .toJson(QJsonDocument::Indented))
                              .trimmed());
        return 0;
    }
    QStringList lines{u"Your id:      %1\nFingerprint:  %2\nKey file:     %3"_s.arg(person->id(), person->fingerprint(), kept.file())};
    if (computers.size() > 1) {
        lines << u"Your computers, paired into your personal space:"_s;
        for (const Spaces::Computer &computer : computers)
            lines << computerLine(computer, thisOne);
    } else if (!computers.isEmpty()) {
        lines << u"No other computer is paired yet: the first omabook sync with one of yours pairs the two."_s;
    }
    printLine(stdout, lines.join(u'\n'));
    return 0;
}
