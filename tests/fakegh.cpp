// A stand-in for GitHub's command line, gh, for the tests of sharing to a
// gist: `gh api [--method <method>] <path> [--input -]`, as Omabook calls it,
// sent to the fake GitHub (fakegithub.h) that OMABOOK_GITHUB_API names, and
// answered the way gh answers: the API's answer on standard output, and on
// a failure what went wrong on standard error. FAKE_GH_SIGNED_OUT makes it a
// gh nobody signed in to. Nothing here reaches the network.

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <cstdio>

namespace {

int failWith(const QByteArray &said, int code) {
    std::fputs(said.constData(), stderr);
    std::fputc('\n', stderr);
    return code;
}

} // namespace

int main(int argc, char **argv) {
    const QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsSet("FAKE_GH_SIGNED_OUT"))
        return failWith("To get started with GitHub CLI, please run:  gh auth login\n"
                        "Alternatively, populate the GH_TOKEN environment variable with a GitHub API authentication token.",
                        4);
    QStringList arguments = QCoreApplication::arguments().mid(1);
    if (arguments.value(0) != "api")
        return failWith("unknown command: this gh only answers gh api", 1);
    QByteArray method = "GET", body;
    QString path;
    for (qsizetype at = 1; at < arguments.size(); ++at) {
        const QString argument = arguments.at(at);
        if (argument == "--method" || argument == "-X")
            method = arguments.value(++at).toLatin1();
        else if (argument == "--input" || argument == "-H")
            ++at; // what follows --input is "-": standard input, read below
        else
            path = argument;
    }
    // Standard input, all of it, when asked to send it.
    if (arguments.contains("--input")) {
        QFile input;
        if (input.open(stdin, QIODevice::ReadOnly))
            body = input.readAll();
    }
    const QString api = qEnvironmentVariable("OMABOOK_GITHUB_API");
    if (api.isEmpty())
        return failWith("error connecting to api.github.com\ncheck your internet connection or https://githubstatus.com", 1);

    QNetworkAccessManager network;
    QNetworkRequest request(QUrl(api + "/" + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = network.sendCustomRequest(request, method, body);
    QObject::connect(reply, &QNetworkReply::finished, &app, &QCoreApplication::quit);
    QCoreApplication::exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray answer = reply->readAll();
    if (status == 0)
        return failWith(
            "error connecting to " + QUrl(api).host().toLatin1() + "\ncheck your internet connection or https://githubstatus.com", 1);
    std::fwrite(answer.constData(), 1, answer.size(), stdout);
    if (status >= 400)
        return failWith("gh: " + QJsonDocument::fromJson(answer).object().value("message").toString().toUtf8() + " (HTTP "
                            + QByteArray::number(status) + ")",
                        1);
    return 0;
}
