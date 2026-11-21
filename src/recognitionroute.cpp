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
#include "voxtypelocal.h"
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

// The ways speech may go, as they are found, and why the others can't.
struct Speech {
    Ways ways;
    QStringList missing;
    std::optional<Refusal> refused;
};

void addServer(const Settings &settings, bool hostedAsked, Speech &speech) {
    const auto server = transcriber(settings);
    if (!server)
        return;
    const QString model = chosen(settings, "transcription_model"_L1);
    speech.refused = hostedRefusal(settings, Capability::SpeechToText, server->baseUrl, model, hostedAsked);
    if (!speech.refused)
        speech.ways.push_back(std::make_unique<Recognizers::Transcription>(*server, model));
}

// On this computer: nothing to ask of anyone. One that isn't installed is
// still tried after another way, so that its failure says what to install.
void addWhisper(const Settings &settings, Speech &speech) {
    const QString program = chosen(settings, "whisper_program"_L1).isEmpty() ? QString::fromLatin1(WhisperLocal::defaultProgram)
                                                                             : chosen(settings, "whisper_program"_L1);
    const QString modelFile =
        chosen(settings, "whisper_model"_L1).isEmpty() ? WhisperLocal::findModel() : chosen(settings, "whisper_model"_L1);
    const QString missing = WhisperLocal::missing(program, modelFile);
    if (!missing.isEmpty())
        speech.missing << missing;
    if (missing.isEmpty() || !speech.ways.empty())
        speech.ways.push_back(std::make_unique<WhisperLocal>(program, modelFile));
}

void addVoxtype(const Settings &settings, Speech &speech) {
    const QString program = RecognitionRoute::voxtypeProgram(settings);
    if (const QString missing = VoxtypeLocal::missing(program); !missing.isEmpty()) {
        speech.missing << missing;
        return;
    }
    speech.ways.push_back(std::make_unique<VoxtypeLocal>(program, chosen(settings, "voxtype_model"_L1)));
}

// Whether the person chose how speech is written out, before there was
// voxtype: a server for it, or whisper.cpp. Their choice keeps going first.
[[nodiscard]] bool chosenBefore(const Settings &settings) {
    return !chosen(settings, "transcription_url"_L1).isEmpty() || !chosen(settings, "whisper_program"_L1).isEmpty()
           || !chosen(settings, "whisper_model"_L1).isEmpty();
}

// voxtype, whisper.cpp, then the server (ADR 0008); where the person chose
// one of the others, as they always went, and voxtype after them.
// A way chosen alone on the AI page (#144) is the only one: "voxtype" or
// "whisper.cpp"; "server" puts the server first, as one chosen before did.
[[nodiscard]] std::expected<Ways, Refusal> speechPlan(const Settings &settings, bool hostedAsked) {
    Speech speech;
    const QString provider = AiPrivacy::chosen(settings, Capability::SpeechToText).value("provider"_L1).toString();
    if (provider == "voxtype"_L1) {
        addVoxtype(settings, speech);
    } else if (provider == "whisper.cpp"_L1) {
        addWhisper(settings, speech);
    } else if (provider == "server"_L1 || chosenBefore(settings)) {
        addServer(settings, hostedAsked, speech);
        addWhisper(settings, speech);
        addVoxtype(settings, speech);
    } else {
        addVoxtype(settings, speech);
        addWhisper(settings, speech);
        addServer(settings, hostedAsked, speech);
    }
    if (!speech.ways.empty())
        return std::move(speech.ways);
    const QString missing = speech.missing.join(u' ');
    return std::unexpected(speech.refused ? Refusal{.lack = speech.refused->lack, .message = speech.refused->message + u' ' + missing}
                                          : Refusal{.lack = Lack::NoServer, .message = missing});
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

QString RecognitionRoute::voxtypeProgram(const Settings &settings) {
    const QString program = chosen(settings, "voxtype_program"_L1);
    return program.isEmpty() ? QString::fromLatin1(VoxtypeLocal::defaultProgram) : program;
}

QString RecognitionRoute::signature(const Ways &ways) {
    QStringList each;
    for (const auto &way : ways)
        each << way->where() + u'\t' + way->model();
    return each.join(u'\n');
}
