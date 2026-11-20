#include "recognitionroute.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include "ai.h"
#include "recognizers.h"
#include "secretstore.h"
#include "settings.h"
#include "whisperlocal.h"

using namespace Qt::StringLiterals;

namespace {

[[nodiscard]] QString chosen(const Settings &settings, QLatin1StringView key) {
    return settings.recognition().value(key).toString().trimmed();
}

[[nodiscard]] QStringList ownServers(const Settings &settings) {
    QStringList own;
    for (const QJsonValue &address : settings.recognition().value("own"_L1).toArray())
        own << address.toString();
    return own;
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

[[nodiscard]] RecognitionRoute::Ways inkWays(AiServer server, const QString &model) {
    RecognitionRoute::Ways ways;
    ways.push_back(std::make_unique<Recognizers::Vision>(std::move(server), model));
    return ways;
}

} // namespace

QString RecognitionRoute::whyNot(const Settings &settings, const QString &baseUrl, const QString &model, bool hostedAsked) {
    const bool elsewhere = AiEndpoints::runsElsewhere(model);
    if (Recognition::reach(baseUrl, ownServers(settings)) != Recognition::Reach::Hosted && !elsewhere)
        return {};
    const QString whose = elsewhere ? QObject::tr("The model %1").arg(model) : QUrl(baseUrl).host();
    if (!settings.recognitionHosted())
        return QObject::tr("%1 is someone else's computer, and your handwriting and speech go only to your own (this computer, your "
                           "tailnet) unless you allow hosted servers for recognition in the settings.")
            .arg(whose);
    if (!hostedAsked)
        return QObject::tr("%1 is someone else's computer: your handwriting and speech go there only where you chose it.").arg(whose);
    return {};
}

std::expected<RecognitionRoute::Ways, QString> RecognitionRoute::ways(const Settings &settings, Recognition::Kind kind, bool hostedAsked) {
    if (kind == Recognition::Kind::Ink) {
        QString model = chosen(settings, "ink_model"_L1);
        if (model.isEmpty())
            model = Ai::models(settings, AiEndpoints::modelsKey(settings)).main;
        const auto server = AiEndpoints::server(settings, model);
        if (!server)
            return std::unexpected(QObject::tr("Handwriting is read by a vision model on your AI server: %1").arg(server.error()));
        if (model.isEmpty())
            model = server->model;
        if (const QString why = whyNot(settings, server->baseUrl, model, hostedAsked); !why.isEmpty())
            return std::unexpected(why);
        return inkWays(*server, model);
    }
    Ways ways;
    QString refused;
    if (const auto server = transcriber(settings)) {
        const QString model = chosen(settings, "transcription_model"_L1);
        refused = whyNot(settings, server->baseUrl, model, hostedAsked);
        if (refused.isEmpty())
            ways.push_back(std::make_unique<Recognizers::Transcription>(*server, model));
    }
    // On this computer: nothing to ask of anyone.
    const QString program = chosen(settings, "whisper_program"_L1).isEmpty() ? QString::fromLatin1(WhisperLocal::defaultProgram)
                                                                             : chosen(settings, "whisper_program"_L1);
    const QString modelFile =
        chosen(settings, "whisper_model"_L1).isEmpty() ? WhisperLocal::findModel() : chosen(settings, "whisper_model"_L1);
    const QString missing = WhisperLocal::missing(program, modelFile);
    if (ways.empty() && !missing.isEmpty())
        return std::unexpected(refused.isEmpty() ? missing : refused + u' ' + missing);
    ways.push_back(std::make_unique<WhisperLocal>(program, modelFile));
    return ways;
}
