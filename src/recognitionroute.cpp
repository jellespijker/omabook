#include "recognitionroute.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include "ai.h"
#include "aiprivacy.h"
#include "failure.h"
#include "recognizers.h"
#include "secretstore.h"
#include "settings.h"
#include "whisperlocal.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString chosen(const Settings &settings, QLatin1StringView key) {
    return settings.recognition().value(key).toString().trimmed();
}

// The server speech is sent to: the one named for it, else the AI server.
[[nodiscard]] std::expected<AiServer, QString> transcriber(const Settings &settings) {
    const QString url = chosen(settings, "transcription_url"_L1);
    if (url.isEmpty())
        return AiEndpoints::server(settings);
    QString base = url;
    while (base.endsWith(u'/'))
        base.chop(1);
    return AiServer{.preset = u"custom"_s,
                    .name = QUrl(base).host(),
                    .baseUrl = base,
                    .key = AiEndpoints::keptKey(base, Secrets::shared()),
                    .keyFile = {},
                    .model = {}};
}

using RecognitionRoute::Lack;
using RecognitionRoute::Refusal;
using RecognitionRoute::Ways;

[[nodiscard]] Ways inkWays(AiServer server, const QString &model) {
    Ways ways;
    ways.push_back(std::make_unique<Recognizers::Vision>(std::move(server), model));
    return ways;
}

[[nodiscard]] std::optional<Refusal> hostedRefusal(const Settings &settings, Capability capability, const QString &baseUrl,
                                                   const QString &model, bool hostedAsked) {
    const CapabilityLack refused = AiPrivacy::refusal(settings, capability, AiPrivacy::hosted(settings, baseUrl, model), hostedAsked);
    if (refused == CapabilityLack::None)
        return std::nullopt;
    const QString whose = AiEndpoints::runsElsewhere(model) ? QObject::tr("The model %1").arg(model) : QUrl(baseUrl).host();
    if (refused == CapabilityLack::HostedNotAllowed)
        return Refusal{.lack = Lack::HostedNotAllowed,
                       .message = QObject::tr("%1 is someone else's computer, and your handwriting and speech go only to your own "
                                              "(this computer, your tailnet) unless you allow hosted servers for recognition in the "
                                              "settings.")
                                      .arg(whose)};
    return Refusal{
            .lack = Lack::HostedNotChosen,
            .message =
                QObject::tr("%1 is someone else's computer: your handwriting and speech go there only where you chose it.").arg(whose)};
}

[[nodiscard]] std::expected<Ways, Refusal> inkPlan(const Settings &settings, bool hostedAsked) {
    QString model = chosen(settings, "ink_model"_L1);
    if (model.isEmpty())
        model = Ai::models(settings, AiEndpoints::modelsKey(settings)).main;
    const auto server = AiEndpoints::server(settings, model);
    if (!server)
        return std::unexpected(
            Refusal{.lack = Lack::NoServer, .message = ::Failure(::Failure::Code::NoVisionServer, {server.error()}).text()});
    if (model.isEmpty())
        model = server->model;
    if (auto refused = hostedRefusal(settings, Capability::Handwriting, server->baseUrl, model, hostedAsked))
        return std::unexpected(std::move(*refused));
    return inkWays(*server, model);
}

[[nodiscard]] std::expected<Ways, Refusal> speechPlan(const Settings &settings, bool hostedAsked) {
    Ways ways;
    std::optional<Refusal> refused;
    if (const auto server = transcriber(settings)) {
        const QString model = chosen(settings, "transcription_model"_L1);
        refused = hostedRefusal(settings, Capability::SpeechToText, server->baseUrl, model, hostedAsked);
        if (!refused)
            ways.push_back(std::make_unique<Recognizers::Transcription>(*server, model));
    }
    // On this computer: nothing to ask of anyone.
    const QString program = chosen(settings, "whisper_program"_L1).isEmpty() ? QString::fromLatin1(WhisperLocal::defaultProgram)
                                                                             : chosen(settings, "whisper_program"_L1);
    const QString modelFile =
        chosen(settings, "whisper_model"_L1).isEmpty() ? WhisperLocal::findModel() : chosen(settings, "whisper_model"_L1);
    const QString missing = WhisperLocal::missing(program, modelFile);
    if (ways.empty() && !missing.isEmpty())
        return std::unexpected(refused ? Refusal{.lack = refused->lack, .message = refused->message + u' ' + missing}
                                       : Refusal{.lack = Lack::NoServer, .message = missing});
    ways.push_back(std::make_unique<WhisperLocal>(program, modelFile));
    return ways;
}

[[nodiscard]] std::expected<Ways, Refusal> plan(const Settings &settings, Recognition::Kind kind, bool hostedAsked) {
    if (settings.aiOff())
        return std::unexpected(Refusal{.lack = Lack::AiOff, .message = ::Failure(::Failure::Code::AiOff).text()});
    return kind == Recognition::Kind::Ink ? inkPlan(settings, hostedAsked) : speechPlan(settings, hostedAsked);
}

} // namespace

QString RecognitionRoute::whyNot(const Settings &settings, const QString &baseUrl, const QString &model, bool hostedAsked) {
    const auto refused = hostedRefusal(settings, Capability::Handwriting, baseUrl, model, hostedAsked);
    return refused ? refused->message : QString();
}

std::optional<RecognitionRoute::Refusal> RecognitionRoute::refusal(const Settings &settings, Recognition::Kind kind, bool hostedAsked) {
    auto planned = plan(settings, kind, hostedAsked);
    return planned ? std::nullopt : std::optional<Refusal>(std::move(planned.error()));
}

std::expected<RecognitionRoute::Ways, QString> RecognitionRoute::ways(const Settings &settings, Recognition::Kind kind, bool hostedAsked) {
    return plan(settings, kind, hostedAsked).transform_error([](const Refusal &refused) { return refused.message; });
}

QString RecognitionRoute::signature(const Ways &ways) {
    QStringList each;
    for (const auto &way : ways)
        each << way->where() + u'\t' + way->model();
    return each.join(u'\n');
}
