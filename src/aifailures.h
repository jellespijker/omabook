#pragma once

#include <QJsonObject>
#include <QNetworkReply>
#include <QString>

#include "failure.h"

// Why asking the AI failed, as a Failure (ADR 0005): read from what a server
// answered or a program said, once, where it is received. Whether another
// model is worth asking is then a question of the code (aboutTheModel), not of
// the words, which are the desktop's language in the window.
namespace AiFailures {

// What a server says went wrong: {"error": {"message", "code" or "type"}}, as
// OpenAI and Anthropic write it, or a plain {"error": "..."} as some others do.
struct ServerSaid {
    QString message, code;
};
[[nodiscard]] ServerSaid serverSaid(const QJsonObject &body);

// A request to a server that didn't end in an answer.
struct Reply {
    QString where;  // the server's address, as the sentences name it
    int status = 0; // its HTTP status; 0 when the network was the trouble
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QString errorString; // the network's reason
    ServerSaid said;
    QString model; // the model it was asked for
    bool keySet = false;
};
[[nodiscard]] Failure ofServer(const Reply &reply);

// A program (a harness, a command of the user's own) that stopped without an
// answer, and what it said instead. One that speaks of the model, when one was
// named, refused it.
[[nodiscard]] Failure ofProgram(const QString &name, const QString &model, const QString &said);

// Whether the failure is about which model was asked: only then may another
// model mend it. One about the network, a key or a limit would fail the same
// way again, on a dearer model.
[[nodiscard]] bool aboutTheModel(const Failure &failure);

} // namespace AiFailures
