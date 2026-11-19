#include "identitycommands.h"

#include <QCommandLineParser>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <print>
#include <tuple>

#include "person.h"
#include "settings.h"

namespace {

using namespace Qt::StringLiterals;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

} // namespace

int runIdentityCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"Print your id and its fingerprint: the public half of the key pair that signs what you write in a shared space. "
        "It is made the first time it is asked for and kept beside the library, readable only by you; the secret half "
        "never leaves this computer. Compare fingerprints by eye, or read them out, to be sure an id is whose it says."_s);
    parser.addHelpOption();
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"export"_s, u"Print only your id (the public key), to give to someone."_s});
    parser.addOption({u"library"_s,
                      u"Use the library in this folder instead of the current one, for this run only "
                      "(omabook libraries --use changes the one in use)."_s,
                      u"folder"_s});
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook identity"_s;
    parser.process(rest);
    if (!parser.positionalArguments().isEmpty()) {
        printLine(stderr, parser.helpText().trimmed());
        return 1;
    }
    const QString file = Person::keyFile(parser.isSet(u"library"_s) ? parser.value(u"library"_s) : Settings().libraryPath());
    const auto person = Person::loadOrCreate(file);
    if (!person) {
        printLine(stderr, person.error());
        return 1;
    }
    if (parser.isSet(u"export"_s))
        printLine(stdout, person->exportPublicKey());
    else if (parser.isSet(u"json"_s))
        printLine(stdout,
                  QString::fromUtf8(
                      QJsonDocument(QJsonObject{{u"id"_s, person->id()}, {u"fingerprint"_s, person->fingerprint()}, {u"key_file"_s, file}})
                          .toJson(QJsonDocument::Indented))
                      .trimmed());
    else
        printLine(stdout, u"Your id:      %1\nFingerprint:  %2\nKey file:     %3"_s.arg(person->id(), person->fingerprint(), file));
    return 0;
}
