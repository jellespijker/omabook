#include "aifailures.h"

#include <QJsonValue>
#include <algorithm>
#include <array>

namespace {

using namespace Qt::StringLiterals;
using Code = Failure::Code;

// The servers' words for being busy or over a limit (Anthropic's types, OpenAI's codes).
[[nodiscard]] bool overALimit(const AiFailures::Reply &reply) {
    static constexpr std::array codes{"rate_limit_error"_L1, "overloaded_error"_L1, "rate_limit_exceeded"_L1, "insufficient_quota"_L1};
    return reply.status == 429 || reply.status == 503 || reply.status == 529
           || std::ranges::any_of(codes, [&reply](QLatin1StringView code) { return reply.said.code == code; });
}

// OpenAI says model_not_found, Anthropic not_found_error; Ollama answers 404 with "model ... not found".
[[nodiscard]] bool noSuchModel(const AiFailures::Reply &reply) {
    const QString &message = reply.said.message;
    return reply.said.code == "model_not_found"_L1 || (reply.status == 404 && reply.said.code == "not_found_error"_L1)
           || (reply.status == 404 && (message.isEmpty() || message.contains("model"_L1, Qt::CaseInsensitive)));
}

} // namespace

AiFailures::ServerSaid AiFailures::serverSaid(const QJsonObject &body) {
    const QJsonValue error = body.value("error"_L1);
    if (error.isString())
        return {.message = error.toString(), .code = {}};
    const QJsonObject object = error.toObject();
    const QJsonValue code = object.value("code"_L1);
    return {.message = object.value("message"_L1).toString(),
            .code = code.isString() ? code.toString() : object.value("type"_L1).toString()};
}

Failure AiFailures::ofServer(const Reply &reply) {
    if (reply.status == 0) {
        // A transfer timeout aborts the reply; a cancel() never gets here.
        const bool slow = reply.error == QNetworkReply::TimeoutError || reply.error == QNetworkReply::OperationCanceledError;
        return slow ? Failure(Code::AiTimedOut, {reply.where}) : Failure(Code::AiUnreachable, {reply.where, reply.errorString});
    }
    if (reply.status == 401 || reply.status == 403)
        return {reply.keySet ? Code::AiRefusedKey : Code::AiWantsKey, {reply.where}};
    const QString &message = reply.said.message;
    if (noSuchModel(reply))
        return message.isEmpty() ? Failure(Code::AiNoModel, {reply.where, reply.model})
                                 : Failure(Code::AiNoModelSaid, {reply.where, reply.model, message});
    if (overALimit(reply))
        return {Code::AiLimited, {reply.where, message.isEmpty() ? QString::number(reply.status) : message}};
    if (!message.isEmpty())
        return {Code::AiAnswered, {reply.where, message}};
    return {Code::AiError, {reply.where, QString::number(reply.status)}};
}

Failure AiFailures::ofProgram(const QString &name, const QString &model, const QString &said) {
    if (said.isEmpty())
        return {Code::AiStopped, {name}};
    if (!model.isEmpty() && said.contains("model"_L1, Qt::CaseInsensitive))
        return {Code::AiModelRefused, {name, model, said}};
    return {Code::AiProgramSaid, {name, said}};
}

bool AiFailures::aboutTheModel(const Failure &failure) {
    const Code code = failure.code();
    return code == Code::AiNoModel || code == Code::AiNoModelSaid || code == Code::AiModelRefused;
}
