#include "aicommands.h"

#include <QCommandLineParser>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <cstdio>
#include <print>
#include <tuple>

#include "ai.h"
#include "aiendpoint.h"
#include "openaiprovider.h"
#include "secretstore.h"
#include "settings.h"

namespace {

using namespace Qt::StringLiterals;

void printLine(FILE *stream, const QString &text) {
    std::println(stream, "{}", qPrintable(text));
    std::ignore = fflush(stream); // Nothing better to do when the terminal is gone.
}

void printJson(const QJsonObject &object) {
    printLine(stdout, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Indented)).trimmed());
}

int fail(const QString &why) {
    printLine(stderr, why);
    return 1;
}

QString presets() {
    QStringList ids;
    for (const AiEndpoint &endpoint : AiEndpoints::all())
        ids.append(QString::fromLatin1(endpoint.id));
    return ids.join(", "_L1);
}

// `ai use <preset> [address]`: a server becomes the AI.
int use(const QStringList &words, bool json) {
    const QString preset = words.value(1);
    if (!AiEndpoints::find(preset))
        return fail(u"Name a server: %1, and an address for a custom one."_s.arg(presets()));
    Settings settings;
    QJsonObject endpoint{{"preset", preset}};
    if (!words.value(2).isEmpty())
        endpoint.insert("base_url", words.value(2));
    settings.setAiEndpoint(endpoint);
    settings.setAiProvider(u"openai"_s);
    const QJsonObject described = Ai::describe(settings);
    if (json)
        printJson(described);
    else
        printLine(stdout, described.value("problem"_L1).toString().isEmpty()
                              ? u"The AI is now %1, at %2."_s.arg(described.value("name"_L1).toString(), AiEndpoints::baseUrl(settings))
                              : described.value("problem"_L1).toString());
    return 0;
}

// `ai models`: what the server in use lists, marking those that send the
// text on elsewhere.
int models(bool json) {
    const Settings settings;
    if (settings.aiProvider() != "openai"_L1)
        return fail(u"The AI isn't a server; choose one with: omabook ai use <preset> [address]"_s);
    const auto server = AiEndpoints::server(settings, Ai::models(settings, AiEndpoints::modelsKey(settings)).main);
    if (!server)
        return fail(server.error());
    const auto listed = OpenAiProvider::listModels(*server);
    if (!listed)
        return fail(listed.error().text());
    QJsonArray rows;
    for (const QString &model : *listed) {
        const bool elsewhere = AiEndpoints::runsElsewhere(model);
        rows.append(QJsonObject{{"name", model}, {"elsewhere", elsewhere}});
        if (!json)
            printLine(stdout, elsewhere ? model + u"\t(runs elsewhere: the text leaves the server)"_s : model);
    }
    if (json)
        printJson(QJsonObject{{"server", server->baseUrl}, {"models", rows}});
    return 0;
}

// `ai key [--forget]`: the key, read from standard input so that it is in
// no shell history, kept in the keyring for the server in use.
int key(bool forget, bool json) {
    const Settings settings;
    const QString url = AiEndpoints::baseUrl(settings);
    if (url.isEmpty())
        return fail(u"No server is set; choose one with: omabook ai use <preset> [address]"_s);
    QString given;
    if (!forget) {
        QTextStream input(stdin);
        given = input.readLine().trimmed();
        if (given.isEmpty())
            return fail(u"Give the key on standard input, such as: omabook ai key < file"_s);
    }
    if (const auto kept = AiEndpoints::keepKey(url, given, Secrets::shared()); !kept)
        return fail(kept.error());
    if (json)
        printJson(QJsonObject{{"server", url}, {"key", !forget}});
    else
        printLine(stdout, forget ? u"The key for %1 is forgotten."_s.arg(url) : u"The key for %1 is kept in the keyring."_s.arg(url));
    return 0;
}

} // namespace

int runAiCli(const QStringList &arguments) {
    QCommandLineParser parser;
    parser.setApplicationDescription(
        u"A server that speaks the OpenAI-compatible API as the AI: Omarchy's local models, Ollama, OpenAI, or any "
        "other (OpenRouter, Groq, LM Studio, llama.cpp, vLLM, ...) by its address.\n\n"
        "  use <preset> [address]   Make it the AI: omarchy-local, ollama, openai or custom\n"
        "  models                   List the models it has; those that run elsewhere are marked\n"
        "  key [--forget]           Keep its key, read from standard input, in the keyring"_s);
    parser.addHelpOption();
    parser.addOption({u"json"_s, u"Print the result as JSON."_s});
    parser.addOption({u"forget"_s, u"key: forget the key instead."_s});
    // Taken by every command; the AI is the same for every library.
    parser.addOption({u"library"_s, u"Has no effect here."_s, u"folder"_s});
    parser.addPositionalArgument(u"command"_s, u"use, models or key."_s);
    QStringList rest = arguments;
    rest.removeAt(1);
    rest[0] = u"omabook ai"_s;
    parser.process(rest);
    const QStringList words = parser.positionalArguments();
    const bool json = parser.isSet(u"json"_s);
    const QString command = words.value(0, u"models"_s);
    if (command == "use"_L1)
        return use(words, json);
    if (command == "models"_L1 && words.size() <= 1)
        return models(json);
    if (command == "key"_L1 && words.size() == 1)
        return key(parser.isSet(u"forget"_s), json);
    printLine(stderr, parser.helpText().trimmed());
    return 1;
}
